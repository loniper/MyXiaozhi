/*
 * sdgoods-radio · 城市电台主屏（应用层）
 *
 * 设计见 docs/RADIO_V3_FIRMWARE_PLAN.md。这里只做三件事：
 *   ① 装配十套共享状态的主屏；
 *   ② 把明确的按钮操作翻译成播放器意图；
 *   ③ 每 tick 把播放器状态、目录状态、时钟、电平、休眠映射成界面。
 *
 * 页面（列表/设置/选择器/WiFi）在 ui_radio_pages.c；绘制原语在 ui_radio_kit.c。
 * 皮肤变化只重建屏幕；城市变化重新获取目录。
 */

#include "ui_radio.h"

#include "radio_lvgl_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "lvgl.h"
#include "radio_catalog.h"
#include "radio_player.h"
#include "radio_settings.h"
#include "sdgoods_app_shell.h"
#include "sdgoods_i18n.h"
#include "ui_radio_kit.h"
#include "ui_radio_pages.h"
#include "ui_radio_skin.h"
#include "wifi_prov.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "UI_RADIO";
static lv_obj_t *s_parent;
static lv_obj_t *s_scr;
static lv_timer_t *s_tick;
static radio_skin_view_t s_view;
static int s_skin = RADIO_SKIN_DEFAULT;
static int s_cur_idx = -1;
static bool s_paused, s_auto_wifi, s_ntp_started, s_sleep_armed;
static bool s_catalog_pending, s_catalog_autoplay;
static atomic_bool s_catalog_running, s_catalog_done;
static unsigned s_catalog_epoch, s_loading_epoch, s_finished_epoch;
static char s_city[24], s_job_city[24];
static char s_name[RADIO_NAME_MAX + 4], s_frequency[32], s_url[RADIO_URL_MAX];
static char s_codec[RADIO_CODEC_MAX];
static char s_clock[12] = "--:--";
static int s_sleep_min;
static uint32_t s_sleep_deadline, s_clock_ms, s_lang_seq;
static bool s_overlay_open, s_swipe_owned, s_swipe_queued;
static int s_swipe_direction;
static uint32_t s_style_error_until;
static bool s_request_error;
static wifi_prov_state_t s_network_prev;

static void refresh_ui(void);
static void rebuild_ui(void);
static bool ctl_set_skin(int skin);

/* 换肤左右滑动。
 *
 * 不直接平移两个活页面：那样每帧都要重新绘制两套皮肤的装饰（点阵数字、
 * 波形、按钮图标、文字），帧时间太长，动画期间会出现「某块还没画完」的底纹。
 *
 * 现在先把新旧两页各截一张 360x360 位图，动画只移动这两张图。每帧绘制量从
 * 「整页重新排版」降到「两次整图搬运」，并且图画面上是两张完整的静态图，
 * 拖动过程中不会露出任何中间态。每帧整屏标脏 → 一次整帧 flush，配合
 * lcd_display 里「整帧写入前等 TE」，面板上永远只有一张完整帧。 */
#define SKIN_SLIDE_MS 100
static int s_skin_slide_dir;       /* 1=下一页从右侧进入，-1=上一页从左侧进入，0=无动效 */
static lv_obj_t *s_slide_old;
static lv_obj_t *s_slide_new;
static lv_obj_t *s_slide_img[2];
static lv_image_dsc_t s_slide_dsc[2];
static lv_draw_buf_t *s_slide_snap[2];
static int s_slide_dir;

static void skin_slide_exec(void *obj, int32_t v)
{
    (void)obj;
    if (s_slide_img[0] == NULL || s_slide_img[1] == NULL) return;
    lv_obj_set_x(s_slide_img[0], -s_slide_dir * v);         /* 旧页快照滑出 */
    lv_obj_set_x(s_slide_img[1], s_slide_dir * (360 - v));  /* 新页快照滑入 */
}

