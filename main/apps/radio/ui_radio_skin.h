/*
 * 谷仓共创计划 · 谷仓 SDGOODS 开放平台基础工程
 * 应用层示例
 * https://github.com/SDGOODS/SDGOODS-ESP32S3
 *
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * 「谷仓共创计划」与「谷仓 SDGOODS 开放平台」项目、谷仓次元屏（谷仓电子徽章）设备，
 *   以及本基础代码的著作权与相关权利，均归深圳希德创新网络有限公司所有。
 * SPDX-License-Identifier: Apache-2.0
 *
 * 本文件属于应用层，以 Apache-2.0 发布：可自由商用、可闭源分发，
 * 只需保留本声明并携带 NOTICE 文件。详见 LICENSING.md。
 */

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * Radio application skin registry and shared live view.
 */
#pragma once

#include "ui_radio_kit.h"

#define RADIO_SKIN_COUNT 1
#define RADIO_SKIN_DEFAULT 0
#define RADIO_SKIN_CLOCK_Y 28
#define RADIO_SKIN_CLOCK_H 22
#define RADIO_SKIN_STATUS_Y 52
#define RADIO_SKIN_STATUS_H 20
#define RADIO_SKIN_ART_W 240
#define RADIO_SKIN_ART_H 140
#define RADIO_SKIN_ART_ROOT_X 56
#define RADIO_SKIN_ART_ROOT_Y 74
#define RADIO_SKIN_ART_ROOT_W 248
#define RADIO_SKIN_ART_ROOT_H 148
#define RADIO_SKIN_ART_INSET 4
#define RADIO_PAPER_SCENE_INSET 6
#define RADIO_PAPER_SCENE_W 276
#define RADIO_PAPER_SCENE_H 28

typedef enum { RADIO_LAYOUT_PAPER, RADIO_LAYOUT_DOTS, RADIO_LAYOUT_ART, RADIO_LAYOUT_WAVE, RADIO_LAYOUT_VINYL } radio_layout_t;
typedef struct {
    const char *id;
    const char *zh;
    const char *en;
    uint32_t bg, ink, muted, accent, card;
    radio_layout_t layout;
    const lv_img_dsc_t *art;
} radio_skin_t;

typedef struct {
    lv_obj_t *name, *meta, *clock, *status, *play_icon, *image, *live_dot;
    lv_obj_t *paper_dial, *paper_needle;
    lv_obj_t *bars[18];
    lv_obj_t *dot_wave;
    lv_obj_t *art_root, *art_fx;
    uint32_t motion_ms, motion_last;
    uint8_t motion_frame;
    bool motion_live, motion_running;
    bool bars_active;
    uint8_t dot_levels[18];
    rk_dot_t dots;
    sdgoods_tap_ctx_t taps[5];
    int skin;
} radio_skin_view_t;

typedef struct {
    sdgoods_tap_cb_t settings, list, prev, play, next;
} radio_skin_actions_t;

const radio_skin_t *radio_skin_get(int index);
int radio_skin_find(const char *id);
const char *radio_skin_name(int index);
void radio_skin_apply_palette(int index);
void radio_skin_preview(lv_obj_t *parent, int index);
void radio_skin_view_create(radio_skin_view_t *view, lv_obj_t *parent, int index,
                            const radio_skin_actions_t *actions);
void radio_skin_view_update(radio_skin_view_t *view, const char *name, const char *frequency,
                            const char *status, const char *clock, bool playing,
                            const uint8_t *levels, int level_count);
/* Advance only on the LVGL thread; background pages freeze the local clock. */
void radio_skin_view_animate(radio_skin_view_t *view, uint32_t now, bool foreground);
