// Vector icons shared by the app center tiles.
#include "app_icons.h"

namespace {

constexpr uint32_t kCream = 0xFFF1D6;
constexpr uint32_t kAmber = 0xFFB45C;
constexpr uint32_t kAmberDark = 0xE39A4F;
constexpr uint32_t kCoral = 0xFF6B4A;
constexpr uint32_t kKnob = 0x8A4A12;
constexpr uint32_t kIconWhite = 0xF2F4F7;
constexpr uint32_t kIconAmber = 0xFFC761;
// 手电图标配色（参考扁平矢量风：青绿机身 / 金色灯头 / 深蓝尾盖）
constexpr uint32_t kIconTeal = 0x93C4C6;
constexpr uint32_t kIconTealDark = 0x70A0A0;
constexpr uint32_t kIconGold = 0xEDC64B;
constexpr uint32_t kIconGoldDark = 0xC9A236;
// 参考图的深蓝尾盖在黑底上会糊掉（应用中心是纯黑），这里提亮一档。
constexpr uint32_t kIconNavy = 0x3D4560;
constexpr uint32_t kIconRed = 0xE4573D;

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

void DrawLineOpa(lv_layer_t* layer, int ox, int oy, int x1, int y1, int x2, int y2,
                 uint32_t color, int width, lv_opa_t opa) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_hex(color);
    dsc.width = width;
    dsc.opa = opa;
    dsc.round_start = 1;
    dsc.round_end = 1;
    dsc.p1 = {static_cast<lv_value_precise_t>(ox + x1), static_cast<lv_value_precise_t>(oy + y1)};
    dsc.p2 = {static_cast<lv_value_precise_t>(ox + x2), static_cast<lv_value_precise_t>(oy + y2)};
    lv_draw_line(layer, &dsc);
}

void DrawRectLine(lv_layer_t* layer, int ox, int oy, int x, int y, int w, int h, int radius,
                  uint32_t color, int width) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_TRANSP;
    dsc.border_color = lv_color_hex(color);
    dsc.border_opa = LV_OPA_COVER;
    dsc.border_width = width;
    dsc.border_side = LV_BORDER_SIDE_FULL;
    dsc.radius = radius;
    lv_area_t area = {ox + x, oy + y, ox + x + w - 1, oy + y + h - 1};
    lv_draw_rect(layer, &dsc, &area);
}

void DrawTri(lv_layer_t* layer, int ox, int oy, int x1, int y1, int x2, int y2, int x3, int y3,
             uint32_t color) {
    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = lv_color_hex(color);
    dsc.opa = LV_OPA_COVER;
    dsc.p[0] = {static_cast<lv_value_precise_t>(ox + x1), static_cast<lv_value_precise_t>(oy + y1)};
    dsc.p[1] = {static_cast<lv_value_precise_t>(ox + x2), static_cast<lv_value_precise_t>(oy + y2)};
    dsc.p[2] = {static_cast<lv_value_precise_t>(ox + x3), static_cast<lv_value_precise_t>(oy + y3)};
    lv_draw_triangle(layer, &dsc);
}

// 任意四边形（拆成两个三角形），斜放的机身/灯头都靠它，避免引位图资源。
void DrawQuad(lv_layer_t* layer, int ox, int oy, int x1, int y1, int x2, int y2, int x3, int y3,
              int x4, int y4, uint32_t color) {
    DrawTri(layer, ox, oy, x1, y1, x2, y2, x3, y3, color);
    DrawTri(layer, ox, oy, x1, y1, x3, y3, x4, y4, color);
}

void DrawRing(lv_layer_t* layer, int ox, int oy, int cx, int cy, int radius, int width,
              uint32_t color) {
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = lv_color_hex(color);
    dsc.opa = LV_OPA_COVER;
    dsc.width = width;
    dsc.radius = static_cast<uint16_t>(radius);
    dsc.start_angle = 0;
    dsc.end_angle = 360;
    dsc.center = {ox + cx, oy + cy};
    lv_draw_arc(layer, &dsc);
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
    // 45° 斜持的扁平手电：青绿机身 + 金色灯头 + 深蓝尾盖 + 黄色虚线光束。
    // 所有顶点由脚本按角度算好后写死（80x80 画布）。整体控制在不含光束 44x38、
    // 含光束 60x58 的范围并居中，与电台图标（56x51 / 含天线 63x58）视觉大小一致；
    // 早期版本画满 80x80，在应用中心里明显比电台大一圈。
    DrawQuad(layer, ox, oy, 15, 53, 35, 34, 44, 44, 23, 62, kIconTeal);       // 机身
    DrawQuad(layer, ox, oy, 21, 60, 42, 42, 44, 44, 23, 62, kIconTealDark);   // 机身暗面
    DrawQuad(layer, ox, oy, 13, 54, 17, 50, 26, 60, 22, 64, kIconNavy);       // 尾盖
    DrawQuad(layer, ox, oy, 33, 32, 39, 26, 54, 41, 46, 47, kIconGold);       // 灯头
    DrawQuad(layer, ox, oy, 33, 32, 39, 26, 41, 26, 34, 33, kIconNavy);       // 灯头上边
    DrawQuad(layer, ox, oy, 46, 46, 53, 39, 54, 41, 46, 47, kIconNavy);       // 灯头下边
    DrawQuad(layer, ox, oy, 39, 26, 42, 24, 55, 39, 54, 41, kIconWhite);      // 灯面
    DrawLine(layer, ox, oy, 35, 31, 46, 45, kIconGoldDark, 1);                // 散热棱
    DrawLine(layer, ox, oy, 37, 30, 49, 42, kIconGoldDark, 1);
    DrawRect(layer, ox, oy, 18, 48, 4, 4, LV_RADIUS_CIRCLE, kIconRed);        // 红色开关
    DrawLine(layer, ox, oy, 46, 23, 46, 21, kIconGold, 2);                    // 光束
    DrawLine(layer, ox, oy, 47, 19, 48, 16, kIconGold, 2);
    DrawLine(layer, ox, oy, 48, 14, 49, 12, kIconGold, 2);
    DrawLine(layer, ox, oy, 51, 29, 53, 27, kIconGold, 2);
    DrawLine(layer, ox, oy, 55, 26, 57, 24, kIconGold, 2);
    DrawLine(layer, ox, oy, 58, 22, 60, 21, kIconGold, 2);
    DrawLine(layer, ox, oy, 57, 35, 59, 34, kIconGold, 2);
    DrawLine(layer, ox, oy, 61, 34, 63, 34, kIconGold, 2);
    DrawLine(layer, ox, oy, 65, 33, 67, 33, kIconGold, 2);
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
