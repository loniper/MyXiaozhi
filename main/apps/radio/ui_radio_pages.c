/*
 * sdgoods-radio · 页面层实现
 *
 * 一条硬规则：**任何会让当前页面被销毁的动作都走 lv_async_call**。
 * 直接在点按回调里删掉自己所在的整棵树，LVGL 的事件派发还在用这些对象，
 * 属于「能跑但迟早出鬼」的写法（旧版设置浮层只是隐藏，所以没暴露这个问题）。
 */

#include "ui_radio_pages.h"
#include "ui_radio.h"

#include "radio_lvgl_compat.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "radio_catalog.h"
/* sdgoods_lcd.h: not needed on this platform */
#include "ui_radio_kit.h"
#include "ui_radio_skin.h"
#include "sdgoods_i18n.h"
#include "sdgoods_cc.h"
#include "wifi_prov.h"

static const char *TAG = "UI_RADIO_PG";

#define LIST_SHOW_MAX   3
#define WIFI_MAX_NETS   12

static const radio_pages_ctl_t *s_ctl;

static rk_page_t s_list;
static rk_page_t s_set;
static rk_page_t s_pick;
static rk_page_t s_wifi;
static rk_page_t s_wifi_pw;
static lv_obj_t *s_skin_page, *s_skin_art, *s_skin_title, *s_skin_name, *s_skin_note;
static int s_skin_candidate;
static sdgoods_tap_ctx_t s_skin_ctx[4];
static sdgoods_tap_ctx_t s_list_nav_ctx[2];
static int s_list_offset;
static bool s_list_owned, s_list_queued;
static int s_list_direction;
static bool s_list_loading;
static lv_obj_t *s_wifi_value;
static wifi_prov_state_t s_wifi_displayed;

/* 位移守卫上下文：由调用方静态存储，生命周期覆盖对应对象 */
static sdgoods_tap_ctx_t s_list_ctx[LIST_SHOW_MAX];
static sdgoods_tap_ctx_t s_set_ctx[8];
static sdgoods_tap_ctx_t s_pick_ctx[20];
static sdgoods_tap_ctx_t s_wifi_ctx[WIFI_MAX_NETS];
static sdgoods_tap_ctx_t s_head_ctx[4];
static sdgoods_tap_ctx_t s_pw_ctx[1];

static lv_obj_t *s_pw_ta;
static lv_obj_t *s_pw_status;
static lv_obj_t *s_pw_kb;    /* 圆屏软键盘容器（定义在下方键盘小节，detach 时要置空） */
static char      s_pw_ssid[33];
static unsigned s_pw_epoch;
static lv_obj_t *s_pw_visibility_label;
static sdgoods_tap_ctx_t s_pw_visibility_ctx;

/* WiFi 扫描交接（任务 → tick） */
static char          s_nets[WIFI_MAX_NETS][33];
static int8_t        s_rssi[WIFI_MAX_NETS];
static volatile int  s_scan_cnt = -1;
static volatile bool s_scan_running;
static bool          s_scan_listed;
static bool          s_wait_connect;
static lv_obj_t *s_wifi_progress;

/* 选择器 */
static void (*s_pick_cb)(int idx);

static void radio_pages_open_wifi(lv_obj_t *parent);
static void radio_pages_open_wifi_pw(lv_obj_t *parent);
static void pw_kb_build(void);
static void list_open_at(lv_obj_t *parent, int offset);

void radio_pages_init(const radio_pages_ctl_t *ctl)
{
    s_ctl = ctl;
}

void radio_pages_detach(void)
{
    if (s_skin_page) lv_obj_del(s_skin_page);
    s_skin_page = s_skin_art = s_skin_title = s_skin_name = s_skin_note = NULL;
    rk_page_close(&s_pick);
    rk_page_close(&s_wifi_pw);
    s_pw_ta = NULL;
    ++s_pw_epoch;
    s_pw_visibility_label=NULL;
    s_pw_status=NULL;
    s_pw_kb = NULL;      /* 随页面一起被销毁；不置空则换页的 lv_async_call 会踩野指针 */
    rk_page_close(&s_wifi);
    rk_page_close(&s_set);
    s_wifi_value=NULL;
    rk_page_close(&s_list);
    s_pick_cb = NULL;
    s_wait_connect = false;
    s_wifi_progress=NULL;
}

void radio_pages_close_all(void)
{
    radio_pages_detach();
}

bool radio_pages_any_open(void)
{
    return s_skin_page || rk_page_is_open(&s_list) || rk_page_is_open(&s_set) ||
           rk_page_is_open(&s_pick) || rk_page_is_open(&s_wifi) ||
           rk_page_is_open(&s_wifi_pw);
}

/* ------------------------------------------------------------------ 异步 */
static void list_apply(void *arg)
{
    int idx = (int)(intptr_t)arg;
    rk_page_close(&s_list);
    if (s_ctl && s_ctl->play_index) {
        s_ctl->play_index(idx);
    }
}