static void skin_slide_cleanup(void)
{
    for (int i = 0; i < 2; ++i) {
        if (s_slide_img[i] != NULL) { lv_obj_del(s_slide_img[i]); s_slide_img[i] = NULL; }
        if (s_slide_snap[i] != NULL) { lv_draw_buf_destroy(s_slide_snap[i]); s_slide_snap[i] = NULL; }
    }
    if (s_slide_new != NULL) {
        lv_obj_clear_flag(s_slide_new, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(s_slide_new, 0);
        s_slide_new = NULL;
    }
    if (s_slide_old != NULL) { lv_obj_del(s_slide_old); s_slide_old = NULL; }
}

static void skin_slide_done(lv_anim_t *a)
{
    (void)a;
    skin_slide_cleanup();
}

/* 立即收尾：重复换肤 / 退出应用时调用，直接钉到终态。 */
static void skin_slide_abort(void)
{
    lv_anim_delete(NULL, skin_slide_exec);
    skin_slide_cleanup();
}

static void start_skin_slide(lv_obj_t *old, lv_obj_t *in, int dir)
{
    /* 截图失败（PSRAM 不够）就退化成瞬时切换，宁可不炫也不留残影 */
    lv_draw_buf_t *so = lv_snapshot_take(old, LV_COLOR_FORMAT_RGB565);
    lv_draw_buf_t *sn = lv_snapshot_take(in, LV_COLOR_FORMAT_RGB565);
    s_slide_snap[0] = so;
    s_slide_snap[1] = sn;
    s_slide_old = old;
    s_slide_new = in;
    s_slide_dir = dir;

    if (so == NULL || sn == NULL) {
        skin_slide_cleanup();
        return;
    }

    for (int i = 0; i < 2; ++i) {
        lv_draw_buf_t *buf = s_slide_snap[i];
        lv_image_dsc_t *d = &s_slide_dsc[i];
        memset(d, 0, sizeof(*d));
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565;
        d->header.w = 360;
        d->header.h = 360;
        d->header.stride = (uint16_t)buf->header.stride;
        d->data_size = buf->data_size;
        d->data = buf->data;

        lv_obj_t *img = lv_image_create(s_parent);
        if (img == NULL) { skin_slide_cleanup(); return; }
        lv_image_set_src(img, d);
        lv_obj_set_pos(img, 0, 0);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        s_slide_img[i] = img;
    }

    /* 两张图已盖住整屏，真实页面先藏起来，动画结束再放出新页 */
    lv_obj_add_flag(old, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(in, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(s_slide_img[0], 0);
    lv_obj_set_x(s_slide_img[1], dir * 360);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_slide_img[0]);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_duration(&a, SKIN_SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, skin_slide_exec);
    lv_anim_set_completed_cb(&a, skin_slide_done);
    lv_anim_start(&a);
}

static const char *status_word(void)
{
    if (s_paused) return SDG_T("已暂停", "Paused");
    wifi_prov_state_t wifi=wifi_prov_get_state();
    if(wifi==WIFI_STATE_CONNECTING)return SDG_T("网络连接中","Wi-Fi connecting");
    if (wifi != WIFI_STATE_CONNECTED) return SDG_T("网络未连接", "Wi-Fi offline");
    if (s_request_error) return SDG_T("连接失败，点播放重试", "Failed - press play to retry");
    switch (radio_player_state()) {
    case RP_CONNECTING: return SDG_T("连接中", "Connecting");
    case RP_BUFFERING: return SDG_T("缓冲中", "Buffering");
    case RP_PLAYING: return SDG_T("直播中", "Live");
    case RP_PAUSED: return SDG_T("已暂停", "Paused");
    case RP_ERROR: return SDG_T("连接失败，点播放重试", "Failed - press play to retry");
    default: return SDG_T("待播放", "Ready");
    }
}

static void catalog_task(void *arg)
{
    (void)arg;
    int n = radio_catalog_load(s_job_city[0] ? s_job_city : NULL);
    ESP_LOGI(TAG, "catalog loaded: %d", n);
    s_finished_epoch = s_loading_epoch;
    atomic_store(&s_catalog_done, true);
    atomic_store(&s_catalog_running, false);
    rk_task_exit();
}

static void start_catalog(void)
{
    if (atomic_load(&s_catalog_running) || atomic_load(&s_catalog_done)) {
        s_catalog_pending = true;
        return;
    }
    if (wifi_prov_get_state() != WIFI_STATE_CONNECTED) {
        s_catalog_pending = true;
        return;
    }
    snprintf(s_job_city, sizeof(s_job_city), "%s", s_city);
    s_loading_epoch = s_catalog_epoch;
    atomic_store(&s_catalog_running, true);
    s_catalog_pending = false;
    if (!rk_task_spawn(catalog_task, "radio_cat", 10240, NULL, 3)) {
        atomic_store(&s_catalog_running, false);
        s_catalog_pending = true;
        ESP_LOGW(TAG, "catalog task unavailable");
    }
}

static void arm_sleep(void)
{
    s_sleep_armed = s_sleep_min > 0;
    s_sleep_deadline = lv_tick_get() + (uint32_t)s_sleep_min * 60000u;
}

static bool select_station(int idx)
{
    if (atomic_load(&s_catalog_running)) return false;
    const radio_station_t *st = radio_catalog_at(idx);
    if (!st || !st->url[0]) return false;
    s_cur_idx = idx;
    rk_text_safe(st->name, s_name, sizeof(s_name), RK_FONT_TITLE);
    snprintf(s_url, sizeof(s_url), "%s", st->url);
    snprintf(s_codec, sizeof(s_codec), "%s", st->codec);
    if (st->freq_mhz > 0.0f) snprintf(s_frequency, sizeof(s_frequency), "FM %.1f", st->freq_mhz);
    else snprintf(s_frequency, sizeof(s_frequency), "%s", SDG_T("网络电台", "Internet radio"));
    return true;
}

static void play_index(int idx)
{
    if (!select_station(idx)) return;
    const radio_station_t *st = radio_catalog_at(idx);
    s_paused = false;
    esp_err_t err = radio_player_play(s_url, st->codec[0] ? st->codec : NULL);
    s_request_error = err != ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "play request failed: %s", esp_err_to_name(err));
        radio_player_stop();
    }
    radio_settings_set_last_index(idx);
    if (!s_request_error) arm_sleep();
    refresh_ui();
}

