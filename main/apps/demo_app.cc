// Skeleton app used to validate the app framework end to end (M1):
// enter (voice or API), back with the single physical key, control center
// still available on top.
#include "app.h"
#include "app_icons.h"

#include <lvgl.h>

extern const AppInfo kDemoInfo = {"demo", "演示应用", 0x6C8BFF, DrawDemoIcon};

namespace {

lv_obj_t* MakeDot(lv_obj_t* parent, int x, int y) {
    lv_obj_t* dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, x, y);
    return dot;
}

class DemoApp : public App {
public:
    const AppInfo& info() const override { return kDemoInfo; }

    void OnEnter(AppContext& ctx) override {
        lv_obj_t* root = ctx.root();
        lv_obj_set_style_bg_color(root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

        // Simple icon: a dark card with three white dots.
        lv_obj_t* card = lv_obj_create(root);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 120, 120);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, -30);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x15181D), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 30, 0);
        MakeDot(card, -26, 0);
        MakeDot(card, 0, 0);
        MakeDot(card, 26, 0);

        const lv_font_t* font = ctx.font();
        if (font == nullptr) {
            font = &lv_font_montserrat_14;
        }

        lv_obj_t* title = lv_label_create(root);
        lv_label_set_text(title, kDemoInfo.name);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_font(title, font, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, 52);

        lv_obj_t* hint = lv_label_create(root);
        lv_label_set_text(hint, "单击 BOOT 键返回");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x8A8F98), 0);
        lv_obj_set_style_text_font(hint, font, 0);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 78);

        lv_obj_t* top_hint = lv_label_create(root);
        lv_label_set_text(top_hint, "顶部下拉：控制中心");
        lv_obj_set_style_text_color(top_hint, lv_color_hex(0x5A5F66), 0);
        lv_obj_set_style_text_font(top_hint, font, 0);
        lv_obj_align(top_hint, LV_ALIGN_BOTTOM_MID, 0, -44);
    }
};

}  // namespace

App* CreateDemoApp() {
    return new DemoApp();
}
