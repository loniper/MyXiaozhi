// Minimal tap guard implementation (LVGL 9 port of the sdgoods tap policy).
#include "sdgoods_tap.h"

#include <stdlib.h>

static void tap_on_pressed(lv_event_t *e)
{
    sdgoods_tap_ctx_t *c = (sdgoods_tap_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    lv_indev_t *indev = lv_indev_get_act();
    c->valid = indev != NULL;
    c->max_disp = 0;
    if (indev) lv_indev_get_point(indev, &c->start);
}

static void tap_on_pressing(lv_event_t *e)
{
    sdgoods_tap_ctx_t *c = (sdgoods_tap_ctx_t *)lv_event_get_user_data(e);
    if (!c || !c->valid) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = LV_ABS(p.x - c->start.x);
    int32_t dy = LV_ABS(p.y - c->start.y);
    int32_t d = dx > dy ? dx : dy;
    if (d > c->max_disp) c->max_disp = d;
}

static void tap_on_clicked(lv_event_t *e)
{
    sdgoods_tap_ctx_t *c = (sdgoods_tap_ctx_t *)lv_event_get_user_data(e);
    if (!c) return;
    if (!c->valid || c->max_disp <= SDGOODS_TAP_SLOP) {
        if (c->cb) c->cb(c->ud);
    }
    c->valid = false;
    c->max_disp = 0;
}

void sdgoods_tap_lock(lv_obj_t *obj)
{
    if (obj) lv_obj_add_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);
}

void sdgoods_tap_transparent(lv_obj_t *obj)
{
    if (obj) lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

void sdgoods_tap_keep_interactive(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_USER_1);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);
}

static void normalize_rec(lv_obj_t *obj)
{
    if (!obj) return;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_1)) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);
    } else if (lv_obj_check_type(obj, &lv_label_class) ||
               lv_obj_check_type(obj, &lv_canvas_class)) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    } else if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        normalize_rec(lv_obj_get_child(obj, (int32_t)i));
    }
}

void sdgoods_tap_normalize(lv_obj_t *root)
{
    normalize_rec(root);
}

void sdgoods_tap_install(void)
{
    /* AppHost owns the screen; nothing global to install. */
}

void sdgoods_tap_bind(lv_obj_t *obj, sdgoods_tap_ctx_t *ctx,
                      sdgoods_tap_cb_t on_tap, void *user_data)
{
    if (!obj || !ctx) return;
    ctx->cb = on_tap;
    ctx->ud = user_data;
    ctx->valid = false;
    ctx->max_disp = 0;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(obj, tap_on_pressed, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(obj, tap_on_pressing, LV_EVENT_PRESSING, ctx);
    lv_obj_add_event_cb(obj, tap_on_clicked, LV_EVENT_CLICKED, ctx);
}
