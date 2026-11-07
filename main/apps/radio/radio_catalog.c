/*
 * sdgoods-radio · 电台目录服务实现
 *
 * 依赖：esp_http_client（拉 JSON）+ cJSON（解析）+ esp_crt_bundle（HTTPS）。
 * 全程在 PSRAM 里做缓冲，避免占用本已紧张的内部 RAM。
 *
 * v2 变更（对应 docs/UI_REDESIGN.md §8）：
 *   · 新增「城市」维度：name=<城市> 查询优先，国家列表补足；
 *   · 去重（同一 URL 只保留一条）；
 *   · 过滤解码器播不了的流（OGG/OPUS/FLAC/HLS/播放列表）；
 *   · 修正 hidebroken 参数名（旧版写成 s-hidebroken，服务端直接忽略）；
 *   · 定位同时取城市名（ip-api 带 lang=zh-CN），供 UI 显示「城市电台」。
 */

#include "radio_catalog.h"
#include "radio_station_identity.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "radio_catalog";

#define RB_BASE        "http://de1.api.radio-browser.info"   /* 明文 HTTP：少一次 TLS 握手（约省 0.4~1s）；目录是公开数据 */
#define HTTP_JSON_CAP  200000   /* Radio Browser 列表 JSON 上限（PSRAM 暂存） */
#define CITY_LIMIT     40
#define COUNTRY_LIMIT  40
#define GLOBAL_LIMIT   24

/* 目录缓存（NVS）：打开应用先出缓存、后台再刷新，避免每次进入都等 2~3 次
 * 跨境 TLS 握手。nvs 分区 840KB，64 台 ≈ 25KB，安全。 */
#define CACHE_NS       "radio_cat"
#define CACHE_KEY      "catalog"
#define CACHE_MAGIC    0x52424331u   /* "RBC1" */

typedef struct {
    uint32_t magic;
    uint32_t count;
    uint32_t saved_sec;              /* unix 秒；系统时间未同步时为 0 */
    char     city[24];
    char     country[RADIO_COUNTRY_MAX];
} radio_cache_hdr_t;

static radio_station_t *s_stations;
static int             s_count;
static char            s_country[RADIO_COUNTRY_MAX] = "";
static char            s_city[24] = "";
static bool            s_builtin;    /* 当前是内置兜底列表，不写缓存 */
static uint32_t        s_cache_sec;  /* 缓存写入时间（unix 秒） */
static SemaphoreHandle_t s_load_lock;  /* 开机预热与界面刷新串行化 */

/* 大小写不敏感子串查找（strcasestr 不可移植，自写） */
static bool contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) {
        return false;
    }
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, nl) == 0) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * 频率解析（三段置信度）：台名里找数字，优先带小数的 87.0~108.0，
 * 其次 3/4 位整数 /10。含 "fm" 提升一档置信度。返回范围内最佳频率；无则 0。
 * ------------------------------------------------------------------------- */
float radio_parse_freq(const char *name)
{
    if (!name) {
        return 0.0f;
    }
    bool has_fm = contains_ci(name, "fm") || contains_ci(name, "mhz");
    float best = 0.0f;
    int best_conf = 0;

    const char *p = name;
    while (*p) {
        if (!isdigit((unsigned char)*p)) {
            p++;
            continue;
        }
        char tok[16];
        int ti = 0;
        bool dot = false;
        while (*p && ti < 15 && (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1])))) {
            if (*p == '.') {
                dot = true;
            }
            tok[ti++] = *p++;
        }
        tok[ti] = '\0';

        float v = 0.0f;
        int conf = 0;
        if (dot) {
            v = strtof(tok, NULL);
            if (v >= 87.0f && v <= 108.0f) {
                conf = has_fm ? 3 : 2;
            }
        } else if (has_fm && (v=strtof(tok,NULL))>=87.0f && v<=108.0f) {
            conf = 3;  /* FM 89 / FM 101 are valid integer frequencies. */
        } else if (ti == 3 || ti == 4) {
            v = strtof(tok, NULL) / 10.0f;   /* 1011 -> 101.1，927 -> 92.7 */
            if (v >= 87.0f && v <= 108.0f) {
                conf = has_fm ? 2 : 1;
            }
        }
        if (conf > best_conf) {
            best_conf = conf;
            best = v;
        }
    }
    return best>0.0f?best:radio_station_known_frequency(name);
}

