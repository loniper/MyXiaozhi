/*
 * sdgoods-radio · 网络电台播放器实现
 *
 * 数据流：esp_http_client 拉流 → esp_audio_simple_dec（MP3/AAC）解码
 *         → sdgoods_audio_stream_start/write/stop（平台层流式 API）输出。
 *
 * 并发模型：单一常驻播放任务；play/stop 通过 s_seq 代际 + s_wake 信号交命令。
 *   任何时刻只有一个会话在跑，天然避免两路会话争抢 I2S0（无需给 stream 再加锁）。
 *   换台 = s_seq++ 并唤醒；当前会话在下一次循环点检测到 s_seq 变化即自行收尾退出。
 */

#include "radio_player.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>   /* strcasecmp：HTTP 头名大小写不敏感 */
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps：任务栈放 PSRAM */
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"   /* esp_audio_simple_dec_register_default：开机注册 MP3/AAC 等解码器 */

#include "sdgoods_audio.h"

static const char *TAG = "radio_player";

/* ---- 容量 / 策略常量 ---- */
#define RP_RAW_CAP      16384            /* 解码用线性 staging（从环形缓冲取一段喂解码器，PSRAM） */
#define RP_RING_CAP    524288            /* 网络环形缓冲（PSRAM）：生产者灌/消费者取。
                                          * 央广 HLS 是 200kbps（~25KB/s），512KB ≈ 20s 垫底：
                                          * 覆盖「整段 252KB 分片下载 + 播放列表轮询」的空窗，
                                          * 单个分片偶发 403/超时重试也不会把缓冲抽干。 */
#define RP_READ_CHUNK   4096             /* 生产者单次 http read 字节 */
#define RP_NET_STACK     8192            /* 生产者任务栈（http + tmp 读缓冲） */
#define RP_NET_PRIO        4             /* 生产者优先级：低于 LVGL(5)，保证滑动/页面永远先响应；
                                          * 大环垫底，拉流慢一点也不影响出声 */
#define RP_PREBUFFER    8192             /* 开播前环形缓冲攒这么多：128kbps 流 ≈ 0.5s 垫底，
                                          * 既留出抗抖动空间，又保持“按键即出声” */
#define RP_PREBUFFER_SOFT     2048       /* 快速起播门槛：攒到这么多就够解出前几帧 */
#define RP_PREBUFFER_SOFT_MS   350       /* 且已等这么久（TTFB 慢的台也能提前起播） */
#define RP_PREBUFFER_MAX_MS 1200         /* 预缓冲最长等待：涓流台超时后带已有数据直接开播，防无限“缓冲中” */
#define RP_OUT_CAP       8192            /* 单帧解码输出上限（MP3 立体 ~4.6KB） */
#define RP_PROBE_BYTES  16384            /* 缓冲这么多字节仍解不出 -> 判编码不符 */
#define RP_MAX_RETRY     6               /* 同一台断连重连次数，超出置 ERROR 换下一台（死台快跳）。
                                          * 该 CDN 实测约 1/6 概率回 502（且要 5s 才回），
                                          * 只给 2~3 次重连会把这种偶发故障直接放大成「连接失败」。 */
#define RP_HTTP_CONNECT_TIMEOUT 5000     /* 建连 / TLS 握手 / 等响应头：跨境 CDN 要给足，
                                          * 压到 3s 会把慢台误判成死台 */
#define RP_HTTP_READ_TIMEOUT     600     /* 连上之后单次 read 超时（超时返回 EAGAIN，不当断流）。
                                          * 切台时要等旧 net 任务从 read 里退出，
                                          * 这个值直接等于切台最坏白等时长，压小收益最大 */
#define RP_NET_IDLE_MAX_MS 15000         /* 连续这么久一个字节都没读到才判死流：
                                          * 电台切歌/服务器缓冲常有几秒空隙，不能当断流 */
#define RP_MAX_REDIRECT   3              /* 30x 最多跟几跳（CDN 常见 1~2 跳） */
#define RP_FIRST_FRAME_MAX_MS 6000       /* 进 BUFFERING 后这么久还没解出第一帧 -> 判死台
                                          * （涓流数据 / 只发头不发体的流会让 probe 永远
                                          *  涨不到阈值，UI 就卡在“缓冲中”很久） */
#define RP_URL_CAP       256
#define RP_CODEC_CAP     16
#define RP_TASK_STACK    8192
#define RP_TASK_PRIO     6               /* 高于 LVGL(5) 保证音频连续；双核 S3 */
#define RP_CONNECT_MAX_MS 20000
#define RP_STARTUP_MAX_MS 30000
#define RP_STABLE_PLAY_MS 3000           /* 稳定播放超过这么久就认为「这个台是活的」：
                                          * 之后断连只当作网络抖动，重连计数清零、无限重连。
                                          * 原来是 10s —— 首次打开播几秒就被 CDN 掐断时，
                                          * 会被当成「连不上」，2 次重连后直接报连接失败。 */

/* ★ 毫秒延时必须至少折算 1 tick。
 *   CONFIG_FREERTOS_HZ=100 ⇒ 1 tick = 10ms，pdMS_TO_TICKS(5) 算出来是 0，
 *   而 vTaskDelay(0) 不是「让出 1 tick」而是立即返回 —— 优先级 6 的任务在
 *   这个循环里会空转，饿死同核 IDLE1，5 秒后触发任务看门狗；
 *   更要命的是 rp_net_shutdown 的等待循环会变成死循环（生产者同在 core 1、
 *   优先级更低，永远抢不到 CPU，s_net_alive 永不落地）⇒ 卡在「连接中」。 */
static void rp_delay_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

/* 会话结束原因 */
enum {
    RP_END_SUPERSEDED = 0,   /* 被新的 play/stop 取代 */
    RP_END_DISCONNECT,       /* 连接断开 / 服务端关闭 */
    RP_END_HTTP_FAIL,        /* 打开失败 / 状态码非 2xx */
    RP_END_NO_DATA,          /* 缓冲够多仍解不出：疑似编码不符 */
};

/* ---- 状态 ---- */
static TaskHandle_t              s_task;
static SemaphoreHandle_t         s_wake;
static SemaphoreHandle_t         s_mu;      /* 保护 s_url/s_codec/s_stop */
static char                      s_url[RP_URL_CAP];
static char                      s_codec[RP_CODEC_CAP];
static volatile uint32_t         s_seq;      /* 代际：每条 play/stop 命令 +1 */
static volatile bool             s_stop;     /* 最新命令是否为停止 */
static volatile radio_player_state_t s_state = RP_IDLE;
static volatile bool             s_paused;
static uint32_t                  s_rate;
static uint8_t                   s_ch;
static volatile uint8_t          s_levels[RADIO_PLAYER_LEVELS];
static bool                      s_dec_registered = false;   /* 解码器只注册一次 */