static void list_row_cb(void *ud)
{
    lv_async_call(list_apply, ud);
}

/* ------------------------------------------------------------------ 列表 */
static void list_settings_async(void *ud)
{
    (void)ud;
    radio_pages_close_all();
    if (s_ctl) {
        lv_obj_t *parent = ui_radio_root();
        radio_pages_open_settings(parent);
    }
}

static void on_list_settings(void *ud)
{
    (void)ud;
    lv_async_call(list_settings_async, NULL);
}

static void list_move_async(void *ud)
{
    if (!rk_page_is_open(&s_list)) return;
    int count = s_ctl && s_ctl->count ? s_ctl->count() : 0;
    int offset = s_list_offset + (int)(intptr_t)ud * LIST_SHOW_MAX;
    if (offset >= 0 && offset < count) list_open_at(ui_radio_root(), offset);
}

static void list_move(void *ud) { lv_async_call(list_move_async, ud); }
static void back_to_list_async(void *ud){(void)ud;list_open_at(ui_radio_root(),s_list_offset);}
static void back_to_list(void *ud){(void)ud;lv_async_call(back_to_list_async,NULL);}
static void back_to_settings_async(void *ud){(void)ud;radio_pages_open_settings(ui_radio_root());}
static void back_to_settings(void *ud){(void)ud;lv_async_call(back_to_settings_async,NULL);}

static void list_gesture_apply(void *arg)
{
    s_list_queued=false;
    if(!rk_page_is_open(&s_list))return;
    if((int)(intptr_t)arg==2)rk_page_close(&s_list);
    else list_move_async(arg);
}

/* 同 ui_radio.c：LVGL 9 未开手势识别，自己按位移判定。
 * 左右滑翻页，上滑关闭列表（与原项目一致）。 */
static lv_point_t s_list_press_pt;
static bool s_list_press_valid;
static int list_swipe_dir_of(lv_event_t *e)
{
    lv_indev_t *indev=lv_event_get_indev(e);
    if(!indev || !s_list_press_valid)return 0;
    lv_point_t p;
    lv_indev_get_point(indev,&p);
    int dx=(int)p.x-(int)s_list_press_pt.x, dy=(int)p.y-(int)s_list_press_pt.y;
    int adx=dx<0?-dx:dx, ady=dy<0?-dy:dy;
    if(adx>=55 && adx>ady)return dx<0?1:-1;
    if(ady>=55 && ady>adx)return dy<0?2:0;
    return 0;
}
static void list_gesture(lv_event_t *e)
{
    lv_event_code_t code=lv_event_get_code(e);
    if(code==LV_EVENT_PRESSED){
        s_list_owned=true;s_list_direction=0;s_list_press_valid=false;
        lv_indev_t *indev=lv_event_get_indev(e);
        if(indev){lv_indev_get_point(indev,&s_list_press_pt);s_list_press_valid=true;}
    }else if(code==LV_EVENT_RELEASED){
        if(s_list_owned && !s_list_queued){
            s_list_direction=list_swipe_dir_of(e);
            if(s_list_direction){
                s_list_queued=true;
                lv_async_call(list_gesture_apply,(void *)(intptr_t)s_list_direction);
            }
        }
        s_list_owned=false;s_list_direction=0;s_list_press_valid=false;
    }else if(code==LV_EVENT_PRESS_LOST){
        s_list_owned=false;s_list_direction=0;s_list_press_valid=false;
    }
}