/* ---------------------------------------------------------------------------
 * HTTP GET 整个响应到一个 PSRAM 缓冲（调用方 free）。非 2xx / 出错返回 NULL。
 * ------------------------------------------------------------------------- */
static char *http_get_buf(const char *url, int *out_len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 6000,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    char *buf = NULL;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", url);
        esp_http_client_cleanup(c);
        return NULL;
    }
    esp_http_client_fetch_headers(c);
    int status = esp_http_client_get_status_code(c);
    if (status / 100 != 2) {
        ESP_LOGW(TAG, "GET %s -> %d", url, status);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }
    buf = heap_caps_malloc(HTTP_JSON_CAP + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGW(TAG, "json buffer alloc failed (%d)", (int)(HTTP_JSON_CAP + 1));
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return NULL;
    }
    size_t fill = 0;
    int r;
    while (fill < HTTP_JSON_CAP &&
           (r = esp_http_client_read(c, buf + fill, (int)(HTTP_JSON_CAP - fill))) > 0) {
        fill += (size_t)r;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (fill == 0) {
        heap_caps_free(buf);
        return NULL;
    }
    buf[fill] = '\0';
    if (out_len) {
        *out_len = (int)fill;
    }
    ESP_LOGI(TAG, "GET %s -> %d, %u bytes", url, status, (unsigned)fill);
    return buf;
}

/* 取字符串字段（不存在返回 def） */
static const char *jstr(const cJSON *obj, const char *key, const char *def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(it) && it->valuestring) {
        return it->valuestring;
    }
    return def;
}

static int jint(const cJSON *obj, const char *key, int def)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(it)) {
        return it->valueint;
    }
    return def;
}

/* UTF-8 百分号编码（城市名是中文，必须编码后才能进 query） */
static void url_encode(const char *in, char *out, size_t cap)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; p && *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
}

/* 解码器（esp_audio_simple_dec）只吃 MP3/AAC。宁可少列，也不要列出来播不了。
 * 只拦「明确不支持」的：OGG/OPUS/FLAC/HLS 与播放列表地址。
 * codec 字段大量为空或 UNKNOWN，不能反过来白名单，否则会砍掉一半可用台。 */
static bool stream_playable(const char *codec, const char *url)
{
    if (!url || !url[0]) {
        return false;
    }
    if (contains_ci(url, ".m3u8") || contains_ci(url, ".m3u") || contains_ci(url, ".pls") ||
        contains_ci(url, ".ogg") || contains_ci(url, "opus") || contains_ci(url, ".flac")) {
        return false;
    }
    if (!codec || !codec[0]) {
        return true;
    }
    if (contains_ci(codec, "ogg") || contains_ci(codec, "opus") || contains_ci(codec, "flac") ||
        contains_ci(codec, "hls") || contains_ci(codec, "dash")) {
        return false;
    }
    return true;
}

static bool already_have(const char *url)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_stations[i].url, url) == 0) {
            return true;
        }
    }
    return false;
}