static void on_prev(void *ud)
{
    (void)ud;
    if (atomic_load(&s_catalog_running)) return;
    int n = radio_catalog_count();
    if (n > 0) play_index(s_cur_idx < 0 ? n - 1 : (s_cur_idx + n - 1) % n);
}

static void on_next(void *ud)
{
    (void)ud;
    if (atomic_load(&s_catalog_running)) return;
    int n = radio_catalog_count();
    if (n > 0) play_index((s_cur_idx + 1) % n);
}

static void retry_current(void)
{
    if (s_url[0]) {
        s_paused = false;
        esp_err_t err = radio_player_play(s_url, s_codec[0] ? s_codec : NULL);
        s_request_error = err != ESP_OK;
        if (s_request_error) {
            ESP_LOGW(TAG, "retry request failed: %s", esp_err_to_name(err));
            radio_player_stop();
        }
        else arm_sleep();
    } else play_index(0);
}

static void on_play(void *ud)
{
    (void)ud;
    radio_player_state_t state = radio_player_state();
    if (s_paused) {
        s_paused = false;
        if (state == RP_PAUSED || radio_player_is_busy()) {
            radio_player_set_paused(false);
            arm_sleep();
        } else retry_current();
    } else if (!s_request_error && radio_player_is_busy()) {
        s_paused = true;
        radio_player_set_paused(true);
    } else retry_current();
    refresh_ui();
}

/* 电台列表/设置页已按需求移除：进入即播，只有左右滑换皮肤与底部三键。 */
static void on_settings(void *ud) { (void)ud; }
static void on_list(void *ud) { (void)ud; }

static void cycle_skin_async(void *arg)
{
    s_swipe_queued = false;
    if (!s_scr || s_overlay_open || radio_pages_any_open()) return;
    int direction = (int)(intptr_t)arg;
    if (direction != 1 && direction != -1) return;
    int next = (s_skin + direction + RADIO_SKIN_COUNT) % RADIO_SKIN_COUNT;
    s_skin_slide_dir = direction;
    if (!ctl_set_skin(next)) {
        s_style_error_until = lv_tick_get() + 3000;
        s_skin_slide_dir = 0;
    }
    refresh_ui();
    /* 有滑动时交给动画逐帧刷新；无动效时同步画完整帧，避免 PARTIAL
     * 分帧刷新留下上一套皮肤的底纹。 */
    if (s_slide_new == NULL) lv_refr_now(NULL);
}

