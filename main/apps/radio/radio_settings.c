/*
 * sdgoods-radio · 电台偏好设置（应用层 · 持久化服务）
 *
 * 见 radio_settings.h 顶部说明。纯 NVS 键值，低频小量读写。
 */

#include "radio_settings.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "RADIO_CFG";

#define RS_NS     "radiocfg"
#define RS_KEY_SLP "slp"     /* i32：定时休眠分钟 */
#define RS_KEY_LAST "last"   /* i32：上次台索引 */
#define RS_KEY_FAV  "fav"    /* u32：收藏位图 */
#define RS_KEY_CITY "city"   /* str：城市，空串 = 自动定位 */
#define RS_KEY_NIGHT "night" /* u8：0 纸张 / 1 夜色 */

#define RS_CITY_MAX 24

static bool s_ready = false;

void radio_settings_init(void)
{
    if (s_ready) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(RS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_close(h);
        s_ready = true;
        ESP_LOGI(TAG, "settings ns ready");
    } else {
        ESP_LOGW(TAG, "settings ns open failed (lazy per-call)");
    }
}

/* 读一个 i32/u32，失败返回默认值。 */
static int32_t rs_get_i(const char *key, int32_t def)
{
    nvs_handle_t h;
    if (nvs_open(RS_NS, NVS_READONLY, &h) != ESP_OK) {
        return def;
    }
    int32_t v = def;
    if (nvs_get_i32(h, key, &v) != ESP_OK) {
        v = def;
    }
    nvs_close(h);
    return v;
}

static void rs_set_i(const char *key, int32_t v)
{
    nvs_handle_t h;
    if (nvs_open(RS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_i32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}

int radio_settings_get_sleep_min(void)
{
    int m = (int)rs_get_i(RS_KEY_SLP, 0);
    return (m < 0) ? 0 : m;
}

void radio_settings_set_sleep_min(int minutes)
{
    if (minutes < 0) minutes = 0;
    rs_set_i(RS_KEY_SLP, minutes);
}

int radio_settings_get_last_index(void)
{
    return (int)rs_get_i(RS_KEY_LAST, -1);
}

void radio_settings_set_last_index(int idx)
{
    rs_set_i(RS_KEY_LAST, idx);
}

/* 城市：一次读出到静态缓冲。读失败就返回空串（= 自动定位），调用方无需判 NULL。 */
const char *radio_settings_get_city(void)
{
    static char s_city[RS_CITY_MAX];
    size_t len = sizeof(s_city);

    nvs_handle_t h;
    if (nvs_open(RS_NS, NVS_READONLY, &h) != ESP_OK) {
        s_city[0] = '\0';
        return s_city;
    }
    if (nvs_get_str(h, RS_KEY_CITY, s_city, &len) != ESP_OK) {
        s_city[0] = '\0';
    }
    nvs_close(h);
    s_city[sizeof(s_city) - 1] = '\0';
    return s_city;
}

bool radio_settings_set_city(const char *city_zh)
{
    nvs_handle_t h;
    if (nvs_open(RS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err=ESP_OK;
    if (city_zh && city_zh[0]) {
        char buf[RS_CITY_MAX];
        strncpy(buf, city_zh, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        if ((err=nvs_set_str(h, RS_KEY_CITY, buf)) != ESP_OK) {
            ESP_LOGW(TAG, "set city failed");
        }
    } else {
        err=nvs_erase_key(h, RS_KEY_CITY);
        if(err==ESP_ERR_NVS_NOT_FOUND)err=ESP_OK;
    }
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    return err==ESP_OK;
}

bool radio_settings_get_night(void)
{
    return rs_get_i(RS_KEY_NIGHT, 0) != 0;
}

void radio_settings_set_night(bool night)
{
    rs_set_i(RS_KEY_NIGHT, night ? 1 : 0);
}

const char *radio_settings_get_skin(void)
{
    static char id[24];
    nvs_handle_t h;
    size_t len = sizeof(id);
    if (nvs_open(RS_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t err = nvs_get_str(h, "skin_id", id, &len);
        int32_t legacy = 1;
        if (err != ESP_OK) {
            nvs_get_i32(h, RS_KEY_NIGHT, &legacy);
            snprintf(id, sizeof(id), "%s", legacy ? "dot-matrix" : "editorial");
        }
        nvs_close(h);
    } else {
        snprintf(id, sizeof(id), "%s", "dot-matrix");
    }
    id[sizeof(id) - 1] = '\0';
    if(!strcmp(id,"editorial"))snprintf(id,sizeof(id),"paper-radio");
    if(!strcmp(id,"vinyl-club"))snprintf(id,sizeof(id),"vinyl-pixel");
    return id;
}

bool radio_settings_set_skin(const char *id)
{
    if (!id || !id[0] || strlen(id) >= 24) return false;
    if(!strcmp(id,"editorial"))id="paper-radio";
    if(!strcmp(id,"vinyl-club"))id="vinyl-pixel";
    nvs_handle_t h;
    if (nvs_open(RS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "skin_id", id);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool radio_settings_is_fav(int idx)
{
    if (idx < 0 || idx > 31) {
        return false;
    }
    uint32_t bm = (uint32_t)rs_get_i(RS_KEY_FAV, 0);
    return (bm >> idx) & 1u;
}

void radio_settings_set_fav(int idx, bool on)
{
    if (idx < 0 || idx > 31) {
        return;
    }
    uint32_t bm = (uint32_t)rs_get_i(RS_KEY_FAV, 0);
    if (on) bm |=  (1u << idx);
    else    bm &= ~(1u << idx);
    rs_set_i(RS_KEY_FAV, (int32_t)bm);
}

void radio_settings_toggle_fav(int idx)
{
    radio_settings_set_fav(idx, !radio_settings_is_fav(idx));
}