/* 把一电台对象追加进 s_stations；返回是否成功加入 */
static bool append_station(const cJSON *item)
{
    if (s_count >= RADIO_CATALOG_MAX) {
        return false;
    }
    const char *url = jstr(item, "url_resolved", jstr(item, "url", NULL));
    const char *name = jstr(item, "name", NULL);
    const char *codec = jstr(item, "codec", "");
    if (!url || url[0] == '\0' || !name || !name[0]) {
        return false;
    }
    if (!stream_playable(codec, url) || jint(item,"hls",0) || already_have(url) || strlen(url)>=RADIO_URL_MAX) {
        return false;
    }
    radio_station_t *st = &s_stations[s_count];
    memset(st, 0, sizeof(*st));
    radio_station_copy_utf8(name,st->name,sizeof(st->name));
    strncpy(st->url, url, RADIO_URL_MAX - 1);
    strncpy(st->codec, codec, RADIO_CODEC_MAX - 1);
    strncpy(st->country, jstr(item, "countrycode", ""), RADIO_COUNTRY_MAX - 1);
    strncpy(st->tag, jstr(item, "tags", ""), RADIO_TAG_MAX - 1);
    st->bitrate = (uint32_t)jint(item, "bitrate", 0);
    st->votes = jint(item, "votes", 0);
    /* Parse before truncating the display buffer: FM may be at the end. */
    st->freq_mhz = radio_parse_freq(name);
    if(st->freq_mhz<=0.0f){
        const char *tags=jstr(item,"tags","");
        if(contains_ci(tags,"fm") || contains_ci(tags,"mhz"))st->freq_mhz=radio_parse_freq(tags);
    }
    s_count++;
    return true;
}