/* 手势：本工程 LVGL 9 未开启 LV_USE_GESTURE_RECOGNITION，LV_EVENT_GESTURE
 * 永远不会触发（上一版滑动失效的根因）。这里按 LVGL 8 的规则自己算位移：
 * 主屏任意位置（封面/文字/按钮）左右滑切换封面。列表页已移除，上滑不再响应。
 * 按钮事件带 EVENT_BUBBLE，会冒泡到这里；按下点在按钮上也一样能滑。 */
static lv_point_t s_press_pt;
static bool s_press_valid;

static int swipe_direction_of(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (!indev || !s_press_valid) return 0;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int dx = (int)p.x - (int)s_press_pt.x;
    int dy = (int)p.y - (int)s_press_pt.y;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    if (adx >= 55 && adx > ady) return dx < 0 ? 1 : -1;   /* 左滑下一张，右滑上一张 */
    return 0;
}

static void skin_gesture(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_swipe_owned = !s_overlay_open && !radio_pages_any_open();
        s_swipe_direction = 0;
        s_press_valid = false;
        lv_indev_t *indev = lv_event_get_indev(e);
        if (indev) {
            lv_indev_get_point(indev, &s_press_pt);
            s_press_valid = true;
        }
    } else if (code == LV_EVENT_RELEASED) {
        if (s_swipe_owned && !s_swipe_queued) {
            s_swipe_direction = swipe_direction_of(e);
            if (s_swipe_direction) {
                s_swipe_queued = true;
                lv_async_call(cycle_skin_async, (void *)(intptr_t)s_swipe_direction);
            }
        }
        s_swipe_owned = false;
        s_swipe_direction = 0;
        s_press_valid = false;
    } else if (code == LV_EVENT_PRESS_LOST) {
        s_swipe_owned = false;
        s_swipe_direction = 0;
        s_press_valid = false;
    }
}

static void overlay_open(void) { s_overlay_open = true; s_swipe_owned = false; }
static void overlay_close(void) { s_overlay_open = false; }

static void refresh_ui(void)
{
    if (!s_scr) return;
    uint8_t levels[RADIO_PLAYER_LEVELS] = {0};
    int n = radio_player_levels(levels, RADIO_PLAYER_LEVELS);
    bool playing = !s_request_error && !s_paused && radio_player_state() == RP_PLAYING;
    const char *name = s_name[0] ? s_name : SDG_T("城市电台", "City Radio");
    const char *status = !s_name[0] && atomic_load(&s_catalog_running)
                       ? SDG_T("正在获取电台", "Loading stations") : status_word();
    if ((int32_t)(s_style_error_until - lv_tick_get()) > 0)
        status = SDG_T("保存失败，请重试", "Save failed. Try again.");
    bool active = !s_request_error && !s_paused && (playing || radio_player_state() == RP_CONNECTING || radio_player_state() == RP_BUFFERING);
    char frequency[32];
    if(strncmp(s_frequency,"FM ",3)==0)snprintf(frequency,sizeof(frequency),"%s",s_frequency);
    else if(s_cur_idx>=0 && !s_catalog_pending && !atomic_load(&s_catalog_running))
        snprintf(frequency,sizeof(frequency),"CH %02d",s_cur_idx+1);
    else snprintf(frequency,sizeof(frequency),"WEB");
    radio_skin_view_update(&s_view, name, frequency, status, s_clock,
                           active, playing ? levels : NULL, playing ? n : 0);
}

