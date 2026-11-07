/*
 * sdgoods-radio · 页面层（应用层）
 *
 * 主屏之外的四张页面：电台列表 / 设置 / 通用选择器 / WiFi 配网。
 * 页面只做「展示 + 收集意图」，所有状态变更都通过 radio_pages_ctl_t 回调交给
 * ui_radio.c —— 页面不直接碰播放器，也不碰 NVS。
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

typedef struct {
    /* 播放第 idx 台（-1 表示忽略） */
    void (*play_index)(int idx);
    /* 城市：空串 = 自动定位。内部会重建界面并重拉目录。 */
    void (*set_city)(const char *city_zh);
    /* Apply a stable registered skin without restarting the audio player. */
    bool (*set_skin)(int skin);
    /* 定时休眠分钟（0 = 关） */
    void (*set_sleep)(int minutes);
    /* 重新拉取电台目录 */
    void (*refresh)(void);

    int         (*cur_index)(void);
    int         (*count)(void);
    const char *(*city)(void);        /* 当前城市标签（"自动定位" / "上海"） */
    int         (*sleep_min)(void);
    int         (*skin)(void);
    bool        (*loading)(void);
} radio_pages_ctl_t;

void radio_pages_init(const radio_pages_ctl_t *ctl);

void radio_pages_open_list(lv_obj_t *parent);
void radio_pages_open_settings(lv_obj_t *parent);
/* 直接打开 WiFi 配网页（开机未配网时自动弹） */
void radio_pages_open_wifi_page(lv_obj_t *parent);

/* 每 tick 调用（LVGL 线程）：处理扫描结果、连接成功收尾。 */
void radio_pages_tick(void);

void radio_pages_close_all(void);
bool radio_pages_any_open(void);

/* 主屏重建前调用：先拆掉所有页面（页面属于旧屏，随屏一起销毁会留下野指针）。 */
void radio_pages_detach(void);