static void list_bubble(lv_obj_t *o, lv_obj_t *root)
{
    /* Leave the platform's top capture strip in sole ownership of its press. */
    if(o!=root && lv_obj_get_y(o)==0 && lv_obj_get_height(o)==70 && lv_obj_get_width(o)==360)return;
    if(o!=root)lv_obj_add_flag(o,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
    for(uint32_t i=0;i<lv_obj_get_child_cnt(o);++i)list_bubble(lv_obj_get_child(o,(int)i),root);
}

void radio_pages_open_list(lv_obj_t *parent)
{
    int current = s_ctl && s_ctl->cur_index ? s_ctl->cur_index() : 0;
    list_open_at(parent, current > 0 ? current / LIST_SHOW_MAX * LIST_SHOW_MAX : 0);
}

static void list_open_at(lv_obj_t *parent, int offset)
{
    if (!parent) {
        return;
    }
    radio_pages_detach();
    rk_page_open(&s_list, parent, SDG_T("电台列表","Stations"));
    rk_page_action(&s_list, SDG_T("设置","Settings"), &s_head_ctx[0], on_list_settings, NULL);
    lv_obj_clear_flag(s_list.root,LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(s_list.body,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_list.root,list_gesture,LV_EVENT_ALL,NULL);
    s_list_owned=s_list_queued=false;

    int n = s_ctl && s_ctl->count ? s_ctl->count() : 0;
    s_list_loading=s_ctl && s_ctl->loading && s_ctl->loading();
    if (n <= 0) {
        rk_page_hint(&s_list,s_ctl && s_ctl->loading && s_ctl->loading()?SDG_T("正在获取电台","Loading stations"):
                     wifi_prov_get_state()==WIFI_STATE_CONNECTED?SDG_T("还没有电台\n请刷新电台","No stations. Refresh to retry."):
                     SDG_T("连上 WiFi 后会自动获取","Connect Wi-Fi to load stations"));
        list_bubble(s_list.root,s_list.root);
        return;
    }
    s_list_offset = offset >= 0 && offset < n ? offset : 0;
    lv_obj_set_height(s_list.body, 144);

    static char names[LIST_SHOW_MAX][RADIO_NAME_MAX + 4];
    static char metas[LIST_SHOW_MAX][32];
    for (int row = 0; row < LIST_SHOW_MAX && s_list_offset + row < n; row++) {
        int i = s_list_offset + row;
        const radio_station_t *st = radio_catalog_at(i);
        if (!st) {
            break;
        }
        rk_text_safe(st->name, names[row], sizeof(names[row]), RK_FONT_TITLE);
        if (names[row][0] == '\0') {
            snprintf(names[row], sizeof(names[row]), SDG_T("电台 %d", "Station %d"), i + 1);
        }
        if(st->freq_mhz>0.0f){
            snprintf(metas[row],sizeof(metas[row]),"FM %.1f MHz",st->freq_mhz);
        }else if (st->bitrate > 0) {
            snprintf(metas[row], sizeof(metas[row]), "%uk", (unsigned)st->bitrate);
        } else {
            metas[row][0] = '\0';
        }
        rk_page_row_net(&s_list, names[row], metas[row], &s_list_ctx[row], list_row_cb,
                        (void *)(intptr_t)i);
    }
    const rk_theme_t *th = rk_theme();
    lv_obj_t *prev = rk_round_btn(s_list.root, 76, 276, 44, RK_ICON_BACK, th->card, th->ink);
    sdgoods_tap_bind(prev, &s_list_nav_ctx[0], list_move, (void *)(intptr_t)-1);
    if (s_list_offset == 0) lv_obj_add_state(prev, LV_STATE_DISABLED);
    lv_obj_t *next = rk_round_btn(s_list.root, 240, 276, 44, RK_ICON_CHEVRON, th->card, th->ink);
    sdgoods_tap_bind(next, &s_list_nav_ctx[1], list_move, (void *)(intptr_t)1);
    if (s_list_offset + LIST_SHOW_MAX >= n) lv_obj_add_state(next, LV_STATE_DISABLED);
    lv_obj_t *page = lv_label_create(s_list.root);
    char page_text[24];
    snprintf(page_text, sizeof(page_text), "%d / %d", s_list_offset / LIST_SHOW_MAX + 1, (n + LIST_SHOW_MAX - 1) / LIST_SHOW_MAX);
    lv_label_set_text(page, page_text);
    lv_obj_set_style_text_font(page, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(page, th->muted, 0);
    lv_obj_set_style_text_align(page, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(page, 120, 288);
    lv_obj_set_size(page, 120, 22);
    lv_obj_t *handle=rk_box(s_list.root,166,336,28,3);
    lv_obj_set_style_bg_color(handle,th->muted,0);
    lv_obj_set_style_bg_opa(handle,LV_OPA_50,0);
    lv_obj_set_style_radius(handle,2,0);
    list_bubble(s_list.root,s_list.root);
}

/* ------------------------------------------------------------------ 选择器 */
static void pick_apply(void *arg)
{
    int idx = (int)(intptr_t)arg;
    void (*cb)(int) = s_pick_cb;
    s_pick_cb = NULL;
    rk_page_close(&s_pick);
    if (cb) {
        cb(idx);
    }
}

static void pick_row_cb(void *ud)
{
    lv_async_call(pick_apply, ud);
}

static void picker_open(lv_obj_t *parent, const char *title, const char *const *labels,
                        int n, int selected, void (*cb)(int))
{
    if (!parent || n <= 0 || n > 20) {
        return;
    }
    rk_page_open(&s_pick, parent, title);
    s_pick_cb = cb;
    for (int i = 0; i < n; i++) {
        static char marks[20][8];
        if (i == selected) {
            snprintf(marks[i], sizeof(marks[i]), "当前");
        } else {
            marks[i][0] = '\0';
        }
        rk_page_row(&s_pick, labels[i], marks[i], &s_pick_ctx[i], pick_row_cb,
                    (void *)(intptr_t)i);
    }

}

/* 城市 */
static const char *const CITY_LABELS[] = {
    "自动定位", "北京", "上海", "广州", "深圳", "杭州", "成都", "武汉", "南京",
    "西安", "重庆", "天津", "香港", "台北", "长沙", "苏州",
};
#define CITY_N ((int)(sizeof(CITY_LABELS) / sizeof(CITY_LABELS[0])))

static int city_index(void)
{
    const char *cur = (s_ctl && s_ctl->city) ? s_ctl->city() : "";
    for (int i = 1; i < CITY_N; i++) {
        if (strcmp(cur, CITY_LABELS[i]) == 0) {
            return i;
        }
    }
    return 0;
}

static void city_picked(int idx)
{
    if (!s_ctl || !s_ctl->set_city || idx < 0 || idx >= CITY_N) {
        return;
    }
    s_ctl->set_city(idx == 0 ? "" : CITY_LABELS[idx]);
}

static void open_city_picker(lv_obj_t *parent)
{
    picker_open(parent, SDG_T("选择城市","City"), CITY_LABELS, CITY_N, city_index(), city_picked);
}

/* 定时 */
static const char *const SLEEP_LABELS[] = {
    "关闭", "15 分钟", "30 分钟", "45 分钟", "60 分钟", "90 分钟",
};
static const int SLEEP_STEPS[] = {0, 15, 30, 45, 60, 90};
#define SLEEP_N ((int)(sizeof(SLEEP_STEPS) / sizeof(SLEEP_STEPS[0])))

static int sleep_index(void)
{
    int cur = (s_ctl && s_ctl->sleep_min) ? s_ctl->sleep_min() : 0;
    for (int i = 0; i < SLEEP_N; i++) {
        if (SLEEP_STEPS[i] == cur) {
            return i;
        }
    }
    return 0;
}

static void sleep_picked(int idx)
{
    if (!s_ctl || !s_ctl->set_sleep || idx < 0 || idx >= SLEEP_N) {
        return;
    }
    s_ctl->set_sleep(SLEEP_STEPS[idx]);
}

static void open_sleep_picker(lv_obj_t *parent)
{
    picker_open(parent, SDG_T("定时休眠","Sleep timer"), SLEEP_LABELS, SLEEP_N, sleep_index(), sleep_picked);
}

/* ------------------------------------------------------------------ 设置 */
static void set_async_open_city(void *a)
{
    (void)a;
    open_city_picker(ui_radio_root());
}

static void set_async_open_sleep(void *a)
{
    (void)a;
    open_sleep_picker(ui_radio_root());
}

static lv_obj_t *skin_label(lv_obj_t *parent, int x, int y, int w, const char *text)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, 24);
    lv_obj_set_style_text_font(o, RK_FONT_BODY, 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    lv_label_set_text(o, text);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void skin_render(void)
{
    if (!s_skin_page) return;
    const radio_skin_t *s = radio_skin_get(s_skin_candidate);
    lv_obj_set_style_bg_color(s_skin_page, lv_color_hex(s->bg), 0);
    lv_obj_clean(s_skin_art);
    radio_skin_preview(s_skin_art, s_skin_candidate);
    char title[40];
    snprintf(title, sizeof(title), SDG_T("外观 %d / %d", "Style %d / %d"), s_skin_candidate + 1, RADIO_SKIN_COUNT);
    lv_label_set_text(s_skin_title, title);
    lv_label_set_text(s_skin_name, radio_skin_name(s_skin_candidate));
    lv_obj_set_style_text_color(s_skin_title, lv_color_hex(s->ink), 0);
    lv_obj_set_style_text_color(s_skin_name, lv_color_hex(s->ink), 0);
    lv_obj_set_style_text_color(s_skin_note, lv_color_hex(s->muted), 0);
    lv_label_set_text(s_skin_note, s_ctl && s_ctl->skin && s_ctl->skin() == s_skin_candidate
                      ? SDG_T("当前风格", "Current style") : SDG_T("点应用后保存", "Apply to save"));
}

static void skin_move_async(void *arg)
{
    if (!s_skin_page) return;
    s_skin_candidate = (s_skin_candidate + (int)(intptr_t)arg + RADIO_SKIN_COUNT) % RADIO_SKIN_COUNT;
    skin_render();
}

static void skin_move(void *ud) { lv_async_call(skin_move_async, ud); }
static void skin_cancel_async(void *ud) { (void)ud; radio_pages_open_settings(ui_radio_root()); }
static void skin_cancel(void *ud) { (void)ud; lv_async_call(skin_cancel_async, NULL); }

static void skin_apply_async(void *ud)
{
    (void)ud;
    if (!s_skin_page || !s_ctl || !s_ctl->set_skin) return;
    int candidate = s_skin_candidate;
    if (!s_ctl->set_skin(candidate)) {
        radio_pages_detach();
        rk_page_open(&s_set, ui_radio_root(), SDG_T("保存失败", "Save failed"));
        rk_page_hint(&s_set, SDG_T("保存失败，请重试", "Save failed. Try again."));
    }
}

static void skin_apply(void *ud) { (void)ud; lv_async_call(skin_apply_async, NULL); }

static void set_async_theme(void *a)
{
    (void)a;
    radio_pages_detach();
    s_skin_candidate = s_ctl && s_ctl->skin ? s_ctl->skin() : RADIO_SKIN_DEFAULT;
    s_skin_page = rk_box(ui_radio_root(), 0, 0, 360, 360);
    lv_obj_set_style_bg_opa(s_skin_page, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_skin_page, LV_OBJ_FLAG_CLICKABLE);
    s_skin_art = rk_box(s_skin_page, 0, 0, 360, 360);
    lv_obj_set_y(s_skin_art,16);
    s_skin_title = skin_label(s_skin_page, 108, 86, 136, "");
    lv_obj_t *back = rk_round_btn(s_skin_page,60,74,40,RK_ICON_BACK,
                                lv_color_hex(0x30343B),lv_color_white());
    sdgoods_tap_bind(back, &s_skin_ctx[0], skin_cancel, NULL);
    s_skin_name = skin_label(s_skin_page, 60, 242, 240, "");
    lv_obj_set_style_text_font(s_skin_name, RK_FONT_TITLE, 0);
    s_skin_note = skin_label(s_skin_page, 60, 230, 240, "");
    lv_obj_set_height(s_skin_note, 18);
    lv_obj_add_flag(s_skin_note, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_skin_note, RK_FONT_SMALL, 0);
    lv_obj_t *prev = rk_round_btn(s_skin_page, 76, 276, 44, RK_ICON_BACK,
                                 lv_color_hex(0x30343B), lv_color_white());
    sdgoods_tap_bind(prev, &s_skin_ctx[1], skin_move, (void *)(intptr_t)-1);
    lv_obj_t *apply = rk_pill(s_skin_page, 132, 276, 96, 44, SDG_T("应用", "Apply"),
                              lv_color_hex(0x30343B), lv_color_white());
    sdgoods_tap_bind(apply, &s_skin_ctx[2], skin_apply, NULL);
    lv_obj_t *next = rk_round_btn(s_skin_page, 240, 276, 44, RK_ICON_CHEVRON,
                                 lv_color_hex(0x30343B), lv_color_white());
    sdgoods_tap_bind(next, &s_skin_ctx[3], skin_move, (void *)(intptr_t)1);
    skin_render();
    sdgoods_cc_bind(s_skin_page);
}

static void set_async_refresh(void *a)
{
    (void)a;
    radio_pages_detach();
    if (s_ctl && s_ctl->refresh) {
        s_ctl->refresh();
    }
}

static void set_async_wifi(void *a)
{
    (void)a;
    radio_pages_detach();
    radio_pages_open_wifi(ui_radio_root());
}

static void set_city_cb(void *ud)    { (void)ud; lv_async_call(set_async_open_city, NULL); }
static void set_sleep_cb(void *ud)   { (void)ud; lv_async_call(set_async_open_sleep, NULL); }
static void set_theme_cb(void *ud)   { (void)ud; lv_async_call(set_async_theme, NULL); }
static void set_refresh_cb(void *ud) { (void)ud; lv_async_call(set_async_refresh, NULL); }
static void set_wifi_cb(void *ud)    { (void)ud; lv_async_call(set_async_wifi, NULL); }

static const char *wifi_state_label(wifi_prov_state_t state)
{
    switch(state){
    case WIFI_STATE_CONNECTED:return SDG_T("已连接","Connected");
    case WIFI_STATE_CONNECTING:return SDG_T("连接中","Connecting");
    case WIFI_STATE_SETUP:return SDG_T("待配网","Setup");
    default:return SDG_T("连接失败","Failed");
    }
}

void radio_pages_open_settings(lv_obj_t *parent)
{
    if (!parent) {
        return;
    }
    radio_pages_detach();
    rk_page_open(&s_set, parent, SDG_T("设置","Settings"));
    s_set.back_cb=back_to_list;

    /* 平台统一管理 WiFi 与外观/休眠：设置页只保留电台自己的两项。 */
    rk_page_row_compact(&s_set, SDG_T("城市","City"), s_ctl && s_ctl->city ? s_ctl->city() : SDG_T("自动定位","Auto"),
                        &s_set_ctx[0], set_city_cb, NULL);
    rk_page_row_compact(&s_set, SDG_T("刷新电台","Refresh"), NULL, &s_set_ctx[3], set_refresh_cb, NULL);
}

/* ------------------------------------------------------------------ WiFi */
static void wifi_scan_task(void *arg)
{
    (void)arg;
    int c = wifi_prov_scan(s_nets, s_rssi, WIFI_MAX_NETS);
    s_scan_cnt = c;
    s_scan_running = false;
    ESP_LOGI(TAG, "wifi scan done: %d", c);
    rk_task_exit();
}

static void wifi_start_scan(void)
{
    if (s_scan_running) {
        return;
    }
    s_scan_running = true;
    s_scan_listed = false;
    s_scan_cnt = -1;
    if (!rk_task_spawn(wifi_scan_task, "wifi_scan", 8192, NULL, 4)) {
        s_scan_running = false;
        ESP_LOGW(TAG, "wifi scan task create failed");
    }
}

static void wifi_net_async(void *ud)
{
    int i = (int)(intptr_t)ud;
    if (i < 0 || i >= s_scan_cnt || !rk_page_is_open(&s_wifi)) {
        return;
    }
    snprintf(s_pw_ssid, sizeof(s_pw_ssid), "%s", s_nets[i]);
    /* 密码页叠在 WiFi 页上：不销毁父页，只把父页藏起来，避免重建列表 */
    if (rk_page_is_open(&s_wifi)) {
        lv_obj_add_flag(s_wifi.root, LV_OBJ_FLAG_HIDDEN);
    }
    radio_pages_open_wifi_pw(ui_radio_root());
}
static void wifi_net_cb(void *ud){lv_async_call(wifi_net_async,ud);}
static void wifi_back_scan_async(void *ud){(void)ud;radio_pages_detach();radio_pages_open_wifi(ui_radio_root());}
static void wifi_back_scan(void *ud){(void)ud;lv_async_call(wifi_back_scan_async,NULL);}

static void wifi_fill_list(void)
{
    if (!rk_page_is_open(&s_wifi)) {
        return;
    }
    lv_obj_t *body = s_wifi.body;
    lv_obj_clean(body);
    if (s_scan_cnt <= 0) {
        rk_page_hint(&s_wifi, "没有扫描到网络\n换个位置再试，或复用手机热点");
        return;
    }
    for (int i = 0; i < s_scan_cnt && i < WIFI_MAX_NETS; i++) {
        rk_page_row(&s_wifi, s_nets[i], s_rssi[i] < -75 ? "弱" : NULL,
                    &s_wifi_ctx[i], wifi_net_cb, (void *)(intptr_t)i);
    }
}

static void wifi_rescan_cb(void *ud)
{
    (void)ud;
    if (!rk_page_is_open(&s_wifi)) {
        return;
    }
    lv_obj_clean(s_wifi.body);
    rk_page_hint(&s_wifi, "扫描中…");
    wifi_start_scan();
}

static void wifi_wait_async(void *ud)
{
    (void)ud;
    radio_pages_detach();
    rk_page_open(&s_wifi,ui_radio_root(),SDG_T("WiFi 配网","Wi-Fi"));
    s_wifi.back_cb=back_to_settings;
    rk_page_hint(&s_wifi,SDG_T("连接中","Connecting"));
    s_wifi_progress=lv_obj_get_child(s_wifi.body,0);
    s_wait_connect=true;
}

static void wifi_connect_cb(void *ud)
{
    (void)ud;
    const char *pass = s_pw_ta ? lv_textarea_get_text(s_pw_ta) : "";
    esp_err_t err=wifi_prov_submit(s_pw_ssid,pass);
    if(err==ESP_OK)lv_async_call(wifi_wait_async,NULL);
    else if(s_pw_status)lv_label_set_text(s_pw_status,SDG_T("连接失败，请重试","Request failed. Try again."));
}

/* Central keyboard: a fixed 5x3 grid, two alphabet pages, three symbol pages.
 * All keys stay away from the unreliable outer touch ring. Credentials live
 * in the textarea and survive page, case and character-set changes. */
#define PW_GRID_X       60
#define PW_GRID_Y       168
#define PW_COLS         5
#define PW_ROWS         3
#define PW_KEY_W        44
#define PW_KEY_H        34
#define PW_COL_GAP      5
#define PW_ROW_GAP      4
#define PW_TEXT_KEYS    13
#define PW_ACTION_X     90
#define PW_ACTION_Y     284
#define PW_ACTION_W     42
#define PW_ACTION_H     30
#define PW_ACTION_GAP   4
#define PW_KEY_MAX      19

#define PW_K_MODE       0x01
#define PW_K_DEL        0x02
#define PW_K_NEXT       0x03
#define PW_K_CASE       0x04

static const char PW_ABC[] = "qwertyuiopasdfghjklzxcvbnm";
static const char PW_NUM[] = "1234567890";
static const char PW_SYM[] = "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~";
static int s_pw_mode, s_pw_page;
static bool s_pw_upper, s_pw_rebuild_pending;
static char s_kb_ch[PW_KEY_MAX];
static sdgoods_tap_ctx_t s_kb_ctx[PW_KEY_MAX];
static struct { unsigned epoch; char action; } s_pw_job;

static const char *pw_charset(void)
{
    return s_pw_mode==0?PW_ABC:s_pw_mode==1?PW_NUM:PW_SYM;
}

static int pw_page_count(void)
{
    return ((int)strlen(pw_charset())+PW_TEXT_KEYS-1)/PW_TEXT_KEYS;
}

static void pw_switch_page(void *unused)
{
    (void)unused;
    s_pw_rebuild_pending=false;
    if(s_pw_job.epoch!=s_pw_epoch || !s_wifi_pw.root || !s_pw_kb || !s_pw_ta)return;
    if(s_pw_job.action==PW_K_MODE){s_pw_mode=(s_pw_mode+1)%3;s_pw_page=0;}
    else if(s_pw_job.action==PW_K_NEXT)s_pw_page=(s_pw_page+1)%pw_page_count();
    else if(s_pw_job.action==PW_K_CASE){
        if(s_pw_mode==0)s_pw_upper=!s_pw_upper;
        else{s_pw_mode=0;s_pw_page=0;}
    }
    pw_kb_build();
}

static void pw_key_any(void *ud)
{
    if(!s_pw_ta || !s_wifi_pw.root)return;
    int idx=(int)(intptr_t)ud;
    if(idx<0 || idx>=PW_KEY_MAX || s_pw_rebuild_pending)return;
    char c=s_kb_ch[idx];
    if(c==PW_K_MODE || c==PW_K_NEXT || c==PW_K_CASE){
        /* Rebuild only after event dispatch. Epoch rejects old queued jobs. */
        s_pw_job.epoch=s_pw_epoch;s_pw_job.action=c;s_pw_rebuild_pending=true;
        if(lv_async_call(pw_switch_page,NULL)!=LV_RES_OK)s_pw_rebuild_pending=false;
    }else if(c==PW_K_DEL)lv_textarea_del_char(s_pw_ta);
    else if(c)lv_textarea_add_char(s_pw_ta,(uint32_t)(uint8_t)c);
}

static lv_obj_t *pw_key(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                        const char *label, lv_color_t bg, lv_color_t fg)
{
    lv_obj_t *k = rk_box(parent, x, y, w, h);
    lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(k, 8, 0);
    lv_obj_set_style_bg_color(k, bg, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(k, LV_OPA_60, LV_STATE_PRESSED);

    lv_obj_t *l = lv_label_create(k);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, RK_FONT_BODY, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return k;
}

static void pw_bind_key(lv_obj_t *key,int slot,char code)
{
    s_kb_ch[slot]=code;
    sdgoods_tap_bind(key,&s_kb_ctx[slot],pw_key_any,(void *)(intptr_t)slot);
}

static void pw_visibility(void *unused)
{
    (void)unused;
    if(!s_pw_ta || !s_pw_visibility_label)return;
    bool hidden=!lv_textarea_get_password_mode(s_pw_ta);
    lv_textarea_set_password_mode(s_pw_ta,hidden);
    lv_label_set_text(s_pw_visibility_label,hidden?SDG_T("显示","Show"):SDG_T("隐藏","Hide"));
}

static void pw_kb_build(void)
{
    if(!s_pw_kb || !s_wifi_pw.root)return;
    lv_obj_clean(s_pw_kb);
    memset(s_kb_ch,0,sizeof(s_kb_ch));
    const rk_theme_t *th=rk_theme();
    const char *chars=pw_charset();
    int length=(int)strlen(chars);
    if(s_pw_page>=pw_page_count())s_pw_page=0;
    for(int slot=0;slot<15;++slot){
        int offset=s_pw_page*PW_TEXT_KEYS+slot;
        char c=0;
        const char *text=NULL;
        char one[2]={0,0};
        if(slot==13){c=PW_K_MODE;text=s_pw_mode==0?"123":s_pw_mode==1?"#+=":"ABC";}
        else if(slot==14){c=PW_K_DEL;text="DEL";}
        else if(offset<length){
            c=chars[offset];
            if(s_pw_mode==0 && s_pw_upper)c=(char)(c-'a'+'A');
            one[0]=c;text=one;
        }else continue;
        bool fn=slot>=13;
        lv_obj_t *k=pw_key(s_pw_kb,PW_GRID_X+(slot%PW_COLS)*(PW_KEY_W+PW_COL_GAP),
                           PW_GRID_Y+(slot/PW_COLS)*(PW_KEY_H+PW_ROW_GAP),PW_KEY_W,PW_KEY_H,
                           text,fn?th->bg:th->card,fn?th->muted:th->ink);
        pw_bind_key(k,slot,c);
    }
    /* 页码上限 3 页，但按 int 最大宽度预留，避免 -Wformat-truncation 逐字节推算 */
    char page[32];snprintf(page,sizeof(page),"%d/%d>",s_pw_page+1,pw_page_count());
    const char *labels[]={s_pw_upper?"AA":"Aa",page,SDG_T("空格","Space"),SDG_T("连接","Join")};
    const char codes[]={PW_K_CASE,PW_K_NEXT,' ',0};
    for(int i=0;i<4;++i){
        lv_obj_t *k=pw_key(s_pw_kb,PW_ACTION_X+i*(PW_ACTION_W+PW_ACTION_GAP),PW_ACTION_Y,
                           PW_ACTION_W,PW_ACTION_H,labels[i],i==3?th->ink:th->card,i==3?th->on_accent:th->ink);
        lv_obj_set_style_text_font(lv_obj_get_child(k,0),RK_FONT_SMALL,0);
        if(i==3)sdgoods_tap_bind(k,&s_pw_ctx[0],wifi_connect_cb,NULL);
        else pw_bind_key(k,15+i,codes[i]);
    }
}

static void radio_pages_open_wifi_pw(lv_obj_t *parent)
{
    if (!parent) {
        return;
    }
    rk_page_close(&s_wifi_pw);
    rk_page_open(&s_wifi_pw, parent, SDG_T("输入密码","Password"));
    s_wifi_pw.back_cb=wifi_back_scan;
    ++s_pw_epoch;
    rk_page_action(&s_wifi_pw,SDG_T("显示","Show"),&s_pw_visibility_ctx,pw_visibility,NULL);
    lv_obj_t *visibility=lv_obj_get_child(s_wifi_pw.root,-1);
    s_pw_visibility_label=lv_obj_get_child(visibility,0);

    const rk_theme_t *th = rk_theme();
    lv_obj_t *ssid = lv_label_create(s_wifi_pw.root);
    s_pw_status=ssid;
    lv_label_set_text(ssid, s_pw_ssid);
    lv_obj_set_style_text_font(ssid, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(ssid, th->muted, 0);
    lv_obj_set_pos(ssid, 70, 118);
    lv_obj_set_size(ssid, 220, 18);
    lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(ssid, LV_OBJ_FLAG_CLICKABLE);

    s_pw_ta = lv_textarea_create(s_wifi_pw.root);
    lv_obj_set_size(s_pw_ta, 240, 26);
    lv_obj_set_pos(s_pw_ta, 60, 138);
    lv_obj_set_style_pad_all(s_pw_ta,2,0);
    lv_textarea_set_one_line(s_pw_ta, true);
    lv_textarea_set_max_length(s_pw_ta, 64);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_placeholder_text(s_pw_ta, SDG_T("WiFi 密码","Wi-Fi password"));
    lv_obj_set_style_text_font(s_pw_ta, RK_FONT_BODY, 0);
    lv_obj_set_style_bg_color(s_pw_ta, th->card, 0);
    lv_obj_set_style_bg_opa(s_pw_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_pw_ta, th->ink, 0);
    lv_obj_set_style_border_width(s_pw_ta, 1, 0);
    lv_obj_set_style_border_color(s_pw_ta, th->hair, 0);
    lv_obj_set_style_radius(s_pw_ta, 10, 0);
    /* 光标必须在末尾：add_char 是「在光标处插入」，停在 0 会让密码倒序 */
    lv_textarea_set_cursor_pos(s_pw_ta, LV_TEXTAREA_CURSOR_LAST);

    /* 键盘容器：整屏透明、不吞触摸（按键自己 CLICKABLE，空白处落到 root） */
    s_pw_kb = rk_box(s_wifi_pw.root, 0, 0, LCD_WIDTH, LCD_HEIGHT);
    s_pw_mode = 0;
    s_pw_page = 0;
    s_pw_upper = false;
    pw_kb_build();
}

static void radio_pages_open_wifi(lv_obj_t *parent)
{
    if (!parent) {
        return;
    }
    rk_page_close(&s_wifi);
    rk_page_close(&s_wifi_pw);
    rk_page_open(&s_wifi, parent, SDG_T("WiFi 配网","Wi-Fi"));
    s_wifi.back_cb=back_to_settings;
    rk_page_action(&s_wifi, "重扫", &s_head_ctx[1], wifi_rescan_cb, NULL);
    rk_page_hint(&s_wifi, "扫描中…");

    /* 上一次已经扫过就直接复用，避免每次进来都等 3 秒 */
    if (s_scan_cnt >= 0 && !s_scan_running) {
        s_scan_listed = false;
        wifi_fill_list();
        s_scan_listed = true;
    } else {
        wifi_start_scan();
    }
}

/* ------------------------------------------------------------------ tick */
void radio_pages_open_wifi_page(lv_obj_t *parent)
{
    radio_pages_open_wifi(parent);
}

void radio_pages_tick(void)
{
    if(rk_page_is_open(&s_list)){
        bool loading=s_ctl && s_ctl->loading && s_ctl->loading();
        if(s_list_loading && !loading)list_open_at(ui_radio_root(),s_list_offset);
        else s_list_loading=loading;
    }
    if(s_wait_connect && rk_page_is_open(&s_wifi) && s_wifi_progress){
        lv_label_set_text(s_wifi_progress,wifi_prov_get_state()==WIFI_STATE_FAILED?
                          SDG_T("连接失败，请返回配网","Failed. Go back to configure."):SDG_T("连接中","Connecting"));
    }
    if(rk_page_is_open(&s_set) && s_wifi_value){
        wifi_prov_state_t state=wifi_prov_get_state();
        if(state!=s_wifi_displayed){
            s_wifi_displayed=state;
            lv_label_set_text(s_wifi_value,wifi_state_label(state));
        }
    }
    if (!s_wait_connect && rk_page_is_open(&s_wifi) && s_scan_cnt >= 0 && !s_scan_listed) {
        s_scan_listed = true;
        wifi_fill_list();
    }

    if (s_wait_connect && wifi_prov_get_state() == WIFI_STATE_CONNECTED) {
        s_wait_connect = false;
        radio_pages_detach();
        if (s_ctl && s_ctl->refresh) {
            s_ctl->refresh();
        }
    }
}
