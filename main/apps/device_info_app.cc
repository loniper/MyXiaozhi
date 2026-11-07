// 设备信息：第二个真实应用，用来验证应用中心的磁贴点击与应用间切换。
#include "app.h"
#include "app_icons.h"

#include "boards/common/board.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"

#include <cstdio>
#include <cstring>
#include <lvgl.h>

extern const AppInfo kDeviceInfoInfo = {"device_info", "设备信息", 0x35D07A, DrawDeviceInfoIcon};

namespace {

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, lv_color_t color, const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

class DeviceInfoApp : public App {
public:
    const AppInfo& info() const override { return kDeviceInfoInfo; }

    void OnEnter(AppContext& ctx) override {
        lv_obj_t* root = ctx.root();
        lv_obj_set_style_bg_color(root, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

        const lv_font_t* font = ctx.font();
        if (font == nullptr) {
            font = &lv_font_montserrat_14;
        }

        lv_obj_t* title = MakeLabel(root, kDeviceInfoInfo.name, lv_color_white(), font);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 52);

        const lv_color_t kKey = lv_color_hex(0x8A8F98);
        const lv_color_t kValue = lv_color_white();

        AddRow(root, "版本", esp_app_get_description()->version, 104, kKey, kValue, font);
        AddRow(root, "电量", BatteryText().c_str(), 142, kKey, kValue, font);
        AddRow(root, "内存", HeapText().c_str(), 180, kKey, kValue, font);
        AddRow(root, "芯片", ChipText().c_str(), 218, kKey, kValue, font);

        lv_obj_t* hint = MakeLabel(root, "单击 BOOT 键返回", lv_color_hex(0x5A5F66), font);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -44);
    }

private:
    static void AddRow(lv_obj_t* root, const char* key, const char* value, int y, lv_color_t key_color,
                       lv_color_t value_color, const lv_font_t* font) {
        lv_obj_t* k = MakeLabel(root, key, key_color, font);
        lv_obj_set_pos(k, 78, y);
        lv_obj_t* v = MakeLabel(root, value, value_color, font);
        lv_obj_set_pos(v, 142, y);
        lv_obj_set_width(v, 176);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    }

    static std::string BatteryText() {
        int level = 0;
        bool charging = false;
        bool discharging = false;
        char buf[32];
        if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
            return "未知";
        }
        if (charging) {
            snprintf(buf, sizeof(buf), "%d%% 充电中", level);
        } else {
            snprintf(buf, sizeof(buf), "%d%%", level);
        }
        return buf;
    }

    static std::string HeapText() {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>(esp_get_free_heap_size() / 1024));
        return buf;
    }

    static std::string ChipText() {
#ifdef CONFIG_IDF_TARGET
        return CONFIG_IDF_TARGET;
#else
        return "ESP32-S3";
#endif
    }
};

}  // namespace

App* CreateDeviceInfoApp() {
    return new DeviceInfoApp();
}