/* ---- 网络生产者 ⇄ 解码消费者 之间的环形缓冲 ---- */
static uint8_t              *s_ring;                 /* PSRAM，RP_RING_CAP */
static SemaphoreHandle_t     s_ring_mu;              /* 保护 head/tail/len */
static uint32_t              s_head, s_tail, s_len;  /* 写位/读位/有效字节 */
static volatile bool         s_net_eof;              /* 生产者已结束（任何原因） */
static volatile bool         s_net_stop;             /* 消费者请求生产者停下 */
static volatile bool         s_net_alive;            /* 生产者任务在跑 */
static volatile bool         s_net_connected;
static volatile int          s_net_reason;           /* 生产者开流失败原因 RP_END_* */
static volatile uint32_t     s_net_seq;              /* 生产者所属会话代际 */
static char                  s_net_url[RP_URL_CAP];  /* 传给生产者的 url 快照 */

/* Publish only for the current command. An old decoder may finish after a tap. */
static void rp_set_state(uint32_t seq, radio_player_state_t state)
{
    xSemaphoreTake(s_mu, portMAX_DELAY);
    if (s_seq == seq) s_state = state;
    xSemaphoreGive(s_mu);
}

/* ---------------------------------------------------------------------------
 * 电平表：把一段 PCM 按帧切成 RADIO_PLAYER_LEVELS 个窗口，取峰值 → 对数刻度 0~255。
 *   仅作圆屏动画的近似频谱（非真实 FFT），快攻慢泄让条子跳动自然。
 * ------------------------------------------------------------------------- */
static void rp_update_levels(const uint8_t *pcm, uint32_t bytes, uint8_t ch)
{
    const int16_t *s = (const int16_t *)pcm;
    uint32_t samples = bytes / sizeof(int16_t);
    if (samples == 0 || ch == 0) {
        return;
    }
    uint32_t frames = samples / ch;
    uint32_t per = frames / RADIO_PLAYER_LEVELS;
    if (per == 0) {
        per = 1;
    }
    for (int b = 0; b < RADIO_PLAYER_LEVELS; b++) {
        uint32_t start = (uint32_t)b * per;
        uint32_t end = start + per;
        if (end > frames) {
            end = frames;
        }
        int32_t peak = 0;
        for (uint32_t i = start; i < end; i++) {
            int32_t v = (ch == 1) ? s[i] : s[i * 2];
            int32_t a = v < 0 ? -v : v;
            if (a > peak) {
                peak = a;
            }
        }
        uint8_t lvl = 0;
        if (peak > 0) {
            /* log10(peak)/log10(32768) * 255，映射到 0~255 */
            float norm = log10f((float)peak) / 4.5155f;   /* log10(32768)=4.5155 */
            int v = (int)(norm * 255.0f);
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            lvl = (uint8_t)v;
        }
        /* 平滑：升快降慢 */
        uint8_t prev = s_levels[b];
        s_levels[b] = (lvl >= prev) ? lvl : (uint8_t)((int)prev * 3 / 4);
    }
}

/* ---------------------------------------------------------------------------
 * 打开解码器：is_aac 选 AAC，否则 MP3；use_frame_dec=false 让内部 parser 处理任意大小输入。
 * ------------------------------------------------------------------------- */
static esp_audio_err_t rp_open_decoder(esp_audio_simple_dec_handle_t *dec, bool is_aac)
{
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = is_aac ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    return esp_audio_simple_dec_open(&cfg, dec);
}

/* ---------------------------------------------------------------------------
 * 环形缓冲原语（互斥保护，跨任务安全）。写满/读空各自截断到可用量。
 * ------------------------------------------------------------------------- */
static void ring_reset(void)
{
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    s_head = s_tail = s_len = 0;
    xSemaphoreGive(s_ring_mu);
}

static uint32_t ring_len(void)
{
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    uint32_t l = s_len;
    xSemaphoreGive(s_ring_mu);
    return l;
}

/* 写入至多 n 字节，返回实际写入量（环形缓冲满时为 0 或少于 n） */
static uint32_t ring_write(const uint8_t *src, uint32_t n)
{
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    uint32_t freeb = RP_RING_CAP - s_len;
    if (n > freeb) n = freeb;
    uint32_t first = RP_RING_CAP - s_head;
    if (first > n) first = n;
    memcpy(s_ring + s_head, src, first);
    if (n - first) memcpy(s_ring, src + first, n - first);
    s_head = (s_head + n) % RP_RING_CAP;
    s_len += n;
    xSemaphoreGive(s_ring_mu);
    return n;
}

/* 读出至多 n 字节到 dst（线性化，供解码器使用），返回实际读出量 */
static uint32_t ring_read(uint8_t *dst, uint32_t n)
{
    xSemaphoreTake(s_ring_mu, portMAX_DELAY);
    if (n > s_len) n = s_len;
    uint32_t first = RP_RING_CAP - s_tail;
    if (first > n) first = n;
    memcpy(dst, s_ring + s_tail, first);
    if (n - first) memcpy(dst + first, s_ring, n - first);
    s_tail = (s_tail + n) % RP_RING_CAP;
    s_len -= n;
    xSemaphoreGive(s_ring_mu);
    return n;
}

/* ---------------------------------------------------------------------------
 * 网络生产者任务：独占 esp_http_client，把裸音频流持续灌进环形缓冲。
 *   与解码/播放解耦——即使消费者正阻塞在 i2s 写，网络仍在后台灌数据，
 *   大环形缓冲吸收网络抖动。codec 无关（只搬字节），编解码判定全在消费者。
 *   结束条件：代际变化 / s_net_stop / 读返回<=0。收尾必关 http 并置 eof/alive。
 * ------------------------------------------------------------------------- */
/* ★ 为什么净网任务不能用普通 xTaskCreatePinnedToCore：
 *   Wi-Fi/BT/LVGL 把内部 RAM 吃到只剩十几 KB，8KB 栈+TCB 在内部分区里拿不到
 *   连续块 → 返回 pdFAIL，于是每次都“开流失败 -> 重连 -> 放弃”，所有台都放不出声。
 *   本项目开了 CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY，统一用 WithCaps 把栈放 PSRAM。 */
#define RP_NET_CAPS     (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static void rp_task_exit(void)
{
    vTaskDeleteWithCaps(NULL);
}

/* 302 的 Location 只能从事件回调里拿：`esp_http_client_get_header()` 读的是
 * **请求**头（client->request->headers），响应头不在那里。只有一个生产者任务，
 * 回调又是同步派发的，所以用静态缓冲足够安全。 */
static char s_redirect_loc[RP_URL_CAP];

static esp_err_t rp_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Location") == 0) {
        strlcpy(s_redirect_loc, evt->header_value, sizeof(s_redirect_loc));
    }
    return ESP_OK;
}

