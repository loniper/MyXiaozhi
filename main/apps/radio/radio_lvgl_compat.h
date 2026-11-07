// LVGL 8 -> LVGL 9 compatibility for the ported sdgoods-radio UI code.
// The upstream app targets LVGL 8.3 (custom draw callbacks). LVGL 9 keeps the
// object/event API almost identical, but the draw descriptors changed shape.
#pragma once

#include "lvgl.h"
#include "lv_api_map_v8.h"

/* Legacy image colour formats (data layout is byte-identical in LVGL 9,
 * LVGL treats stride==0 indexed descriptors as the v8 layout). */
#define LV_IMG_CF_UNKNOWN 0x00
#define LV_IMG_CF_RAW 0x01
#define LV_IMG_CF_INDEXED_1BIT LV_COLOR_FORMAT_I1
#define LV_IMG_CF_INDEXED_2BIT LV_COLOR_FORMAT_I2
#define LV_IMG_CF_INDEXED_4BIT LV_COLOR_FORMAT_I4
#define LV_IMG_CF_INDEXED_8BIT LV_COLOR_FORMAT_I8
#define LV_IMG_CF_TRUE_COLOR LV_COLOR_FORMAT_NATIVE
#define LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED LV_COLOR_FORMAT_NATIVE
#define LV_IMG_CF_TRUE_COLOR_ALPHA LV_COLOR_FORMAT_NATIVE_ALPHA

/* Fixed panel geometry used by the ported layout constants. */
#define LCD_WIDTH 360
#define LCD_HEIGHT 360

/* LVGL 8 animation "speed" (ms per 10 px) -> LVGL 9 time-based duration. */
#define lv_obj_set_style_anim_speed(obj, speed, sel) \
    lv_obj_set_style_anim_duration((obj), (int32_t)(speed) * 10, (sel))

#define lv_textarea_del_char(ta) lv_textarea_delete_char(ta)

#define lv_draw_ctx_t lv_layer_t
#define lv_event_get_draw_ctx(e) lv_event_get_layer(e)

static inline void rk_draw_arc_v8(lv_layer_t *layer, lv_draw_arc_dsc_t *dsc,
                                  const lv_point_t *center, uint16_t radius,
                                  uint16_t a0, uint16_t a1) {
    dsc->center = *center;
    dsc->radius = radius;
    dsc->start_angle = a0;
    dsc->end_angle = a1;
    lv_draw_arc(layer, dsc);
}
#define lv_draw_arc(layer, dsc, center, radius, a0, a1) \
    rk_draw_arc_v8((layer), (dsc), (center), (radius), (a0), (a1))

static inline void rk_draw_line_v8(lv_layer_t *layer, lv_draw_line_dsc_t *dsc,
                                   const lv_point_t *p1, const lv_point_t *p2) {
    dsc->p1.x = p1->x;
    dsc->p1.y = p1->y;
    dsc->p2.x = p2->x;
    dsc->p2.y = p2->y;
    lv_draw_line(layer, dsc);
}
#define lv_draw_line(layer, dsc, p1, p2) \
    rk_draw_line_v8((layer), (dsc), (p1), (p2))

/* Upstream passes a rect descriptor to draw a solid triangle; honour that. */
static inline void rk_draw_triangle_v8(lv_layer_t *layer,
                                       const lv_draw_rect_dsc_t *rd,
                                       const lv_point_t *pts) {
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = rd->bg_color;
    dsc.opa = rd->bg_opa;
    for (int i = 0; i < 3; ++i) {
        dsc.p[i].x = pts[i].x;
        dsc.p[i].y = pts[i].y;
    }
    lv_draw_triangle(layer, &dsc);
}
#define lv_draw_triangle(layer, rd, pts) \
    rk_draw_triangle_v8((layer), (rd), (pts))