static void build_screen(void)
{
    /* Embedded in the AppHost scene root: a plain 360x360 container, not a
     * standalone screen (the host owns screen switching). */
    s_scr = lv_obj_create(s_parent);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_size(s_scr, 360, 360);
    lv_obj_set_pos(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    /* 整屏可点击，LVGL 9 才会把触摸目标定为 s_scr；否则摸到艺术画/文字时
     * 找不到可点击对象，滑动事件不会产生。 */
    lv_obj_add_flag(s_scr, LV_OBJ_FLAG_CLICKABLE);
    const radio_skin_actions_t actions = {on_settings, on_list, on_prev, on_play, on_next};
    radio_skin_view_create(&s_view, s_scr, s_skin, &actions);
    lv_obj_add_event_cb(s_scr, skin_gesture, LV_EVENT_ALL, NULL);
    refresh_ui();
}

static void rebuild_ui(void)
{
    radio_pages_detach();
    skin_slide_abort();
    lv_obj_t *old = s_scr;
    int dir = s_skin_slide_dir;
    s_skin_slide_dir = 0;

    build_screen();
    if (!old) return;

    if (dir == 0) {
        /* 非滑动场景（切语言、回滚等）：直接替换 */
        lv_obj_del(old);
        return;
    }
    start_skin_slide(old, s_scr, dir);
}

static void ctl_set_city(const char *city)
{
    if(!radio_settings_set_city(city)){
        s_style_error_until=lv_tick_get()+3000;
        radio_pages_detach();refresh_ui();return;
    }
    snprintf(s_city, sizeof(s_city), "%s", city ? city : "");
    ++s_catalog_epoch;
    s_catalog_autoplay = !s_name[0] && !s_paused;
    s_catalog_pending = true;
    s_cur_idx = -1;
    radio_pages_detach();
    refresh_ui();
}

static bool ctl_set_skin(int skin)
{
    if (skin < 0 || skin >= RADIO_SKIN_COUNT || !s_scr) return false;
    int old = s_skin;
    s_skin = skin;
    radio_skin_apply_palette(s_skin);
    rebuild_ui();
    if (!radio_settings_set_skin(radio_skin_get(s_skin)->id)) {
        s_skin = old;
        radio_skin_apply_palette(old);
        rebuild_ui();
        ESP_LOGW(TAG, "skin persistence failed; previous skin restored");
        return false;
    }
    ESP_LOGI(TAG, "skin=%s internal=%u psram=%u", radio_skin_get(s_skin)->id,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

static void ctl_set_sleep(int minutes)
{
    s_sleep_min = minutes;
    radio_settings_set_sleep_min(minutes);
    arm_sleep();
}

static void ctl_refresh(void)
{
    ++s_catalog_epoch;
    s_catalog_pending = true;
    s_catalog_autoplay = !s_name[0] && !s_paused;
}

static int ctl_cur_index(void) { return s_cur_idx; }
static int ctl_count(void) { return s_catalog_pending || atomic_load(&s_catalog_running) ? 0 : radio_catalog_count(); }
static bool ctl_loading(void) { return s_catalog_pending || atomic_load(&s_catalog_running); }
static int ctl_skin(void) { return s_skin; }
static const char *ctl_city(void) { return s_city[0] ? s_city : SDG_T("自动定位", "Auto"); }
static int ctl_sleep_min(void) { return s_sleep_min; }
static const radio_pages_ctl_t s_ctl = {
    .play_index = play_index, .set_city = ctl_set_city, .set_skin = ctl_set_skin,
    .set_sleep = ctl_set_sleep, .refresh = ctl_refresh, .cur_index = ctl_cur_index,
    .count = ctl_count, .city = ctl_city, .sleep_min = ctl_sleep_min, .skin = ctl_skin,
    .loading = ctl_loading,
};

static void clock_tick(uint32_t now)
{
    if (now - s_clock_ms < 1000) return;
    s_clock_ms = now;
    time_t t = time(NULL);
    struct tm tm_now;
    localtime_r(&t, &tm_now);
    if (tm_now.tm_year + 1900 >= 2024) snprintf(s_clock, sizeof(s_clock), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    if (!s_ntp_started && wifi_prov_get_state() == WIFI_STATE_CONNECTED) {
        s_ntp_started = true;
        setenv("TZ", "CST-8", 1);
        tzset();
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "ntp.aliyun.com");
        esp_sntp_init();
    }
}

static void radio_tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s_scr) return;
    if (s_lang_seq != sdg_i18n_seq() && !s_overlay_open) {
        s_lang_seq = sdg_i18n_seq();
        rebuild_ui();
    }
    if (s_auto_wifi && !s_overlay_open) {
        s_auto_wifi = false;
        radio_pages_open_wifi_page(s_scr);
    }
    radio_pages_tick();
    uint32_t now = lv_tick_get();
    clock_tick(now);
    if (s_sleep_armed && (int32_t)(now - s_sleep_deadline) >= 0) {
        s_sleep_armed = false;
        s_paused = true;
        radio_player_stop();
    }
    wifi_prov_state_t network=wifi_prov_get_state();
    if(network==WIFI_STATE_CONNECTED && s_network_prev!=WIFI_STATE_CONNECTED){
        if(!s_paused && s_url[0] && !radio_player_is_busy() && radio_player_state()!=RP_PAUSED){
            s_request_error=radio_player_play(s_url,s_codec[0]?s_codec:NULL)!=ESP_OK;
        }
    }
    s_network_prev=network;
    if (!atomic_load(&s_catalog_running) && atomic_exchange(&s_catalog_done, false)) {
        if (s_finished_epoch != s_catalog_epoch) {
            s_catalog_pending = true;
        } else {
            int count = radio_catalog_count();
            int selected = -1;
            for (int i = 0; i < count && s_url[0]; ++i) {
                const radio_station_t *st = radio_catalog_at(i);
                if (st && strcmp(st->url, s_url) == 0) { selected = i; break; }
            }
            if (selected >= 0) s_cur_idx = selected;
            else if (s_catalog_autoplay && !s_paused && count > 0) {
                int last = radio_settings_get_last_index();
                play_index(last >= 0 && last < count ? last : 0);
            } else if (!s_name[0] && count > 0) select_station(0);
            else if (s_name[0]) s_cur_idx = -1;
            s_catalog_autoplay = false;
        }
    }
    if (s_catalog_pending && !atomic_load(&s_catalog_running) &&
        wifi_prov_get_state() == WIFI_STATE_CONNECTED) start_catalog();
    if (!s_overlay_open && !radio_pages_any_open()) refresh_ui();
    radio_skin_view_animate(&s_view,now,!s_overlay_open && !radio_pages_any_open() && s_scr != NULL);
}

static void close_to_app(void)
{
    if (!s_scr) return;
    radio_pages_detach();
    skin_slide_abort();
    radio_player_stop();
    if (s_tick) { lv_timer_del(s_tick); s_tick = NULL; }
    lv_obj_t *old = s_scr;
    s_scr = NULL;
    s_parent = NULL;
    memset(&s_view, 0, sizeof(s_view));
    lv_obj_del(old);
}

/* 开机预热：内置国内电台是本地数据，直接同步装入，零网络、零等待。 */
void radio_startup_warmup(void)
{
    radio_catalog_seed_domestic();
}

void ui_radio_start(lv_obj_t *parent)
{
    if (s_scr || !parent) return;
    s_parent = parent;
    /* This radio opens in Chinese; keep bilingual strings for runtime switching. */
    sdg_i18n_set(SDG_LANG_ZH);
    radio_settings_init();
    s_skin = radio_skin_find(radio_settings_get_skin());
    s_sleep_min = radio_settings_get_sleep_min();
    s_paused = false;
    s_catalog_autoplay = true;
    s_request_error = false;
    s_network_prev=wifi_prov_get_state();
    s_cur_idx = -1;
    s_name[0] = s_url[0] = s_frequency[0] = '\0';
    s_lang_seq = sdg_i18n_seq();
    snprintf(s_city, sizeof(s_city), "%s", radio_settings_get_city());
    radio_skin_apply_palette(s_skin);
    radio_player_init();
    /* 内置国内电台（央广 HLS）：同步装载，零网络等待，进入即可播。 */
    radio_catalog_seed_domestic();
    {
        int count = radio_catalog_count();
        int last = radio_settings_get_last_index();
        if (last < 0 || last >= count) last = 0;
        if (count > 0 && select_station(last)) {
            s_catalog_autoplay = false;
            if (wifi_prov_get_state() == WIFI_STATE_CONNECTED) {
                s_request_error = radio_player_play(s_url, s_codec[0] ? s_codec : NULL) != ESP_OK;
            }
        }
    }
    s_catalog_pending = false;
    radio_pages_init(&s_ctl);
    build_screen();
    sdgoods_app_shell_set_exit_cb(close_to_app);
    s_overlay_open = s_swipe_owned = s_swipe_queued = false;
    sdgoods_app_shell_set_pause_cb(overlay_open);
    sdgoods_app_shell_set_resume_cb(overlay_close);
    s_tick = lv_timer_create(radio_tick, 120, NULL);
    s_auto_wifi = wifi_prov_is_setup_mode();
}

void ui_radio_poll(void) { }
bool ui_radio_is_open(void) { return s_scr != NULL; }
lv_obj_t *ui_radio_root(void) { return s_scr; }
void ui_radio_close(void) { if (s_scr) sdgoods_app_shell_leave(true); }
void ui_radio_pause(void) { overlay_open(); }
void ui_radio_resume(void) { overlay_close(); }