/* 把服务端给的 Location 转成绝对 URL：CDN 通常直接给绝对地址，
 * 少数给 "/path"，此时沿用当前 URL 的 scheme://host。 */
static void rp_abs_url(const char *base, const char *loc, char *out, size_t cap)
{
    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
        strlcpy(out, loc, cap);
        return;
    }
    const char *p = strstr(base, "://");
    const char *slash = p ? strchr(p + 3, '/') : NULL;
    size_t host_len = slash ? (size_t)(slash - base) : strlen(base);
    if (host_len >= cap) {
        host_len = cap - 1;
    }
    memcpy(out, base, host_len);
    out[host_len] = '\0';
    strlcat(out, loc, cap);
}

/* ---------------------------------------------------------------------------
 * HLS（m3u8 + MPEG-TS）：国内电台（央广等）只提供 HLS 音频。
 *   拉 m3u8 → 只播“已完整”的分片（跳过播放列表最后一条进行中的分片）
 *   → 解 TS（PAT/PMT/PES）→ 把 ADTS ES 写进同一条环形缓冲，
 *   后面的解码/I2S 路径完全复用，播放器其余逻辑不用改。
 * ------------------------------------------------------------------------- */
#define HLS_PL_CAP      8192
#define HLS_SEG_CAP     (512 * 1024)
#define HLS_MAX_SEG     48
#define HLS_HTTP_TO     3000
#define HLS_MAX_FAIL    3

typedef struct {
    char    uri[RP_URL_CAP];
    float   dur;
    int64_t seq;
} hls_seg_t;

typedef struct {
    hls_seg_t seg[HLS_MAX_SEG];
    int       count;
    float     target;
    int64_t   media_seq;
    bool      endlist;
    bool      master;
    char      variant[RP_URL_CAP];
    int       variant_bw;
} hls_pl_t;

typedef struct { int pmt_pid; int audio_pid; } hls_ts_t;

static bool rp_is_hls(const char *url)
{
    return url != NULL && strstr(url, ".m3u8") != NULL;
}

/* 相对地址解析：绝对地址原样；"/x" 取 scheme://host；其余取 base 所在目录。 */
static void hls_join(const char *base, const char *rel, char *out, size_t cap)
{
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        strlcpy(out, rel, cap);
        return;
    }
    const char *p = strstr(base, "://");
    if (rel[0] == '/') {
        const char *slash = p ? strchr(p + 3, '/') : NULL;
        size_t n = slash ? (size_t)(slash - base) : strlen(base);
        if (n >= cap) n = cap - 1;
        memcpy(out, base, n);
        out[n] = '\0';
        strlcat(out, rel, cap);
        return;
    }
    const char *slash = strrchr(base, '/');
    size_t n = slash ? (size_t)(slash - base) + 1 : strlen(base);
    if (n >= cap) n = cap - 1;
    memcpy(out, base, n);
    out[n] = '\0';
    strlcat(out, rel, cap);
}

/* GET 到内存（m3u8/分片都很小；分片约 250KB）。返回 0 成功。 */
static int hls_get(const char *url, uint8_t *buf, int cap, int *out_len, int *out_status)
{
    *out_len = 0;
    *out_status = 0;
    char cur[RP_URL_CAP];
    strlcpy(cur, url, sizeof(cur));

    esp_http_client_config_t cfg = {0};
    cfg.url = cur;
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = HLS_HTTP_TO;
    cfg.buffer_size = 1024;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = false;
    cfg.event_handler = rp_http_event;

    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) return -1;
    esp_http_client_set_header(http, "User-Agent", "SDGOODS-Radio/1.0");

    int status = 0;
    for (int hop = 0; hop <= RP_MAX_REDIRECT; hop++) {
        s_redirect_loc[0] = '\0';
        if (esp_http_client_open(http, 0) != ESP_OK) {
            esp_http_client_cleanup(http);
            return -1;
        }
        esp_http_client_fetch_headers(http);
        status = esp_http_client_get_status_code(http);
        if (status / 100 != 3 || s_redirect_loc[0] == '\0') break;
        char next[RP_URL_CAP];
        hls_join(cur, s_redirect_loc, next, sizeof(next));
        esp_http_client_close(http);
        esp_http_client_reset_redirect_counter(http);
        if (esp_http_client_set_url(http, next) != ESP_OK) break;
        strlcpy(cur, next, sizeof(cur));
    }
    *out_status = status;
    if (status / 100 != 2) {
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        return -1;
    }

    int total = 0;
    TickType_t t0 = xTaskGetTickCount();
    while (total < cap) {
        int r = esp_http_client_read(http, (char *)buf + total, cap - total);
        if (r == -ESP_ERR_HTTP_EAGAIN) {
            if ((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS >= 8000) break;
            rp_delay_ms(5);
            continue;
        }
        if (r <= 0) break;
        total += r;
    }
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    *out_len = total;
    return 0;
}

/* 写 ES 到环形缓冲；缓冲满时等消费者腾空间，期间仍响应停止/换台。 */
static bool hls_emit(const uint8_t *d, int n, uint32_t sess)
{
    int off = 0;
    while (off < n) {
        if (s_seq != sess || s_net_stop) return false;
        uint32_t w = ring_write(d + off, (uint32_t)(n - off));
        off += (int)w;
        if (w == 0) rp_delay_ms(5);
    }
    return true;
}

static bool hls_demux(const uint8_t *seg, int len, hls_ts_t *ts, uint32_t sess)
{
    for (int i = 0; i + 188 <= len; ) {
        if (seg[i] != 0x47) { i++; continue; }
        const uint8_t *p = seg + i;
        int pid = ((p[1] & 0x1f) << 8) | p[2];
        bool pusi = (p[1] & 0x40) != 0;
        int afc = (p[3] >> 4) & 3;
        int off = 4;
        if (afc == 3) {
            off += 1 + p[4];
        } else if (afc == 2) {
            i += 188;   /* 纯适配字段，无有效载荷 */
            continue;
        }
        if (off >= 187) { i += 188; continue; }

        if (pusi && pid == 0) {                      /* PAT → PMT PID */
            int ptr = p[off];
            int sh = off + 1 + ptr;
            int avail = 188 - sh;
            const uint8_t *sec = p + sh;
            if (avail >= 12 && sec[0] == 0x00) {
                int sl = ((sec[1] & 0x0f) << 8) | sec[2];
                if (avail >= 3 + sl) {
                    ts->pmt_pid = ((sec[10] & 0x1f) << 8) | sec[11];
                }
            }
        } else if (pusi && ts->pmt_pid != 0 && pid == ts->pmt_pid) {   /* PMT → 音频 PID */
            int ptr = p[off];
            int sh = off + 1 + ptr;
            int avail = 188 - sh;
            const uint8_t *sec = p + sh;
            if (avail >= 13 && sec[0] == 0x02) {
                int sl = ((sec[1] & 0x0f) << 8) | sec[2];
                int end = 3 + sl - 4;                /* 去掉 CRC */
                int j = 12 + (((sec[10] & 0x0f) << 8) | sec[11]);
                while (j + 4 < end && j < avail) {
                    uint8_t st = sec[j];
                    int ep = ((sec[j + 1] & 0x1f) << 8) | sec[j + 2];
                    int ei = ((sec[j + 3] & 0x0f) << 8) | sec[j + 4];
                    if (st == 0x0f || st == 0x03 || st == 0x04) {
                        ts->audio_pid = ep;          /* AAC(ADTS) / MP3 */
                        break;
                    }
                    j += 5 + ei;
                }
            }
        } else if (ts->audio_pid != 0 && pid == ts->audio_pid) {
            if (pusi) {                              /* PES 头：9 字节 + 可选扩展 */
                if (off + 9 > 188) { i += 188; continue; }
                int start = off + 9 + p[off + 8];
                if (start < 188 && !hls_emit(p + start, 188 - start, sess)) return false;
            } else if (!hls_emit(p + off, 188 - off, sess)) {
                return false;
            }
        }
        i += 188;
    }
    return true;
}