/* 解析 Radio Browser 返回的 JSON 数组，追加到列表 */
static void parse_station_array(char *json)
{
    cJSON *root = cJSON_Parse(json);
    heap_caps_free(json);
    if (!cJSON_IsArray(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return;
    }
    int n = cJSON_GetArraySize(root);
    for (int i = 0; i < n && s_count < RADIO_CATALOG_MAX; i++) {
        append_station(cJSON_GetArrayItem(root, i));
    }
    cJSON_Delete(root);
}

/* 通用搜索：query 由调用方拼好（不含前导 '?'）。返回新增数量。 */
static int fetch_search(const char *query)
{
    char url[320];
    snprintf(url, sizeof(url), "%s/json/stations/search?%s", RB_BASE, query);
    int len = 0;
    char *json = http_get_buf(url, &len);
    if (!json) {
        ESP_LOGW(TAG, "fetch failed: %s", query);
        return 0;
    }
    int before = s_count;
    parse_station_array(json);   /* 内部已 free json */
    ESP_LOGI(TAG, "fetch %s -> +%d", query, s_count - before);
    return s_count - before;
}

static int fetch_city(const char *city_zh, int limit)
{
    char enc[64];
    char query[192];
    url_encode(city_zh, enc, sizeof(enc));
    snprintf(query, sizeof(query),
             "name=%s&order=votes&reverse=true&hidebroken=true&offset=0&limit=%d",
             enc, limit);
    return fetch_search(query);
}

static int fetch_country(const char *cc, int limit)
{
    char query[160];
    snprintf(query, sizeof(query),
             "countrycode=%s&order=votes&reverse=true&hidebroken=true&offset=0&limit=%d",
             cc, limit);
    return fetch_search(query);
}

static int fetch_global(int limit)
{
    char query[128];
    snprintf(query, sizeof(query),
             "order=votes&reverse=true&hidebroken=true&offset=0&limit=%d", limit);
    return fetch_search(query);
}

/* 定位只走 https://ipwho.is/：原来的 ip-api.com 是明文 HTTP，境内多数网络
 * 直接丢包，要白等 10s 超时才回退，是进入应用"特别慢"的最大单点。 */
static bool fetch_place(char *cc, size_t cclen, char *city, size_t citylen)
{
    int len = 0;
    char *json = http_get_buf("https://ipwho.is/", &len);
    if (json) {
        cJSON *root = cJSON_Parse(json);
        heap_caps_free(json);
        const char *code = root ? jstr(root, "country_code", NULL) : NULL;
        const char *ct = root ? jstr(root, "city", NULL) : NULL;
        if (code && strlen(code) == 2) {
            strncpy(cc, code, cclen - 1);
            cc[cclen - 1] = '\0';
            if (ct) {
                strncpy(city, ct, citylen - 1);
                city[citylen - 1] = '\0';
            }
            cJSON_Delete(root);
            return true;
        }
        if (root) {
            cJSON_Delete(root);
        }
    }
    return false;
}

/* 内置国内电台：全部是 64kbps MP3「连续流」（单条 HTTP 长连接，无分片/无 m3u8）。
 * 之前用央广 HLS 有两个硬伤：① 一片 250KB 每 10s 拉一次，WiFi 抖动就出现断音；
 * ② ngcdn 上十几个频道直接 502/403（听不了）。这组地址都实测过 200 + audio，
 * 首字节 0.1~0.5s、64kbps 只占 8KB/s：环形缓冲 512KB ≈ 64s 垫底，非常稳。
 * 目录同步装载，不联网。 */
static void load_domestic(void)
{
    static const struct {
        const char *name;
        const char *url;
    } cn[] = {
        {"中国之声",       "http://lhttp.qtfm.cn/live/15318317/64k.mp3"},
        {"上海新闻广播",   "http://lhttp.qtfm.cn/live/270/64k.mp3"},
        {"上海经典音乐广播", "http://lhttp.qtfm.cn/live/267/64k.mp3"},
        {"上海金曲广播",   "http://lhttp.qtfm.cn/live/273/64k.mp3"},
        {"上海动感101",    "http://lhttp.qtfm.cn/live/274/64k.mp3"},
        {"第一财经广播",   "http://lhttp.qtfm.cn/live/276/64k.mp3"},
        {"上海长三角之声", "http://lhttp.qtfm.cn/live/275/64k.mp3"},
        {"北京新闻广播",   "http://lhttp.qtfm.cn/live/339/64k.mp3"},
        {"北京交通广播",   "http://lhttp.qtfm.cn/live/336/64k.mp3"},
        {"北京音乐广播",   "http://lhttp.qtfm.cn/live/332/64k.mp3"},
        {"北京文艺广播",   "http://lhttp.qtfm.cn/live/333/64k.mp3"},
        {"广东音乐之声",   "http://lhttp.qtfm.cn/live/1260/64k.mp3"},
        {"广东珠江经济台", "http://lhttp.qtfm.cn/live/1259/64k.mp3"},
        {"羊城交通电台",   "http://lhttp.qtfm.cn/live/1262/64k.mp3"},
        {"深圳新闻广播",   "http://lhttp.qtfm.cn/live/1270/64k.mp3"},
        {"江苏经典流行音乐", "http://lhttp.qtfm.cn/live/4938/64k.mp3"},
        {"四川新闻广播",   "http://lhttp.qtfm.cn/live/4906/64k.mp3"},
        {"安徽音乐广播",   "http://lhttp.qtfm.cn/live/1947/64k.mp3"},
        {"安徽小说评书广播", "http://lhttp.qtfm.cn/live/1951/64k.mp3"},
        {"苏州都市音乐广播", "http://lhttp.qtfm.cn/live/2803/64k.mp3"},
        {"长沙音乐广播",   "http://lhttp.qtfm.cn/live/20847/64k.mp3"},
        {"清晨音乐台",     "http://lhttp.qtfm.cn/live/4915/64k.mp3"},
        {"云梦音乐台",     "http://lhttp.qtfm.cn/live/20500187/64k.mp3"},
        {"听越剧",         "http://lhttp.qtfm.cn/live/20500178/64k.mp3"},
    };
    for (size_t i = 0; i < sizeof(cn) / sizeof(cn[0]) && s_count < RADIO_CATALOG_MAX; i++) {
        radio_station_t *st = &s_stations[s_count];
        memset(st, 0, sizeof(*st));
        snprintf(st->name, RADIO_NAME_MAX, "%s", cn[i].name);
        snprintf(st->url, RADIO_URL_MAX, "%s", cn[i].url);
        snprintf(st->codec, RADIO_CODEC_MAX, "%s", "MP3");
        st->freq_mhz = radio_parse_freq(st->name);
        s_count++;
    }
    snprintf(s_city, sizeof(s_city), "国内电台");
    s_builtin = true;
}

void radio_catalog_seed_domestic(void)
{
    if (!s_stations) {
        s_stations = heap_caps_calloc(RADIO_CATALOG_MAX, sizeof(*s_stations),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_stations) {
            ESP_LOGE(TAG, "no PSRAM for station catalog");
            return;
        }
    }
    if (s_count > 0) {
        return;
    }
    load_domestic();
    ESP_LOGI(TAG, "seeded %d domestic stations", s_count);
}

/* 内置兜底精选台（网络全失败时保证可用）。url 用公开稳定的电台流。 */
static void load_fallback(void)
{
    struct {
        const char *name;
        const char *url;
        const char *codec;
    } fb[] = {
        {"Radio Paradise Main",   "https://stream.radioparadise.com/mp3-192",   "MP3"},
        {"Radio Paradise Mellow", "https://stream.radioparadise.com/mellow-192", "MP3"},
        {"SomaFM Groove Salad",   "https://ice1.somafm.com/groovesalad-128-mp3", "MP3"},
    };
    for (size_t i = 0; i < sizeof(fb) / sizeof(fb[0]); i++) {
        if (s_count >= RADIO_CATALOG_MAX) {
            break;
        }
        radio_station_t *st = &s_stations[s_count];
        memset(st, 0, sizeof(*st));
        strncpy(st->name, fb[i].name, RADIO_NAME_MAX - 1);
        strncpy(st->url, fb[i].url, RADIO_URL_MAX - 1);
        strncpy(st->codec, fb[i].codec, RADIO_CODEC_MAX - 1);
        st->freq_mhz = radio_parse_freq(st->name);
        s_count++;
    }
    snprintf(s_city, sizeof(s_city), "预设电台");
    s_builtin = true;
    ESP_LOGW(TAG, "using built-in fallback list (%d stations)", s_count);
}

/* ---------------------------------------------------------------------------
 * 目录缓存
 * ------------------------------------------------------------------------- */
static void catalog_cache_save(void)
{
    if (s_count <= 0 || s_builtin) return;
    size_t payload = sizeof(radio_station_t) * (size_t)s_count;
    size_t len = sizeof(radio_cache_hdr_t) + payload;
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;
    radio_cache_hdr_t *hdr = (radio_cache_hdr_t *)buf;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = CACHE_MAGIC;
    hdr->count = (uint32_t)s_count;
    time_t now = time(NULL);
    hdr->saved_sec = now > 1600000000 ? (uint32_t)now : 0;
    snprintf(hdr->city, sizeof(hdr->city), "%s", s_city);
    snprintf(hdr->country, sizeof(hdr->country), "%s", s_country);
    memcpy(buf + sizeof(*hdr), s_stations, payload);

    nvs_handle_t h;
    if (nvs_open(CACHE_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_blob(h, CACHE_KEY, buf, len) == ESP_OK) {
            nvs_commit(h);
            s_cache_sec = hdr->saved_sec;
        } else {
            ESP_LOGW(TAG, "catalog cache write failed");
        }
        nvs_close(h);
    }
    heap_caps_free(buf);
}

bool radio_catalog_load_cache(void)
{
    if (!s_stations) {
        s_stations = heap_caps_calloc(RADIO_CATALOG_MAX, sizeof(*s_stations),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_stations) return false;
    }
    nvs_handle_t h;
    if (nvs_open(CACHE_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    if (nvs_get_blob(h, CACHE_KEY, NULL, &len) != ESP_OK ||
        len < sizeof(radio_cache_hdr_t) + sizeof(radio_station_t)) {
        nvs_close(h);
        return false;
    }
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        nvs_close(h);
        return false;
    }
    if (nvs_get_blob(h, CACHE_KEY, buf, &len) != ESP_OK) {
        heap_caps_free(buf);
        nvs_close(h);
        return false;
    }
    nvs_close(h);

    radio_cache_hdr_t *hdr = (radio_cache_hdr_t *)buf;
    bool ok = hdr->magic == CACHE_MAGIC && hdr->count > 0 &&
              hdr->count <= RADIO_CATALOG_MAX &&
              len >= sizeof(*hdr) + (size_t)hdr->count * sizeof(radio_station_t);
    if (ok) {
        memcpy(s_stations, buf + sizeof(*hdr),
               (size_t)hdr->count * sizeof(radio_station_t));
        s_count = (int)hdr->count;
        snprintf(s_city, sizeof(s_city), "%s", hdr->city);
        snprintf(s_country, sizeof(s_country), "%s", hdr->country);
        s_cache_sec = hdr->saved_sec;
        s_builtin = false;
        ESP_LOGI(TAG, "catalog cache: %d stations (city=%s)", s_count, s_city);
    }
    heap_caps_free(buf);
    return ok;
}

int radio_catalog_cache_age(void)
{
    if (s_cache_sec == 0) return -1;
    time_t now = time(NULL);
    if (now <= 1600000000) return -1;
    long age = (long)now - (long)s_cache_sec;
    return age < 0 ? 0 : (int)age;
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */
static int catalog_load_impl(const char *city_zh)
{
    if(!s_stations)s_stations=heap_caps_calloc(RADIO_CATALOG_MAX,sizeof(*s_stations),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s_stations){ESP_LOGE(TAG,"no PSRAM for station catalog");return 0;}
    char cached_city[24];
    snprintf(cached_city, sizeof(cached_city), "%s", s_city);
    char cc[RADIO_COUNTRY_MAX] = "";
    snprintf(cc,sizeof(cc),"%s",s_country);
    s_count = 0;
    s_builtin = false;
    s_country[0] = '\0';
    s_city[0] = '\0';
    /* 刷新时也先放内置国内台：联网结果只往后追加，失败也不会把列表清空。 */
    load_domestic();

    char auto_city[24] = "";
    /* 已有缓存的国家码就不再定位（定位要一次跨境 HTTPS）。 */
    if ((!city_zh || !city_zh[0]) && !cc[0] &&
        fetch_place(cc, sizeof(cc), auto_city, sizeof(auto_city))) {
        strncpy(s_country, cc, sizeof(s_country) - 1);
        ESP_LOGI(TAG, "located country=%s city=%s", cc, auto_city);
    }
    snprintf(s_country,sizeof(s_country),"%s",cc);

    const char *want = (city_zh && city_zh[0]) ? city_zh
                     : (auto_city[0] ? auto_city
                     : (cached_city[0] ? cached_city : NULL));
    if (want) {
        snprintf(s_city, sizeof(s_city), "%s", want);
        fetch_city(want, CITY_LIMIT);
    }
    /* 城市列表已够用就不再补国家/全球请求：每次多一个跨境 TLS 握手，
     * 是进入应用后一直转圈的主要原因之一。 */
    if (s_count < 8 && cc[0]) {
        fetch_country(cc, COUNTRY_LIMIT);
    }
    if (s_count < 8) {
        fetch_global(GLOBAL_LIMIT);
    }
    if (s_count == 0) {
        /* 刷新失败（断网/超时）：恢复上次缓存，别用 3 台兜底把好列表顶掉。 */
        if (!radio_catalog_load_cache()) {
            load_fallback();
        }
    } else {
        ESP_LOGI(TAG, "catalog loaded: %d stations (city=%s)", s_count, s_city);
        catalog_cache_save();
    }
    return s_count;
}

int radio_catalog_load(const char *city_zh)
{
    /* 开机预热任务与界面刷新可能同时调用：串行化，避免两个解析同时改 s_count。 */
    if (!s_load_lock) {
        s_load_lock = xSemaphoreCreateMutex();
    }
    if (s_load_lock) {
        xSemaphoreTake(s_load_lock, portMAX_DELAY);
    }
    int count = catalog_load_impl(city_zh);
    if (s_load_lock) {
        xSemaphoreGive(s_load_lock);
    }
    return count;
}

int radio_catalog_count(void)
{
    return s_count;
}

const radio_station_t *radio_catalog_at(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return NULL;
    }
    return &s_stations[idx];
}

const radio_station_t *radio_catalog_next(int idx)
{
    if (s_count == 0) {
        return NULL;
    }
    int n = idx + 1;
    if (n >= s_count) {
        n = 0;
    }
    if (n < 0) {
        n = s_count - 1;
    }
    return &s_stations[n];
}

const char *radio_catalog_country(void)
{
    return s_country;
}

const char *radio_catalog_city(void)
{
    return s_city;
}
