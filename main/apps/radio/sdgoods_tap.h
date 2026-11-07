// Minimal tap guard for the ported radio UI (LVGL 9).
// Only the pieces the radio screens use: press-displacement guard + press lock.
#pragma once

#include "lvgl.h"

#define SDGOODS_TAP_SLOP  24

typedef void (*sdgoods_tap_cb_t)(void *user_data);

typedef struct {
    lv_point_t       start;
    int32_t          max_disp;
    bool             valid;
    sdgoods_tap_cb_t cb;
    void            *ud;
} sdgoods_tap_ctx_t;

void sdgoods_tap_install(void);
void sdgoods_tap_normalize(lv_obj_t *root);
void sdgoods_tap_lock(lv_obj_t *obj);
void sdgoods_tap_transparent(lv_obj_t *obj);
void sdgoods_tap_keep_interactive(lv_obj_t *obj);
void sdgoods_tap_bind(lv_obj_t *obj, sdgoods_tap_ctx_t *ctx,
                      sdgoods_tap_cb_t on_tap, void *user_data);
