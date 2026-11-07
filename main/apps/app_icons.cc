// Vector icons shared by the app center tiles.
#include "app_icons.h"

namespace {

constexpr uint32_t kCream = 0xFFF1D6;
constexpr uint32_t kAmber = 0xFFB45C;
constexpr uint32_t kAmberDark = 0xE39A4F;
constexpr uint32_t kCoral = 0xFF6B4A;
constexpr uint32_t kKnob = 0x8A4A12;

void DrawRect(lv_layer_t* layer, int ox, int oy, int x, int y, int w, int h, int radius,
              uint32_t color) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = radius;
    lv_area_t area = {ox + x, oy + y, ox + x + w - 1, oy + y + h - 1};
    lv_draw_rect(layer, &dsc, &area);
}

void DrawLine(lv_layer_t* layer, int ox, int oy, int x1, int y1, int x2, int y2,
              uint32_t color, int width) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_hex(color);
    dsc.width = width;
    dsc.opa = LV_OPA_COVER;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1 = {static_cast<lv_value_precise_t>(ox + x1), static_cast<lv_value_precise_t>(oy + y1)};
    dsc.p2 = {static_cast<lv_value_precise_t>(ox + x2), static_cast<lv_value_precise_t>(oy + y2)};
    lv_draw_line(layer, &dsc);
}

void DrawRadio(lv_layer_t* layer, int ox, int oy) {
    // Antenna first so the body covers its lower end.
    DrawLine(layer, ox, oy, 58, 24, 68, 11, kCream, 3);
    DrawRect(layer, ox, oy, 12, 24, 56, 38, 9, kAmber);
    DrawRect(layer, ox, oy, 18, 31, 23, 24, 6, kCream);
    for (int y = 38; y <= 48; y += 5) {
        DrawLine(layer, ox, oy, 24, y, 35, y, kAmberDark, 2);
    }
    DrawRect(layer, ox, oy, 45, 31, 17, 12, 3, kCream);
    DrawLine(layer, ox, oy, 54, 32, 54, 42, kCoral, 3);
    DrawRect(layer, ox, oy, 45, 50, 4, 4, LV_RADIUS_CIRCLE, kKnob);
    DrawRect(layer, ox, oy, 55, 50, 4, 4, LV_RADIUS_CIRCLE, kKnob);
    DrawRect(layer, ox, oy, 18, 58, 44, 2, 1, kAmberDark);
}

void DrawFlashlight(lv_layer_t* layer, int ox, int oy) {
    // 灯头射出的三道光束
    DrawLine(layer, ox, oy, 34, 20, 26, 9, kCream, 3);
    DrawLine(layer, ox, oy, 40, 18, 40, 6, kCream, 3);
    DrawLine(layer, ox, oy, 46, 20, 54, 9, kCream, 3);
    // 灯头与灯珠
    DrawRect(layer, ox, oy, 27, 20, 26, 12, 4, kCream);
    DrawRect(layer, ox, oy, 32, 22, 16, 8, 3, kCoral);
    // 筒身与防滑纹
    DrawRect(layer, ox, oy, 31, 32, 18, 34, 6, kAmber);
    for (int y = 40; y <= 56; y += 6) {
        DrawLine(layer, ox, oy, 35, y, 45, y, kAmberDark, 2);
    }
    // 开关
    DrawRect(layer, ox, oy, 35, 35, 10, 3, 1, kKnob);
}

void OnDraw(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const uintptr_t kind = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    switch (kind) {
        case 0:
            DrawRadio(layer, coords.x1, coords.y1);
            break;
        default:
            DrawFlashlight(layer, coords.x1, coords.y1);
            break;
    }
}

void MakeIcon(lv_obj_t* parent, int x, int y, int kind) {
    lv_obj_t* icon = lv_obj_create(parent);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 80, 80);
    lv_obj_set_pos(icon, x, y);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(icon, OnDraw, LV_EVENT_DRAW_MAIN,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(kind)));
}

}  // namespace

void DrawRadioIcon(lv_obj_t* parent, int x, int y) { MakeIcon(parent, x, y, 0); }

void DrawFlashlightIcon(lv_obj_t* parent, int x, int y) { MakeIcon(parent, x, y, 1); }