static void hls_parse(const uint8_t *data, int len, const char *base, hls_pl_t *pl)
{
    memset(pl, 0, sizeof(*pl));
    pl->variant_bw = 0x7fffffff;
    int64_t idx = 0;
    float pend = 0;
    int pend_bw = -1;
    const uint8_t *p = data, *end = data + len;

    while (p < end) {
        const uint8_t *e = memchr(p, '\n', (size_t)(end - p));
        if (!e) e = end;
        size_t ln = (size_t)(e - p);
        if (ln && p[ln - 1] == '\r') ln--;
        char line[RP_URL_CAP];
        bool usable = ln > 0 && ln < sizeof(line);
        if (usable) {
            memcpy(line, p, ln);
            line[ln] = '\0';
        }
        p = e < end ? e + 1 : end;
        if (!usable) continue;

        if (line[0] == '#') {
            if (!strncmp(line, "#EXT-X-TARGETDURATION:", 23)) {
                pl->target = (float)atof(line + 23);
            } else if (!strncmp(line, "#EXT-X-MEDIA-SEQUENCE:", 22)) {
                pl->media_seq = atoll(line + 22);
            } else if (!strncmp(line, "#EXT-X-ENDLIST", 14)) {
                pl->endlist = true;
            } else if (!strncmp(line, "#EXTINF:", 8)) {
                pend = (float)atof(line + 8);
            } else if (!strncmp(line, "#EXT-X-STREAM-INF:", 18)) {
                pl->master = true;
                const char *bw = strstr(line, "BANDWIDTH=");
                pend_bw = bw ? atoi(bw + 10) : 0;
            }
            continue;
        }

        if (pl->master) {                 /* 主列表：取最低带宽（设备带宽有限） */
            if (pend_bw >= 0 && pend_bw < pl->variant_bw) {
                pl->variant_bw = pend_bw;
                hls_join(base, line, pl->variant, sizeof(pl->variant));
            }
            pend_bw = -1;
        } else if (pl->count < HLS_MAX_SEG) {   /* 媒体列表：按顺序存分片 */
            hls_join(base, line, pl->seg[pl->count].uri, sizeof(pl->seg[pl->count].uri));
            pl->seg[pl->count].dur = pend > 0 ? pend : 10.0f;
            pl->seg[pl->count].seq = pl->media_seq + idx;
            pl->count++;
            idx++;
            pend = 0;
        }
    }
}

static void hls_run(const char *url, uint32_t sess)
{
    uint8_t *plbuf = heap_caps_malloc(HLS_PL_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *segbuf = heap_caps_malloc(HLS_SEG_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    hls_pl_t *pl = heap_caps_calloc(1, sizeof(hls_pl_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!plbuf || !segbuf || !pl) {
        ESP_LOGE(TAG, "hls: no PSRAM");
        heap_caps_free(plbuf); heap_caps_free(segbuf); heap_caps_free(pl);
        return;
    }

    char media[RP_URL_CAP];
    strlcpy(media, url, sizeof(media));
    hls_ts_t ts = {0, 0};
    int64_t next_seq = -1;
    int fails = 0;
    bool announced = false;
    s_net_reason = RP_END_DISCONNECT;

    while (s_seq == sess && !s_net_stop) {
        int len = 0, status = 0;
        if (hls_get(media, plbuf, HLS_PL_CAP, &len, &status) != 0 || len <= 0) {
            ESP_LOGW(TAG, "hls: playlist fail (status %d)", status);
            if (++fails > HLS_MAX_FAIL) break;
            rp_delay_ms(1000);
            continue;
        }
        hls_parse(plbuf, len, media, pl);
        if (pl->master) {
            if (pl->variant[0] == '\0') break;
            strlcpy(media, pl->variant, sizeof(media));
            ESP_LOGI(TAG, "hls: variant -> %s", media);
            continue;
        }
        if (pl->count <= 0) {
            rp_delay_ms(800);
            continue;
        }
        fails = 0;
        if (!announced) {
            announced = true;
            s_net_connected = true;          /* 已连上，UI 显示“连接中/直播” */
            ESP_LOGI(TAG, "hls: %d segs, target %.1fs", pl->count, (double)pl->target);
        }
        if (next_seq < 0) {                  /* 从倒数第二个分片（已完整）起播 */
            next_seq = pl->endlist ? pl->seg[0].seq
                                   : pl->seg[pl->count > 2 ? pl->count - 2 : 0].seq;
        }
        if (next_seq < pl->seg[0].seq) next_seq = pl->seg[0].seq;

        /* 直播时最后一条通常是“正在写入”的分片，跳过，等它完整后再播，
         * 否则同一段音频会重播两次（听到明显重复）。 */
        int limit = pl->endlist ? pl->count : pl->count - 1;
        for (int i = 0; i < limit; i++) {
            if (pl->seg[i].seq < next_seq) continue;
            if (s_seq != sess || s_net_stop) break;
            int slen = 0, sst = 0;
            bool got = false;
            for (int attempt = 0; attempt < 2 && !got; attempt++) {
                if (hls_get(pl->seg[i].uri, segbuf, HLS_SEG_CAP, &slen, &sst) == 0 && slen >= 188)
                    got = true;
                else if (attempt == 0)
                    rp_delay_ms(300);        /* CDN 偶发 403/超时：重试一次再判死 */
            }
            if (!got) {
                /* 单片确实拿不到就跳过它继续往下播：丢一段也比整条流重连好 */
                ESP_LOGW(TAG, "hls: seg %lld fail (%d), skip", (long long)pl->seg[i].seq, sst);
                next_seq = pl->seg[i].seq + 1;
                continue;
            }
            /* 不解 PAT/PMT 状态：多数分片都带，个别不带时沿用上一片的 PID 映射。 */
            if (!hls_demux(segbuf, slen, &ts, sess)) goto out;
            next_seq = pl->seg[i].seq + 1;
        }
        /* 一轮分片播完（或本轮没有新分片）：等新分片出现再刷新播放列表。
         * 不能死等固定的 target/2（10s 分片就是 5s）：环里只剩 2~3s 数据时，
         * 每轮都会硬等出一次 2s 以上的断音。这里缓冲还充足就正常轮询，
         * 一旦降到容量的 70% 立刻刷新补货。 */
        int wait_cap = pl->target >= 1.0f ? (int)(pl->target * 500) : 1000;
        int waited = 0;
        int min_wait = 1000;             /* 至少隔 1s 再拉一次列表，避免缓冲见底时空转轮询 */
        uint32_t floor = (RP_RING_CAP * 7) / 10;
        while (s_seq == sess && !s_net_stop &&
               (waited < min_wait || (waited < wait_cap && ring_len() > floor))) {
            rp_delay_ms(100);
            waited += 100;
        }
        if (pl->endlist) break;
    }

out:
    s_net_connected = false;
    heap_caps_free(pl);
    heap_caps_free(plbuf);
    heap_caps_free(segbuf);
}

static void rp_net_task(void *arg)
{
    (void)arg;
    uint32_t sess = s_net_seq;
    s_net_reason = RP_END_HTTP_FAIL;

    /* ★ 必须自己跟 30x：`esp_http_client_set_redirection()` 只对阻塞式
     *   esp_http_client_perform() 生效（其源码里 process_again 明确标注
     *   "used only in the blocking mode"）。本函数走的是 open/fetch/read 手动
     *   路径，IDF 的自动重定向逻辑（esp_http_check_response）根本不参与 ⇒
     *   凡是被 CDN 302 到真实流地址的电台每次都会失败，表现为“切台有概率失败”。 */
    char cur[RP_URL_CAP];
    strlcpy(cur, s_net_url, sizeof(cur));

    if (rp_is_hls(cur)) {                 /* 国内电台走 HLS，单独的解封装路径 */
        hls_run(cur, sess);
        s_net_eof = true;
        s_net_alive = false;
        rp_task_exit();
        return;
    }

    esp_http_client_config_t hcfg = {
        .url = cur,
        .method = HTTP_METHOD_GET,
        .timeout_ms = RP_HTTP_CONNECT_TIMEOUT,
        .buffer_size = RP_READ_CHUNK,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .event_handler = rp_http_event,
    };
    esp_http_client_handle_t http = esp_http_client_init(&hcfg);
    if (!http) { s_net_eof = true; s_net_alive = false; rp_task_exit(); return; }
    esp_http_client_set_header(http, "User-Agent", "SDGOODS-Radio/1.0");
    esp_http_client_set_header(http, "Icy-MetaData", "0");

    int status = 0;
    bool opened = false;
    for (int hop = 0; hop <= RP_MAX_REDIRECT; hop++) {
        if (s_seq != sess || s_net_stop) break;
        s_redirect_loc[0] = '\0';   /* 每次请求前清掉上一跳的 Location */
        if (esp_http_client_open(http, 0) != ESP_OK) {
            ESP_LOGW(TAG, "http open failed: %s", cur);
            break;
        }
        opened = true;
        esp_http_client_fetch_headers(http);
        status = esp_http_client_get_status_code(http);
        if (s_seq != sess || s_net_stop) break;
        if (status / 100 != 3 || s_redirect_loc[0] == '\0') {
            break;
        }
        if (hop >= RP_MAX_REDIRECT) {
            ESP_LOGW(TAG, "too many redirects, stop at %s", cur);
            break;
        }
        char next[RP_URL_CAP];
        rp_abs_url(cur, s_redirect_loc, next, sizeof(next));
        /* 先断开再换地址：open() 只接受 INIT 状态的 client */
        esp_http_client_close(http);
        esp_http_client_reset_redirect_counter(http);   /* 跳数自己数，库里的计数器只服务阻塞模式 */
        if (esp_http_client_set_url(http, next) != ESP_OK) {
            opened = false;
            break;
        }
        strlcpy(cur, next, sizeof(cur));
        ESP_LOGI(TAG, "http %d -> %s", status, cur);
    }

    if (!opened || status / 100 != 2 || s_seq != sess || s_net_stop) {
        if (opened) {
            ESP_LOGW(TAG, "http status %d for %s", status, cur);
        }
        esp_http_client_close(http);
        esp_http_client_cleanup(http);
        s_net_eof = true; s_net_alive = false; rp_task_exit(); return;
    }
    /* 建连阶段用的是宽容超时；开始拉流后换成短超时，死台/切台少白等 */
    esp_http_client_set_timeout_ms(http, RP_HTTP_READ_TIMEOUT);
    s_net_reason = RP_END_DISCONNECT;   /* 之后若因读<=0 结束，按断流计 */
    s_net_connected = true;
    ESP_LOGI(TAG, "net: connected, streaming");

    uint8_t *tmp = heap_caps_malloc(RP_READ_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool tmp_owned = tmp != NULL;
    int rchunk = (int)RP_READ_CHUNK;
    if (!tmp) {   /* 兜底：无 PSRAM 就用栈上小块（注意同步缩小单次读入量，勿越界） */
        static uint8_t stackbuf[2048];
        tmp = stackbuf;
        rchunk = (int)sizeof(stackbuf);
    }
    /* ★ 读超时不是断流：`esp_http_client_read()` 在 socket 等不到完整数据时返回
     *   -ESP_ERR_HTTP_EAGAIN（底层 transport_ssl 打的那行
     *   `esp_tls_conn_read error, errno=No more processes` 就是它：poll 说可读，
     *   但 TLS 记录只到了一半，mbedtls 返回 WANT_READ）。
     *   旧代码 `r <= 0` 一律 break，于是切歌/网络抖动几秒就整条流重连一次，
     *   表现为“长时间播放后偶发断一次”。这里只累计空闲时间。 */
    TickType_t last_data = xTaskGetTickCount();
    while (s_seq == sess && !s_net_stop) {
        int r = esp_http_client_read(http, (char *)tmp, rchunk);
        if (r == -ESP_ERR_HTTP_EAGAIN) {
            unsigned idle_ms = (xTaskGetTickCount() - last_data) * portTICK_PERIOD_MS;
            if (idle_ms >= RP_NET_IDLE_MAX_MS) {
                ESP_LOGW(TAG, "no data for %ums, closing stream", idle_ms);
                break;
            }
            rp_delay_ms(10);
            continue;
        }
        if (r <= 0) break;              /* 真断流 / 服务端 FIN */
        last_data = xTaskGetTickCount();
        uint32_t off = 0;
        while (off < (uint32_t)r && s_seq == sess && !s_net_stop) {
            uint32_t w = ring_write(tmp + off, (uint32_t)r - off);
            off += w;
            if (w == 0) rp_delay_ms(5);   /* 环形缓冲满：等消费者腾空间（暂停即在此背压） */
        }
    }
    esp_http_client_close(http);
    esp_http_client_cleanup(http);
    if (tmp_owned) heap_caps_free(tmp);
    s_net_eof = true;
    s_net_alive = false;
    rp_task_exit();
}

/* 消费者退出会话前调它：置停止标志并等生产者任务真正退出，
 * 避免重连时新旧两个生产者同时读写全局 s_net_* / s_ring 造成竞态。 */
static void rp_net_shutdown(void)
{
    s_net_stop = true;
    while (s_net_alive) {
        rp_delay_ms(5);
    }
}

/* ---------------------------------------------------------------------------
 * 单次会话（消费者）：拉起网络生产者 → 打开解码器 → 从环形缓冲取数据解码写 I2S，
 *   直到断流 / 被取代 / 疑似编码不符。进入 PLAYING 会置 *played=true（供上层清零重连计数）。
 *   返回 RP_END_* 原因。http 由生产者成对开关；此处只管解码器 + I2S 流。
 * ------------------------------------------------------------------------- */
static int rp_attempt(const char *url, bool is_aac, uint32_t sess_seq,
                      uint8_t *raw, uint8_t *outb, bool *played)
{
    int rc = RP_END_HTTP_FAIL;
    esp_audio_simple_dec_handle_t dec = NULL;
    bool stream_started = false;
    uint32_t stream_rate = 0;
    uint8_t stream_ch = 0;
    uint64_t decoded_frames = 0;

    /* 复位环形缓冲并拉起网络生产者：生产者独立拉流灌 s_ring（PSRAM 64KB），
     * 本消费者只从环形缓冲取数据解码。网络与播放解耦后，即使这里阻塞在
     * i2s 写，后台仍在拉流，~4s 垫底足以吸收 WiFi 抖动，根治 underrun。 */
    ring_reset();
    s_net_seq = sess_seq;
    s_net_eof = false;
    s_net_connected = false;
    s_net_stop = false;
    s_net_reason = RP_END_HTTP_FAIL;
    s_net_alive = true;
    strncpy(s_net_url, url, sizeof(s_net_url) - 1);
    s_net_url[sizeof(s_net_url) - 1] = '\0';
    if (xTaskCreatePinnedToCoreWithCaps(rp_net_task, "radio_net", RP_NET_STACK, NULL,
                                       RP_NET_PRIO, NULL, 0, RP_NET_CAPS) != pdPASS) {
        s_net_alive = false;
        ESP_LOGE(TAG, "net task create failed (int free=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return RP_END_HTTP_FAIL;
    }

    if (rp_open_decoder(&dec, is_aac) != ESP_AUDIO_ERR_OK) {
        rp_net_shutdown();
        return RP_END_HTTP_FAIL;
    }

    rp_set_state(sess_seq, RP_CONNECTING);
    size_t fill = 0;
    size_t probe = 0;
    TickType_t t_connect = xTaskGetTickCount();
    while (s_seq == sess_seq && !s_net_connected && !s_net_eof) {
        if ((xTaskGetTickCount() - t_connect) * portTICK_PERIOD_MS >= RP_CONNECT_MAX_MS) {
            ESP_LOGW(TAG, "connection deadline reached");
            s_net_stop = true;
            break;
        }
        rp_delay_ms(20);
    }
    TickType_t t_first_frame = xTaskGetTickCount();
    if (s_net_connected) rp_set_state(sess_seq, RP_BUFFERING);

    /* 起播前先在环形缓冲里攒够 RP_PREBUFFER，给随后的网络抖动留垫。
     * 但设上限等待：慢速/涓流台若迟迟凑不满，超时后带已有数据直接进主循环
     * 解码（主循环有 probe/NO_DATA 兑底），避免无限“缓冲中”卡死。
     * 快速起播：TTFB 慢的台往往一开流就来一大块，等到 SOFT 门槛 + 350ms 即开播，
     * 不再死等满 8KB（相差约 0.2~0.9s）；此后生产者仍在持续灌 512KB 大环。 */
    uint32_t waited = 0;
    while (s_seq == sess_seq && !s_paused && ring_len() < RP_PREBUFFER) {
        if (s_net_eof || s_net_stop) {
            break;   /* 生产者已结束：再等也不会长数据，带已有缓冲进主循环判定 */
        }
        if (waited >= RP_PREBUFFER_SOFT_MS && ring_len() >= RP_PREBUFFER_SOFT) {
            break;
        }
        if (waited >= RP_PREBUFFER_MAX_MS) {
            ESP_LOGW(TAG, "prebuffer timeout at %u bytes, play anyway", (unsigned)ring_len());
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }

    while (s_seq == sess_seq) {
        if (s_net_stop) break;
        if (s_paused) {
            rp_set_state(sess_seq, RP_PAUSED);
            t_first_frame = xTaskGetTickCount();   /* 暂停不算超时 */
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (s_state == RP_PAUSED) {
            rp_set_state(sess_seq, stream_started ? RP_PLAYING : RP_BUFFERING);
        }

        /* 首帧看门狗：开了连接但迟迟解不出音频（涓流/只发头）就主动放弃这台，
         * 交给上面的重试 → 失败转 ERROR → UI 自动跳下一台。 */
        if (!stream_started && !s_paused &&
            (uint32_t)((xTaskGetTickCount() - t_first_frame) * portTICK_PERIOD_MS) >=
                RP_FIRST_FRAME_MAX_MS) {
            ESP_LOGW(TAG, "no audio frame in %dms, giving up this stream",
                     (int)RP_FIRST_FRAME_MAX_MS);
            rc = RP_END_HTTP_FAIL;
            break;
        }

        if (RP_RAW_CAP - fill < RP_READ_CHUNK) {
            /* 理论不会到这：每轮都把已消费部分前移。若 fill 逼近上限说明 parser 没消费。 */
            ESP_LOGW(TAG, "raw full without consumption, dropping %u bytes", (unsigned)fill);
            fill = 0;
        }

        uint32_t got = ring_read(raw + fill, (uint32_t)(RP_RAW_CAP - fill));
        if (got == 0) {
            if (s_net_eof) {                 /* 生产者已结束且缓冲排空 → 断流 */
                rc = s_net_reason;
                break;
            }
            rp_delay_ms(5);                  /* 等生产者灌（有 4s 垫底时极少走到） */
            continue;
        }
        fill += got;
        probe += got;

        /* 解码缓冲里的全部数据。每帧前先看 s_paused：暂停指令不能等这一大块
         * raw 全喂完才生效，否则一次点击要再响 100ms 以上。未消费的尾巴会在
         * 下面 memmove 前移，恢复播放后接着解，不丢数据。 */
        size_t consumed = 0;
        while (fill - consumed > 0 && s_seq == sess_seq) {
            if (s_paused) {
                break;
            }
            esp_audio_simple_dec_raw_t rin;
            memset(&rin, 0, sizeof(rin));
            rin.buffer = raw + consumed;
            rin.len = (uint32_t)(fill - consumed);

            esp_audio_simple_dec_out_t o;
            memset(&o, 0, sizeof(o));
            o.buffer = outb;
            o.len = RP_OUT_CAP;

            esp_audio_err_t e = esp_audio_simple_dec_process(dec, &rin, &o);
            if (rin.consumed > rin.len || o.decoded_size > RP_OUT_CAP) {
                ESP_LOGE(TAG, "invalid decoder lengths");
                rc = RP_END_NO_DATA;
                s_net_stop = true;
                break;
            }
            consumed += rin.consumed;
            if (s_seq != sess_seq) break;

            if (e == ESP_AUDIO_ERR_OK && o.decoded_size > 0) {
                if (!stream_started) {
                    esp_audio_simple_dec_info_t info;
                    memset(&info, 0, sizeof(info));
                    esp_audio_simple_dec_get_info(dec, &info);
                    stream_rate = info.sample_rate ? info.sample_rate : 44100;
                    stream_ch = info.channel ? info.channel : 1;
                    if (sdgoods_audio_stream_start(stream_rate, stream_ch) != ESP_OK) {
                        ESP_LOGE(TAG, "stream_start %uHz ch%u failed", (unsigned)stream_rate, stream_ch);
                        rc = RP_END_DISCONNECT;
                        s_net_stop = true;
                        break;
                    }
                    stream_started = true;
                    xSemaphoreTake(s_mu, portMAX_DELAY);
                    if(s_seq == sess_seq){s_rate=stream_rate;s_ch=stream_ch;s_state=RP_PLAYING;}
                    xSemaphoreGive(s_mu);
                    ESP_LOGI(TAG, "playing: %u Hz ch=%u %s",
                             (unsigned)stream_rate, stream_ch, is_aac ? "AAC" : "MP3");
                }
                if(s_seq != sess_seq) break;
                if(sdgoods_audio_stream_write((const int16_t *)outb, o.decoded_size) != ESP_OK){
                    ESP_LOGW(TAG, "stream write failed");
                    rc=RP_END_DISCONNECT;
                    s_net_stop=true;
                    break;
                }
                decoded_frames += o.decoded_size / (sizeof(int16_t) * stream_ch);
                rp_update_levels(outb, o.decoded_size, stream_ch);
            }
            if (rin.consumed == 0) {
                break;   /* 无进展，留待更多数据 */
            }
        }

        /* 未消费的尾部前移 */
        size_t left = fill - consumed;
        if (left > 0 && consumed > 0) {
            memmove(raw, raw + consumed, left);
        }
        fill = left;

        /* 缓冲够多却一帧未出：疑似编码不符（仅未进 PLAYING 时判定） */
        if (!stream_started && probe >= RP_PROBE_BYTES) {
            rc = RP_END_NO_DATA;
            break;
        }
    }

    if (s_seq != sess_seq) {
        rc = RP_END_SUPERSEDED;
    }

    /* 先干净收尾生产者（等它退出，避免重连时两个生产者抢 s_net_* / s_ring），
     * 再关解码器 / 冲停 I2S。http 由生产者自己成对关闭。 */
    rp_net_shutdown();
    if (stream_started) {
        *played = decoded_frames * 1000 >= (uint64_t)stream_rate * RP_STABLE_PLAY_MS;
        sdgoods_audio_stream_stop();
    }
    if (dec) {
        esp_audio_simple_dec_close(dec);
    }
    return rc;
}

/* ---------------------------------------------------------------------------
 * 一个 play 命令的完整处理：编码自动改判 + 断连重连（带退避）。
 * ------------------------------------------------------------------------- */
static void rp_session(const char *url, const char *codec, uint32_t sess_seq,
                       uint8_t *raw, uint8_t *outb)
{
    bool aac = (strstr(codec, "aac") || strstr(codec, "AAC") ||
                        strstr(codec, "mp4") || strstr(codec, "m4a"));
    bool hint = aac || strcasecmp(codec, "MP3") == 0;
    int flips = 0;
    int attempts = 0;
    TickType_t startup = xTaskGetTickCount();

    for (;;) {
        if (s_seq != sess_seq) {
            return;   /* 被新命令取代 */
        }
        bool played = false;
        int res = rp_attempt(url, aac, sess_seq, raw, outb, &played);

        if (res == RP_END_SUPERSEDED) {
            return;
        }

        /* 编码不符且无提示：翻一次 MP3<->AAC 重试（不计入网络重连） */
        if (res == RP_END_NO_DATA && !hint && flips < 1) {
            aac = !aac;
            flips++;
            ESP_LOGI(TAG, "no frames decoded, retry as %s", aac ? "AAC" : "MP3");
            continue;
        }

        /* 正常播过一阵（曾进 PLAYING）后断流：视为网络问题，重连计数清零 */
        if (played) {
            attempts = 0;
            startup = xTaskGetTickCount();
        }

        if ((xTaskGetTickCount() - startup) * portTICK_PERIOD_MS >= RP_STARTUP_MAX_MS) {
            ESP_LOGW(TAG, "startup deadline reached");
            rp_set_state(sess_seq, RP_ERROR);
            return;
        }

        if (++attempts <= RP_MAX_RETRY) {
            rp_set_state(sess_seq, RP_CONNECTING);
            uint32_t backoff_ms = 400u * (uint32_t)attempts;
            if (backoff_ms > 2000u) {
                backoff_ms = 2000u;
            }
            ESP_LOGW(TAG, "reconnect attempt %d/%d in %ums", attempts, RP_MAX_RETRY, (unsigned)backoff_ms);
            for (uint32_t wait = 0; wait < backoff_ms && s_seq == sess_seq; wait += 50) rp_delay_ms(50);
            continue;
        }

        ESP_LOGE(TAG, "giving up after %d attempts", RP_MAX_RETRY);
        rp_set_state(sess_seq, RP_ERROR);
        return;
    }
}

/* ---------------------------------------------------------------------------
 * 常驻播放任务：等命令 → 跑会话。换台/停止只需 bump s_seq + give(s_wake)。
 * ------------------------------------------------------------------------- */
static void rp_task(void *arg)
{
    (void)arg;
    uint8_t *raw = heap_caps_malloc(RP_RAW_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *outb = heap_caps_malloc(RP_OUT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!raw || !outb) {
        ESP_LOGE(TAG, "no PSRAM for player buffers");
        if (raw) heap_caps_free(raw);
        if (outb) heap_caps_free(outb);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    uint32_t handled_seq = UINT32_MAX;
    for (;;) {
        xSemaphoreTake(s_wake, portMAX_DELAY);

        char url[RP_URL_CAP];
        char codec[RP_CODEC_CAP];
        bool stop;
        xSemaphoreTake(s_mu, portMAX_DELAY);
        memcpy(url, s_url, sizeof(url));
        memcpy(codec, s_codec, sizeof(codec));
        stop = s_stop;
        s_stop = false;
        uint32_t sess_seq = s_seq;
        xSemaphoreGive(s_mu);

        if (sess_seq == handled_seq) continue;
        handled_seq = sess_seq;

        if (stop || url[0] == '\0') {
            continue;
        }

        memset((void *)s_levels, 0, sizeof(s_levels));
        rp_session(url, codec, sess_seq, raw, outb);

        /* 会话结束：若未被取代，说明已置 ERROR 或干净收尾 */
        if (s_seq == sess_seq && s_state != RP_ERROR) {
            rp_set_state(sess_seq, RP_IDLE);
        }
    }
}

/* ---------------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------------- */
esp_err_t radio_player_init(void)
{
    if (s_task) {
        return ESP_OK;
    }
    /* 解码器必须先注册，否则 esp_audio_simple_dec_open 报 “not registered” ret -7。
     * 需要两张表都注册：
     *   esp_audio_dec_register_default()        —— 注册裸 MP3/AAC 等编解码器 ops
     *     （AUDIO_DEC 表，裸流 TYPE_MP3/AAC 走的就是它，缺了就 “MP3 not registered”）
     *   esp_audio_simple_dec_register_default() —— 只注册 WAV/M4A/TS/OGG 容器 parser
     * 只需一次；放在 s_task 判空之后，避免重复注册。 */
    if (!s_dec_registered) {
        esp_audio_err_t de = esp_audio_dec_register_default();
        if (de != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "audio dec register_default failed (%d)", (int)de);
            return ESP_FAIL;
        }
        de = esp_audio_simple_dec_register_default();
        if (de != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "audio simple dec register_default failed (%d)", (int)de);
            return ESP_FAIL;
        }
        s_dec_registered = true;
        ESP_LOGI(TAG, "audio decoders registered (esp_audio_dec + simple_dec default)");
    }
    if (!s_wake) {
        s_wake = xSemaphoreCreateBinary();
    }
    if (!s_mu) {
        s_mu = xSemaphoreCreateMutex();
    }
    if (!s_wake || !s_mu) {
        return ESP_ERR_NO_MEM;
    }
    /* 网络生产者 ⇄ 解码消费者之间的环形缓冲：PSRAM 64KB，不占内部 DMA RAM */
    if (!s_ring) {
        s_ring_mu = xSemaphoreCreateMutex();
        s_ring = heap_caps_malloc(RP_RING_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring_mu || !s_ring) {
            ESP_LOGE(TAG, "no mem for radio ring buffer");
            return ESP_ERR_NO_MEM;
        }
    }
    s_state = RP_IDLE;
    s_paused = false;
    /* 钉到 core 0：LVGL 任务被显式钉在 core 1（lvgl_port task_affinity=1），
     * 解码任务优先级 6 > LVGL 5，两者同核时解码每 20ms 就抢占一次绘图，
     * 画面会周期性卡顿。放到 core 0（WiFi/app_main 那边）后两边真正并行；
     * 解码本身只占几个百分点 CPU，且大部分时间阻塞在 I2S DMA 上，不会饿死网络。 */
    if (xTaskCreatePinnedToCore(rp_task, "radio_pl", RP_TASK_STACK, NULL, RP_TASK_PRIO, &s_task, 0) != pdPASS) {
        s_task = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t radio_player_play(const char *url, const char *codec_hint)
{
    if (!url || url[0] == '\0' || strlen(url) >= RP_URL_CAP) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = radio_player_init();
    if (err != ESP_OK) {
        return err;
    }
    s_paused = false;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    if (codec_hint) {
        strncpy(s_codec, codec_hint, sizeof(s_codec) - 1);
        s_codec[sizeof(s_codec) - 1] = '\0';
    } else {
        s_codec[0] = '\0';
    }
    s_stop = false;
    s_seq++;
    /* 换台先把上一路的采样率抹掉：解码首帧之前 UI 不该拿旧值冒充新台参数 */
    s_rate = 0;
    s_ch = 0;
    s_state = RP_CONNECTING;
    memset((void *)s_levels, 0, sizeof(s_levels));
    xSemaphoreGive(s_mu);
    xSemaphoreGive(s_wake);
    return ESP_OK;
}

void radio_player_stop(void)
{
    s_paused = false;
    if (s_mu) {
        xSemaphoreTake(s_mu, portMAX_DELAY);
        s_url[0] = '\0';
        s_codec[0] = '\0';
        s_stop = true;
        s_seq++;
        xSemaphoreGive(s_mu);
    }
    if (s_wake) {
        xSemaphoreGive(s_wake);
    }
    s_state = RP_IDLE;
    memset((void *)s_levels, 0, sizeof(s_levels));
}

void radio_player_set_paused(bool paused)
{
    s_paused = paused;
    if (paused) {
        /* 暂停要立即宣告，不能等解码循环。rp_task 每轮先看 s_paused 再干活，
         * 但它可能正阻塞在一次 i2s 写（或一整块 raw 的解码）里，那段就是
         * 「点了暂停没反应」的观感时长。这里只翻状态位，真正的停声仍由
         * s_paused 控制（见 rp_attempt 的内外两层检查）。
         * ⚠️ 只改 PLAYING：BUFFERING/CONNECTING 阶段若也标 PAUSED，恢复时
         *    主循环会把「还没出声」误置成 PLAYING（状态比事实超前）。 */
        if (s_state == RP_PLAYING) {
            s_state = RP_PAUSED;
        }
    } else if (s_state == RP_PAUSED) {
        s_state = s_rate ? RP_PLAYING : RP_BUFFERING;
    }
}

radio_player_state_t radio_player_state(void)
{
    return s_state;
}

bool radio_player_is_busy(void)
{
    radio_player_state_t st = s_state;
    return st == RP_CONNECTING || st == RP_BUFFERING || st == RP_PLAYING;
}

bool radio_player_is_active(void)
{
    return s_state != RP_IDLE;
}

uint32_t radio_player_sample_rate(void)
{
    return s_rate;
}

uint8_t radio_player_channels(void)
{
    return s_ch;
}

int radio_player_levels(uint8_t *out, int n)
{
    if (!out || n <= 0) {
        return 0;
    }
    int copy = n < RADIO_PLAYER_LEVELS ? n : RADIO_PLAYER_LEVELS;
    for (int i = 0; i < copy; i++) {
        out[i] = s_levels[i];
    }
    for (int i = copy; i < n; i++) {
        out[i] = 0;
    }
    return copy;
}
