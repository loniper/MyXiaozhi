// 谷仓次元屏（SDGOODS Dimension Screen）xiaozhi-esp32 板型
// 硬件事实源：SDGOODS-ESP32S3/components/sdgoods_board
//   ESP32-S3 + 8MB Octal PSRAM + 32MB Flash
//   ST77916 QSPI 360x360 圆屏（屏厂初始化表取自 sdgoods_board/lcd/st77916_vendor_init.inc）
//   数字 MEMS 麦 + I2S 功放（无 codec 芯片，见 sdgoods_audio_codec.cc）
#include "wifi_board.h"
#include "sdgoods_audio_codec.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "application.h"
#include "apps/app_host.h"
#include "apps/radio/ui_radio.h"
#include "button.h"
#include "adc_battery_monitor.h"
#include "system_info.h"
#include "mcp_server.h"
#include "config.h"
#include "grokbot_avatar.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>  // adc_unit_t / ADC_CHANNEL_7（新 esp_adc 头，避免 legacy driver/adc.h 弃用警告）
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st77916.h>
#include <esp_lcd_touch.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_lvgl_port.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cctype>
#include <cstring>
#include <lvgl.h>

#define TAG "sdgoods_dimension_screen"

// 控制中心样式用字体（与原厂同源 Noto Sans SC，lv_font_conv 生成子集）：
// 16 号 = 标题/正文（对齐原厂 si_yuan_black_icon_16），14 号 = 瓷砖下方 caption（对齐 si_yuan_black_icon_14）。
// 控制中心页文案为中文，两支字体均覆盖 ASCII 0x20-0x7E + 所需汉字子集（控制中心设置…关于数据缓存等）。
// 图标不再用 Material 字体，改回原厂 canvas 手绘 48x48 白线稿（CcIconDraw）。
LV_FONT_DECLARE(font_cc_noto_16_4);
LV_FONT_DECLARE(font_cc_noto_14_4);

// 圆屏安全区：上游 LcdDisplay 是矩形布局，在 360x360 圆屏上四角内容会被圆形黑边裁掉。
// 这里在板级子类里覆写 SetupUI，对全宽元素按弦宽向内收紧（r=180，不改动上游通用代码）。
class SdgoodsRoundLcdDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    // SetupUI 完成后回调（板级用来挂控制中心浮层：必须在容器建好之后再叠在上层）
    std::function<void()> on_setup_done;

    // GrokBot 版小智：待机/聆听/思考/说话只显示形象与一枚顶部状态点，不显示任何文字；
    // 配网与联网阶段保留顶部提示（纯白文字、无底框）。
    enum class Phase { None, Listening, Thinking, Speaking };

    bool IsProvisioningStatus(const char* status) {
        return strcmp(status, Lang::Strings::WIFI_CONFIG_MODE) == 0 ||
               strcmp(status, Lang::Strings::SCANNING_WIFI) == 0 ||
               strcmp(status, Lang::Strings::ENTERING_WIFI_CONFIG_MODE) == 0;
    }
    void HideTopText() {
        if (grok_text_ != nullptr) {
            lv_label_set_text(grok_text_, "");
            lv_obj_add_flag(grok_text_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_label_ != nullptr) {
            lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (notification_label_ != nullptr) {
            lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    void ShowTopText(const char* text) {
        if (grok_text_ == nullptr || text == nullptr || text[0] == '\0') {
            return;
        }
        lv_label_set_text(grok_text_, text);
        lv_obj_remove_flag(grok_text_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(grok_text_);
    }

    // 顶部状态点：绿=聆听，黄=思考，白=说话。只在交互阶段出现。
    void SetDot(uint32_t color_hex, bool show) {
        if (grok_dot_ == nullptr) {
            return;
        }
        if (!show) {
            lv_obj_add_flag(grok_dot_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_color_t c = lv_color_hex(color_hex);
        lv_obj_set_style_bg_color(grok_dot_, c, 0);
        lv_obj_set_style_shadow_color(grok_dot_, c, 0);
        lv_obj_remove_flag(grok_dot_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(grok_dot_);
    }

    static bool IsClockString(const char* s) {
        // 状态栏待机时会推送 "HH:MM" 时钟；此版本待机不显示任何文字。
        return s != nullptr && strlen(s) == 5 && s[2] == ':' && isdigit(static_cast<unsigned char>(s[0])) &&
               isdigit(static_cast<unsigned char>(s[1])) && isdigit(static_cast<unsigned char>(s[3])) &&
               isdigit(static_cast<unsigned char>(s[4]));
    }

    virtual void SetStatus(const char* status) override {
        if (status == nullptr || IsClockString(status) || app_mode_) {
            return;
        }
        DisplayLockGuard lock(this);
        if (strcmp(status, Lang::Strings::LISTENING) == 0) {
            silent_phase_ = true;
            phase_ = Phase::Listening;
            HideTopText();
            if (grok_avatar_ != nullptr) {
                grok_avatar_->SetMode(GrokbotAvatar::Mode::Listening);
            }
            SetDot(0x35D07A, true);
            return;
        }
        if (strcmp(status, Lang::Strings::CONNECTING) == 0) {
            // 开音频通道 / 等待回复：思考阶段。
            silent_phase_ = true;
            phase_ = Phase::Thinking;
            HideTopText();
            if (grok_avatar_ != nullptr) {
                grok_avatar_->SetMode(GrokbotAvatar::Mode::Other);
                grok_avatar_->SetEmotion("thinking");
            }
            SetDot(0xFFC53D, true);
            return;
        }
        if (strcmp(status, Lang::Strings::SPEAKING) == 0) {
            silent_phase_ = true;
            phase_ = Phase::Speaking;
            HideTopText();
            if (grok_avatar_ != nullptr) {
                grok_avatar_->SetMode(GrokbotAvatar::Mode::Speaking);
            }
            SetDot(0xFFFFFF, true);
            return;
        }
        if (strcmp(status, Lang::Strings::STANDBY) == 0) {
            // 说完话到回复开始之间会先回到 Idle：按待机处理，不显示状态点。
            // （黄点只在 Connecting 阶段出现。）
            silent_phase_ = true;
            phase_ = Phase::None;
            HideTopText();
            if (grok_avatar_ != nullptr) {
                grok_avatar_->SetMode(GrokbotAvatar::Mode::Standby);
            }
            SetDot(0, false);
            return;
        }
        phase_ = Phase::None;
        silent_phase_ = false;
        SetDot(0, false);
        if (grok_avatar_ != nullptr) {
            grok_avatar_->SetMode(IsProvisioningStatus(status) ? GrokbotAvatar::Mode::Provisioning
                                                              : GrokbotAvatar::Mode::Other);
        }
        ShowTopText(status);
    }

    virtual void ShowNotification(const char* notification, int duration_ms = 3000) override {
        (void)duration_ms;
        if (silent_phase_ || notification == nullptr || app_mode_) {
            return;
        }
        DisplayLockGuard lock(this);
        ShowTopText(notification);
    }
    virtual void ShowNotification(const std::string& notification,
                                  int duration_ms = 3000) override {
        ShowNotification(notification.c_str(), duration_ms);
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        // 底部字幕条永久隐藏：所有可显示的提示都走顶部纯白文字。
        DisplayLockGuard lock(this);
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (silent_phase_ || content == nullptr || content[0] == '\0') {
            return;
        }
        if (role != nullptr && strcmp(role, "system") != 0) {
            return;
        }
        // SetupUI 之后的第一条是 User-Agent 调试信息，不打扰界面。
        if (SystemInfo::GetUserAgent() == content) {
            return;
        }
        ShowTopText(content);
    }

    virtual void ClearChatMessages() override {
        DisplayLockGuard lock(this);
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    virtual void SetEmotion(const char* emotion) override {
        if (grok_avatar_ == nullptr || emotion == nullptr || app_mode_) {
            return;
        }
        DisplayLockGuard lock(this);
        grok_avatar_->SetEmotion(emotion);
    }

    // 皮肤资源（工厂 assets 里的蓝色渐变底图 + 小机器人水印）会在换肤/加载字体时
    // 重新贴到 container_ 上，正好盖在开机睁眼动画下面。GrokBot 自己就是整屏，
    // 每次换肤后都把背景压回纯黑、并再次隐藏 AI 机器人图标。
    virtual void SetTheme(Theme* theme) override {
        DisplayLockGuard lock(this);
        // 关键：在基类之前就把皮肤底图摘掉。基类 SetTheme 会直接把
        // background_image 设到 container_ 上，之后再清掉时那一帧早已被刷到
        // 屏上（开机一瞬的蓝色锯齿底纹）。先清主题字段，基类拿到的就是无
        // 底图的主题，只会设纯黑底，从源头不可能再出现那张图。
        if (theme != nullptr) {
            auto* lvgl_theme = static_cast<LvglTheme*>(theme);
            lvgl_theme->set_background_image(nullptr);
            lvgl_theme->set_background_color(lv_color_hex(0x000000));
            lvgl_theme->set_chat_background_color(lv_color_hex(0x000000));
            // 应用框架的 UI 字体：主题自带的中文字体（支持任意汉字）。
            // 注意：动态字体重载时 LvglDisplay::SetTextFont 会替换主题字体并释放旧
            // LvglFont 对象，而 LVGL 样式里保存的是裸 lv_font_t*。应用层会跨很长的
            // 时间持有该指针，所以这里额外持有 shared_ptr 保活，避免应用一打开就
            // 使用已释放的字体（表现为进入应用瞬间重启）。
            auto text_font = lvgl_theme->text_font();
            if (text_font != nullptr && text_font->font() != nullptr) {
                ui_font_ = text_font->font();
                KeepFontAlive(text_font);
            } else {
                ui_font_ = nullptr;
            }
        }
        SpiLcdDisplay::SetTheme(theme);
        ApplyGrokBackdrop();
    }

    virtual void SetupUI() override {
        // 先锁渲染再建基础 UI：否则在创建状态栏/机器人标签的过程中，
        // 渲染任务会刷出一帧「蓝色底图 + 小机器人 + 正在初始化」，
        // 这就是开机时一闪而过的底纹。锁住后屏上只会出现我们收尾后的纯黑帧。
        DisplayLockGuard lock(this);
        SpiLcdDisplay::SetupUI();
        // 原生顶栏与状态栏整体移除：屏幕上不留任何底框，提示文字改用自建纯白标签。
        if (top_bar_ != nullptr) {
            lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_bar_ != nullptr) {
            lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        if (notification_label_ != nullptr) {
            lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
        }
        // 聊天区（仅微信气泡样式才有；默认大 emoji 样式下 content_ 为 null，此段空转）
        if (content_ != nullptr) {
            lv_obj_set_style_pad_left(content_, 44, 0);
            lv_obj_set_style_pad_right(content_, 44, 0);
        }
        // 底部字幕条永久隐藏：GrokBot 版只在顶部用纯白文字提示，互动阶段不显示任何文字。
        if (bottom_bar_ != nullptr) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        // 顶部提示：去掉状态栏底框，文字统一纯白（形象为深色，白字直接浮在上面）。
        if (top_bar_ != nullptr) {
            lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        }
        lv_obj_t* const icon_labels[] = {network_label_, mute_label_, battery_label_};
        for (auto label : icon_labels) {
            if (label != nullptr) {
                lv_obj_set_style_text_color(label, lv_color_white(), 0);
            }
        }
        if (status_label_ != nullptr) {
            lv_obj_set_width(status_label_, 240);
            lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_color(status_label_, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(status_label_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
        }
        if (notification_label_ != nullptr) {
            lv_obj_set_width(notification_label_, 240);
            lv_obj_set_style_text_color(notification_label_, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(notification_label_, LV_OPA_TRANSP, 0);
        }
        // 原 emoji 层彻底隐藏，改由 GrokBot 自绘形象占满整屏。
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_label_ != nullptr) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        // 低电提示（底部 y≈320..340，弦宽急剧变窄）：缩窄并上提
        if (low_battery_popup_ != nullptr) {
            lv_obj_set_width(low_battery_popup_, 160);
            lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -48);
        }
        if (grok_avatar_ == nullptr) {
            grok_avatar_ = new GrokbotAvatar();
            grok_avatar_->Create(lv_screen_active(), width_, height_);
            // 形象整屏自绘（黑底 + 眼睛），必须压在皮肤底图/机器人图标之上；
            // 顶部提示与控制中心随后再 move_foreground，仍会盖在它上面。
            lv_obj_move_foreground(grok_avatar_->obj());
            // 低电提示平时隐藏，展示时要浮在形象之上（形象是整屏不透明层）。
            if (low_battery_popup_ != nullptr) {
                lv_obj_move_foreground(low_battery_popup_);
            }
        }
        // 自建顶部提示：纯白文字、无底框、无边框（只用于配网/联网等非交互阶段）。
        if (grok_text_ == nullptr) {
            grok_text_ = lv_label_create(lv_screen_active());
            lv_obj_set_width(grok_text_, 176);
            lv_label_set_long_mode(grok_text_, LV_LABEL_LONG_WRAP);
            lv_obj_set_style_text_align(grok_text_, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(grok_text_, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(grok_text_, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(grok_text_, 0, 0);
            lv_obj_set_style_pad_all(grok_text_, 0, 0);
            lv_obj_align(grok_text_, LV_ALIGN_TOP_MID, 0, 18);
            lv_obj_add_flag(grok_text_, LV_OBJ_FLAG_HIDDEN);
        }
        // 交互状态点：14px 圆点 + 同色柔光，绿=聆听 / 黄=思考 / 白=说话。
        if (grok_dot_ == nullptr) {
            grok_dot_ = lv_obj_create(lv_screen_active());
            lv_obj_remove_style_all(grok_dot_);
            lv_obj_set_size(grok_dot_, 14, 14);
            lv_obj_set_style_radius(grok_dot_, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(grok_dot_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(grok_dot_, lv_color_hex(0x35D07A), 0);
            lv_obj_set_style_shadow_width(grok_dot_, 18, 0);
            lv_obj_set_style_shadow_spread(grok_dot_, 3, 0);
            lv_obj_set_style_shadow_opa(grok_dot_, 120, 0);
            lv_obj_set_style_shadow_color(grok_dot_, lv_color_hex(0x35D07A), 0);
            lv_obj_align(grok_dot_, LV_ALIGN_TOP_MID, 0, 36);
            lv_obj_clear_flag(grok_dot_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(grok_dot_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_move_foreground(grok_text_);
        lv_obj_move_foreground(grok_dot_);
        ApplyGrokBackdrop();
        if (on_setup_done) {
            on_setup_done();
        }
        BootLightUp();
    }

    // 开机点亮：先把「纯黑 + 闭眼」刷进面板 GRAM，再开背光，最后才睁眼。
    // 这样开机阶段的所有残留画面（厂方蓝底图、面板上电雪花、皮肤底图那一帧）
    // 都发生在背光关闭期间，用户看到的第一帧就是我们画的黑底闭眼。
    void BootLightUp() {
        lv_obj_invalidate(lv_screen_active());
        lv_timer_create([](lv_timer_t* t) {
            auto* self = static_cast<SdgoodsRoundLcdDisplay*>(lv_timer_get_user_data(t));
            Backlight* backlight = Board::GetInstance().GetBacklight();
            if (backlight != nullptr) {
                backlight->RestoreBrightness();
            }
            if (self->grok_avatar_ != nullptr) {
                self->grok_avatar_->StartIntro();
            }
            lv_timer_delete(t);
        }, 90, this);
    }

public:
    // 控制中心等全屏浮层打开时暂停形象刷新，避免无意义的全屏重绘。
    void SetGrokPaused(bool paused) {
        if (grok_avatar_ != nullptr) {
            grok_avatar_->SetPaused(paused);
        }
    }

    // 应用（应用中心/设备信息等）使用的主题字体，可能为空（调用方自行回退）。
    const lv_font_t* UiFont() const {
        if (ui_font_ != nullptr) {
            return ui_font_;
        }
        return grok_text_ != nullptr ? lv_obj_get_style_text_font(grok_text_, LV_PART_MAIN) : nullptr;
    }

    // 应用模式：应用独占屏幕时隐藏形象、状态点与顶部文字；退出时恢复待机形象。
    void SetAssistantSurface(bool visible) {
        app_mode_ = !visible;
        if (visible) {
            if (grok_avatar_ != nullptr) {
                lv_obj_remove_flag(grok_avatar_->obj(), LV_OBJ_FLAG_HIDDEN);
                grok_avatar_->SetMode(GrokbotAvatar::Mode::Standby);
                grok_avatar_->SetPaused(false);
            }
        } else if (grok_avatar_ != nullptr) {
            grok_avatar_->SetPaused(true);
            lv_obj_add_flag(grok_avatar_->obj(), LV_OBJ_FLAG_HIDDEN);
        }
        HideTopText();
        SetDot(0, false);
    }

private:
    // 主题字体可能在运行期被动态字体重载替换并释放；应用层持有裸指针，
    // 因此把用过的字体对象都保活（去重，最多保留最近 4 个）。
    void KeepFontAlive(const std::shared_ptr<LvglFont>& font) {
        for (const auto& kept : app_font_keepalive_) {
            if (kept.get() == font.get()) {
                return;
            }
        }
        app_font_keepalive_.push_back(font);
        if (app_font_keepalive_.size() > 4) {
            app_font_keepalive_.erase(app_font_keepalive_.begin());
        }
    }

    // 开机只留一张纯黑底：screen 与 container 都清掉皮肤背景图（工厂 assets
    // 的蓝色渐变 + 小机器人水印就是从这里冒出来的），并再次藏掉 AI 机器人图标。
    void ApplyGrokBackdrop() {
        lv_obj_t* screen = lv_screen_active();
        if (screen != nullptr) {
            lv_obj_set_style_bg_image_src(screen, nullptr, 0);
            lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
        }
        if (container_ != nullptr) {
            lv_obj_set_style_bg_image_src(container_, nullptr, 0);
            lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(container_, lv_color_hex(0x000000), 0);
        }
        if (emoji_box_ != nullptr) {
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_label_ != nullptr) {
            lv_label_set_text(emoji_label_, "");
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_image_ != nullptr) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (screen != nullptr) {
            lv_obj_invalidate(screen);
        }
    }

    GrokbotAvatar* grok_avatar_ = nullptr;
    lv_obj_t* grok_text_ = nullptr;
    lv_obj_t* grok_dot_ = nullptr;
    Phase phase_ = Phase::None;
    bool silent_phase_ = false;
    // 应用独占屏幕：此后所有小智状态文案/状态点/表情更新都被忽略，
    // 直到应用退出（SetAssistantSurface(true)）。防止后台会话把 UI 抢回来。
    bool app_mode_ = false;
    const lv_font_t* ui_font_ = nullptr;   // 主题中文字体（应用 UI 使用）
    std::vector<std::shared_ptr<LvglFont>> app_font_keepalive_;   // 防止 ui_font_ 悬空
};

// ST77916 QSPI 屏厂初始化序列（谷仓次元屏实测可用）
static const st77916_lcd_init_cmd_t vendor_specific_init_sdgoods[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x73, (uint8_t []){0xF0}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x56}, 1, 0},
    {0xB1, (uint8_t []){0x4D}, 1, 0},
    {0xB2, (uint8_t []){0x24}, 1, 0},
    {0xB4, (uint8_t []){0x87}, 1, 0},
    {0xB5, (uint8_t []){0x44}, 1, 0},
    {0xB6, (uint8_t []){0x8B}, 1, 0},
    {0xB7, (uint8_t []){0x40}, 1, 0},
    {0xB8, (uint8_t []){0x86}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t []){0x4F}, 1, 0},
    {0xDE, (uint8_t []){0x4F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x07}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x00}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x01}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x02}, 1, 0},
    {0xEA, (uint8_t []){0xDA}, 1, 0},
    {0xEB, (uint8_t []){0x00}, 1, 0},
    {0xEC, (uint8_t []){0x00}, 1, 0},
    {0xED, (uint8_t []){0x0F}, 1, 0},
    {0xEE, (uint8_t []){0x00}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0x00}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x00}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x40}, 1, 0},
    {0x61, (uint8_t []){0x04}, 1, 0},
    {0x62, (uint8_t []){0x00}, 1, 0},
    {0x63, (uint8_t []){0x42}, 1, 0},
    {0x64, (uint8_t []){0xD9}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x00}, 1, 0},
    {0x67, (uint8_t []){0x00}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x40}, 1, 0},
    {0x71, (uint8_t []){0x03}, 1, 0},
    {0x72, (uint8_t []){0x00}, 1, 0},
    {0x73, (uint8_t []){0x42}, 1, 0},
    {0x74, (uint8_t []){0xD8}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x00}, 1, 0},
    {0x77, (uint8_t []){0x00}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    {0x80, (uint8_t []){0x48}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    {0x82, (uint8_t []){0x06}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    {0x84, (uint8_t []){0xD6}, 1, 0},
    {0x85, (uint8_t []){0x04}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    {0x88, (uint8_t []){0x48}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    {0x8A, (uint8_t []){0x08}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    {0x8C, (uint8_t []){0xD8}, 1, 0},
    {0x8D, (uint8_t []){0x04}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    {0x90, (uint8_t []){0x48}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    {0x92, (uint8_t []){0x0A}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    {0x94, (uint8_t []){0xDA}, 1, 0},
    {0x95, (uint8_t []){0x04}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    {0x98, (uint8_t []){0x48}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    {0x9A, (uint8_t []){0x0C}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    {0x9C, (uint8_t []){0xDC}, 1, 0},
    {0x9D, (uint8_t []){0x04}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    {0xA0, (uint8_t []){0x48}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    {0xA2, (uint8_t []){0x05}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    {0xA4, (uint8_t []){0xD5}, 1, 0},
    {0xA5, (uint8_t []){0x04}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    {0xA8, (uint8_t []){0x48}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    {0xAA, (uint8_t []){0x07}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    {0xAC, (uint8_t []){0xD7}, 1, 0},
    {0xAD, (uint8_t []){0x04}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    {0xB0, (uint8_t []){0x48}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    {0xB2, (uint8_t []){0x09}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0xD9}, 1, 0},
    {0xB5, (uint8_t []){0x04}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    {0xB8, (uint8_t []){0x48}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    {0xBA, (uint8_t []){0x0B}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    {0xBC, (uint8_t []){0xDB}, 1, 0},
    {0xBD, (uint8_t []){0x04}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x10}, 1, 0},
    {0xC1, (uint8_t []){0x47}, 1, 0},
    {0xC2, (uint8_t []){0x56}, 1, 0},
    {0xC3, (uint8_t []){0x65}, 1, 0},
    {0xC4, (uint8_t []){0x74}, 1, 0},
    {0xC5, (uint8_t []){0x88}, 1, 0},
    {0xC6, (uint8_t []){0x99}, 1, 0},
    {0xC7, (uint8_t []){0x01}, 1, 0},
    {0xC8, (uint8_t []){0xBB}, 1, 0},
    {0xC9, (uint8_t []){0xAA}, 1, 0},
    {0xD0, (uint8_t []){0x10}, 1, 0},
    {0xD1, (uint8_t []){0x47}, 1, 0},
    {0xD2, (uint8_t []){0x56}, 1, 0},
    {0xD3, (uint8_t []){0x65}, 1, 0},
    {0xD4, (uint8_t []){0x74}, 1, 0},
    {0xD5, (uint8_t []){0x88}, 1, 0},
    {0xD6, (uint8_t []){0x99}, 1, 0},
    {0xD7, (uint8_t []){0x01}, 1, 0},
    {0xD8, (uint8_t []){0xBB}, 1, 0},
    {0xD9, (uint8_t []){0xAA}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 1, 0},
    {0x11, (uint8_t []){0x00}, 1, 120},
    /* TEON：参数 0x00 = 仅 V-blank 期间输出脉冲。
     * 保留这条厂商初始化命令，但本机 GPIO18 上没有测到 TE 信号（参考工程
     * sdgoods_lvgl.c 也注明「面板没有 VSYNC/TE 门控」），所以刷新无法与面板扫描
     * 对齐；整屏跳变的撕裂改用背光遮盖写入窗口来消除，见 lcd_display.cc。 */
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x29, (uint8_t []){0x00}, 1, 0},
};

class SdgoodsDimensionScreen : public WifiBoard {
private:
    Button boot_button_;
    SdgoodsRoundLcdDisplay* display_;
    AppHost app_host_;   // 应用框架：应用层 + 单应用生命周期（见 main/apps）
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t imu_dev_ = nullptr;   // QMI8658（手电“摇一摇”用）
    bool imu_ready_ = false;
    AdcBatteryMonitor* battery_monitor_ = nullptr;

    // 控制中心浮层（照抄原厂 sdgoods_cc.c 的 iOS 风格：满屏纯黑 + 圆形瓷砖 + 二级页）
    lv_obj_t* cc_page_ = nullptr;        // 一级页（控制中心）
    lv_obj_t* cc_set_page_ = nullptr;    // 二级：设置（音量/亮度/网络/电池）
    lv_obj_t* cc_slider_page_ = nullptr; // 二级：滑块页（音量/亮度）
    lv_obj_t* cc_data_page_ = nullptr;   // 二级：网络数据页
    lv_obj_t* cc_battery_page_ = nullptr;// 二级：电量页
    lv_obj_t* cc_about_page_ = nullptr;  // 二级：关于设备页
    lv_obj_t* cc_slider_value_label_ = nullptr; // 滑块页顶部数值
    bool cc_slider_is_volume_ = true;            // 当前滑块页是音量还是亮度
    lv_coord_t cc_gesture_start_y_ = 0;

    // 熄屏（语音“关闭屏幕”的友好形态：降背光 + 全黑页触摸即醒，避免误以为死机）
    lv_obj_t* cc_dim_page_ = nullptr;
    int saved_brightness_ = -1;   // 熄屏前的背光档位（-1 = 无记录，唤醒时从 settings 恢复）

    void InitializePowerLatch() {
        // 电池供电自锁：开机立即拉高 GPIO7 保持上电，否则松开电源键就断电（USB 供电时看不出，
        // 电池供电时表现为“只能按住才开机”）。与 SDGOODS BSP main.c 开机顺序一致。
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << BAT_CONTROL_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(BAT_CONTROL_GPIO, BAT_CONTROL_LATCH_LEVEL);
        ESP_LOGI(TAG, "Battery power latch engaged (GPIO%d high)", BAT_CONTROL_GPIO);
    }

    void InitializeDisplayPower() {
        // 次元屏屏电源使能：GPIO12 拉低上电（与 SDGOODS BSP 一致）
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << DISPLAY_POWER_GPIO,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(DISPLAY_POWER_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize QSPI bus");
        const spi_bus_config_t bus_config = TAIJIPI_ST77916_PANEL_BUS_QSPI_CONFIG(QSPI_PIN_NUM_LCD_PCLK,
                                                                        QSPI_PIN_NUM_LCD_DATA0,
                                                                        QSPI_PIN_NUM_LCD_DATA1,
                                                                        QSPI_PIN_NUM_LCD_DATA2,
                                                                        QSPI_PIN_NUM_LCD_DATA3,
                                                                        DISPLAY_SPI_MAX_TRANSFER_SZ);
        ESP_ERROR_CHECK(spi_bus_initialize(QSPI_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));
    }

    void Initializest77916Display() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = QSPI_PIN_NUM_LCD_CS,
            .dc_gpio_num = GPIO_NUM_NC,
            .spi_mode = 0,
            .pclk_hz = DISPLAY_SPI_PCLK_HZ,
            .trans_queue_depth = DISPLAY_SPI_TRANS_QUEUE_DEPTH,
            .on_color_trans_done = NULL,
            .user_ctx = NULL,
            .lcd_cmd_bits = 32,
            .lcd_param_bits = 8,
            .flags = {
                .dc_low_on_data = 0,
                .octal_mode = 0,
                .quad_mode = 1,
                .sio_mode = 0,
                .lsb_first = 0,
                .cs_high_active = 0,
            },
        };
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)QSPI_LCD_HOST, &io_config, &panel_io));

        ESP_LOGI(TAG, "Install ST77916 panel driver");
        st77916_vendor_config_t vendor_config = {};
        vendor_config.init_cmds = vendor_specific_init_sdgoods;
        vendor_config.init_cmds_size = sizeof(vendor_specific_init_sdgoods) / sizeof(st77916_lcd_init_cmd_t);
        vendor_config.flags.use_qspi_interface = 1;

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = QSPI_LCD_BIT_PER_PIXEL;
        panel_config.reset_gpio_num = QSPI_PIN_NUM_LCD_RST;
        panel_config.vendor_config = &vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(panel_io, &panel_config, &panel));

        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_disp_on_off(panel, true);
        esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        // 面板一开就先把 GRAM 刷成近黑：上电后到 LVGL 首次整屏刷新之间有几秒，
        // 期间屏上会留着厂方开机画面（蓝色渐变 + 小机器人）。这里用 DMA 条带
        // 直接清屏，让「开机」从第一帧就是纯黑底。
        {
            constexpr int kStrip = 8;
            size_t strip_px = static_cast<size_t>(DISPLAY_WIDTH) * kStrip;
            uint16_t* strip =
                static_cast<uint16_t*>(heap_caps_malloc(strip_px * sizeof(uint16_t),
                                                        MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (strip != nullptr) {
                for (size_t i = 0; i < strip_px; i++) {
                    strip[i] = 0x0000;  // RGB565 近黑
                }
                for (int y = 0; y < DISPLAY_HEIGHT; y += kStrip) {
                    int y2 = y + kStrip;
                    if (y2 > DISPLAY_HEIGHT) {
                        y2 = DISPLAY_HEIGHT;
                    }
                    esp_lcd_panel_draw_bitmap(panel, 0, y, DISPLAY_WIDTH, y2, strip);
                }
                heap_caps_free(strip);
            }
        }

        display_ = new SdgoodsRoundLcdDisplay(panel_io, panel,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                    /*fullscreen_psram_buf=*/true);  // 整屏 PSRAM 双缓冲：一帧只做一次整屏 flush（见 lcd_display.cc / config.h 的说明）
        // 上游 SetupUI 由 Application::Start 触发，建完容器后先叠应用层，再叠控制中心浮层
        display_->on_setup_done = [this]() {
            SetupAppLayer();
            SetupControlCenter();
        };
    }

    void InitializeI2c() {
        // 触摸专用 I2C 总线（新驱动）：SDA=11 / SCL=10
        i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = TOUCH_I2C_SDA_PIN,
            .scl_io_num = TOUCH_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        // 触摸不是致命路径：失败只记日志，不 crash（语音交互仍可用）
        if (i2c_new_master_bus(&bus_cfg, &i2c_bus_) != ESP_OK) {
            i2c_bus_ = nullptr;
            ESP_LOGE(TAG, "I2C bus init failed, touch disabled");
        }
        InitializeImu();
    }

    void InitializeTouch() {
        if (i2c_bus_ == nullptr) {
            return;
        }
        esp_lcd_touch_handle_t tp = nullptr;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = TOUCH_GPIO_RST,
            .int_gpio_num = TOUCH_GPIO_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                // 屏做了 swap_xy + mirror_x 硬件旋转，触摸原始坐标（面板坐标系）需要同变换。
                // esp_lcd_touch 软件变换顺序：先 mirror 后 swap，即 (x,y)->(y, 359-x)，
                // 与原厂 map_raw_to_lvgl（out_x=raw_y, out_y=W-1-raw_x）一致；若实机方向不对再调
                .swap_xy = 1,
                .mirror_x = 1,
                .mirror_y = 0,
            },
        };
        esp_lcd_panel_io_handle_t tp_io = nullptr;
        esp_lcd_panel_io_i2c_config_t io_cfg = {};
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS;
        io_cfg.scl_speed_hz = 400 * 1000;
        io_cfg.control_phase_bytes = 1;
        io_cfg.dc_bit_offset = 0;
        io_cfg.lcd_cmd_bits = 8;
        io_cfg.lcd_param_bits = 0;
        io_cfg.flags.disable_control_phase = 1;
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &io_cfg, &tp_io) != ESP_OK) {
            ESP_LOGE(TAG, "Touch panel IO init failed");
            return;
        }
        if (esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp) != ESP_OK) {
            ESP_LOGE(TAG, "CST816S init failed");
            return;
        }
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "CST816S touch ready (I2C addr 0x%02X)", ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS);
    }

    // ---------- QMI8658 六轴 IMU（与触摸共用 I2C1）：供手电的“摇一摇”使用 ----------
    bool ImuWrite(uint8_t reg, uint8_t value) {
        uint8_t buf[2] = {reg, value};
        return i2c_master_transmit(imu_dev_, buf, sizeof(buf), 50) == ESP_OK;
    }

    void InitializeImu() {
        if (i2c_bus_ == nullptr) {
            return;
        }
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = 0x6B;
        dev_cfg.scl_speed_hz = 400 * 1000;
        if (i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &imu_dev_) != ESP_OK) {
            imu_dev_ = nullptr;
            return;
        }
        // 与原厂 BSP 相同的初始化：地址自增；加速度 ±4g/250Hz；陀螺 ±64dps/250Hz；六轴使能
        imu_ready_ = ImuWrite(0x02, 0x40) && ImuWrite(0x03, 0x15) &&
                     ImuWrite(0x04, 0x25) && ImuWrite(0x08, 0x43);
        ESP_LOGI(TAG, "QMI8658 IMU %s", imu_ready_ ? "ready" : "not found");
    }

    // 关机：熄屏 -> 释放电池自锁闩断电；USB 供电时落到深睡兑底（与原厂 sdgoods_power_off 同构）
    void PowerOff() {
        ESP_LOGW(TAG, "Power off: releasing battery latch GPIO%d", BAT_CONTROL_GPIO);
        GetBacklight()->SetBrightness(0);
        vTaskDelay(pdMS_TO_TICKS(800));  // 等背光渐变完全熄灭
        gpio_set_level(BAT_CONTROL_GPIO, BAT_CONTROL_LATCH_LEVEL ? 0 : 1);
        esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0);  // 电源键低有效唤醒
        esp_deep_sleep_start();
    }
    
    // 熄屏：不全黑“装死机”，而是降背光 + 全黑页常驻“轻触唤醒”提示，触摸任意位置即醒。
    // 语音“关闭屏幕”走这里（MCP self.screen.sleep），而不是 set_brightness(0) 真黑屏。
    void ScreenOff() {
        if (cc_dim_page_ != nullptr) {
            return;
        }
        DeleteAllCcPages();  // 先收掉控制中心浮层，熄屏页才是最上层
        int cur = GetBacklight()->brightness();
        saved_brightness_ = cur > 0 ? cur : -1;
        GetBacklight()->SetBrightness(4);  // 保留微亮：页面提示可见 + 触摸可唤醒 + 省电
        auto page = lv_obj_create(lv_screen_active());
        cc_dim_page_ = page;
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_style_bg_color(page, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(page);   // 盖住顶部下滑捕获条，熄屏时误滑不开控制中心
        auto lbl = lv_label_create(page);
        lv_label_set_text(lbl, "熄屏中\n轻触屏幕唤醒");
        lv_obj_set_style_text_font(lbl, &font_cc_noto_16_4, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x555A66), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(page, OnDimTouched, LV_EVENT_CLICKED, this);
        ESP_LOGI(TAG, "Screen off (dim mode), wake by touch");
    }
    
    static void OnDimTouched(lv_event_t* e) {
        auto self = static_cast<SdgoodsDimensionScreen*>(lv_event_get_user_data(e));
        // CLICKED 在按下分发中会删页？不会——CLICKED 在松手后派发；但为与全局惯例一致仍走 async
        lv_async_call([](void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->ScreenWake(); }, self);
    }
    
    void ScreenWake() {
        if (cc_dim_page_ != nullptr) {
            lv_obj_delete(cc_dim_page_);
            cc_dim_page_ = nullptr;
        }
        if (saved_brightness_ >= 0) {
            GetBacklight()->SetBrightness(static_cast<uint8_t>(saved_brightness_));
            saved_brightness_ = -1;
        } else {
            GetBacklight()->RestoreBrightness();  // 无记录（如开机即熄屏）：回到 settings 档位
        }
        ESP_LOGI(TAG, "Screen wake");
    }

    // 覆写 Board 虚函数：上游通用 MCP 工具 self.screen.set_brightness 会透传到这里。
    // 0 = 进触摸唤醒的熄屏页（而非真关背光被误认为死机）；非 0 若正在熄屏则先唤醒再调亮度。
    virtual void SetScreenBrightness(uint8_t brightness) override {
        if (cc_dim_page_ != nullptr) {
            ScreenWake();
        }
        if (brightness == 0) {
            ScreenOff();
            return;
        }
        GetBacklight()->SetBrightness(brightness, true);  // permanent：存 settings
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            // 应用内：唯一实体键 = 返回桌面（规则 1/7），小智在此阶段不参与。
            if (app_host_.IsActive()) {
                ExitApp();
                return;
            }
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        // 双击：仅在小智待机（Idle）桌面生效 → 进入应用中心。
        // 触发条件刻意收得很窄，保证不影响任何既有行为：
        //   - 只在 kDeviceStateIdle 响应（启动/配网/连接/对话中全部忽略）；
        //   - 应用内忽略（应用内单击仍是返回桌面）；
        //   - 控制中心浮层打开时忽略（不去抢它的页面栈）；
        //   - 熄屏待机页在最上层，先唤醒再进应用中心，否则应用会开在熄屏页后面。
        // 单击/长按的语义完全不变：iot_button 判定为双击时只发 BUTTON_DOUBLE_CLICK，
        // 不会再发 BUTTON_SINGLE_CLICK（双击窗口由 short_press_time 决定）。
        boot_button_.OnDoubleClick([this]() {
            if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
                return;
            }
            Application::GetInstance().Schedule([this]() {
                if (display_ == nullptr) {
                    return;
                }
                DisplayLockGuard lock(display_);
                if (app_host_.IsActive() || cc_page_ != nullptr || cc_set_page_ != nullptr ||
                    cc_slider_page_ != nullptr) {
                    return;
                }
                if (cc_dim_page_ != nullptr) {
                    ScreenWake();
                }
                LaunchAppLocked("app_center");
                ESP_LOGI(TAG, "double click -> app center");
            });
        });
        // 长按 ≥1.5s：释放自锁闩关机（LONG_PRESS_START 触发一次，无需等松手）
        boot_button_.OnLongPress([this]() {
            PowerOff();
        });
    }

    // 语音控制工具：self.screen.sleep / self.screen.wake（名字唯一，不与上游冲突）。
    // 上游通用 self.screen.set_brightness 已改为透传 Board::SetScreenBrightness（见本类覆写），
    // “亮度调到 0”不再真关背光装死机，而是进微亮熄屏页触摸即醒。
    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.screen.sleep",
            "Turn off the screen (dim standby). Call when user says turn off / close the screen (关闭屏幕/熄屏). The screen wakes up by touching it.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ScreenOff();
                return true;
            });
        mcp_server.AddTool("self.screen.wake",
            "Wake up the screen from dim standby. Call when user says turn on / open the screen (打开屏幕/唤醒屏幕).",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ScreenWake();
                return true;
            });

        // ---- 应用框架（M2）：应用中心 + 语音入口 ----
        mcp_server.AddTool("self.apps.center",
            "Open the app center on the device screen. Call when the user asks to show/see their apps, "
            "open the app center or list apps on the screen (显示我的应用/我有哪些应用/进入应用中心/打开应用列表).",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                LaunchApp("app_center");
                return true;
            });
        mcp_server.AddTool("self.apps.list",
            "List the apps installed on this device. Call when the user asks what apps are available "
            "(我的应用/我有哪些应用/打开应用列表). Do not open the app center for a named app; use self.apps.launch.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                // 只返回清单，不再顺带打开应用中心：否则“打开网络电台”时模型先列应用，
                // 用户看到的就是应用中心（真正要开的 App 反而没进）。
                int count = 0;
                const AppEntry* entries = GetAppRegistry(&count);
                std::string text;
                for (int i = 0; i < count; ++i) {
                    if (entries[i].info == nullptr || entries[i].info->id == nullptr) {
                        continue;
                    }
                    if (!text.empty()) {
                        text += "; ";
                    }
                    text += entries[i].info->id;
                    text += " = ";
                    text += entries[i].info->name;
                }
                return text;
            });
        std::string launch_desc =
            "Open one specific app on the device screen. Accepts the app id or its Chinese name "
            "(e.g. \"radio\" or \"网络电台\"). Call this whenever the user names an app; do NOT call "
            "self.apps.center for a named app. Installed apps: ";
        {
            int count = 0;
            const AppEntry* entries = GetAppRegistry(&count);
            for (int i = 0; i < count; ++i) {
                if (entries[i].info == nullptr || entries[i].info->id == nullptr) {
                    continue;
                }
                if (i > 0) {
                    launch_desc += "; ";
                }
                launch_desc += entries[i].info->id;
                launch_desc += " = ";
                launch_desc += entries[i].info->name;
            }
        }
        mcp_server.AddTool("self.apps.launch", launch_desc,
            PropertyList({Property("id", kPropertyTypeString)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string id = properties["id"].value<std::string>();
                LaunchApp(id.c_str());
                return true;
            });
        mcp_server.AddTool("self.apps.exit",
            "Close the running app and return to the assistant desktop.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ExitApp();
                return true;
            });
    }

    // ---------- 应用框架（M1）：应用层 + 进入/退出编排 ----------
    // 层级：形象 < 应用层(app_host_) < 控制中心页。应用层建立在控制中心之前，
    // 所以控制中心永远盖在应用之上（规则 4）。
    void SetupAppLayer() {
        AppHost::Hooks hooks;
        hooks.on_enter = [this]() { EnterAppSurface(); };
        hooks.on_exit = [this]() { LeaveAppSurface(); };
        app_host_.Init(lv_screen_active(), DISPLAY_WIDTH, DISPLAY_HEIGHT, display_->UiFont(), hooks);
        // 开机即后台预热电台目录：第一次进电台也不必等网络（有新鲜缓存时只读 NVS）。
        radio_startup_warmup();
    }

    // 进入应用：关掉控制中心 → 藏形象/状态点/文字 → 结束会话并关闭唤醒词闸门。
    void EnterAppSurface() {
        DeleteAllCcPages();
        display_->SetAssistantSurface(false);
        Application::GetInstance().SetInteractionSuppressed(true);
    }

    void LeaveAppSurface() {
        display_->SetAssistantSurface(true);
        Application::GetInstance().SetInteractionSuppressed(false);
    }

    // 调用方已持 LVGL 锁。
    bool LaunchAppLocked(const char* id) {
        auto state = Application::GetInstance().GetDeviceState();
        if (state == kDeviceStateStarting || state == kDeviceStateActivating ||
            state == kDeviceStateWifiConfiguring) {
            return false;   // 启动/配网阶段不接管屏幕
        }
        // 每次进入都现取主题字体：动态字体重载后旧指针可能已释放，
        // 不能沿用 Init 时捕获的那一个。
        app_host_.SetFont(display_->UiFont());
        // 语音在应用内切换：走 SwitchToAsync（内部 suppress_exit_hook + DoExit），
        // 不能 Exit()->Enter()：Exit 会恢复助理界面（on_exit），再把屏幕交回桌面，
        // 于是看起来还是停在应用中心；紧接着 Enter 还可能和界面恢复竞争导致重启。
        if (app_host_.IsActive()) {
            ESP_LOGI(TAG, "app switch -> '%s'", id);
            app_host_.SwitchToAsync(id);
            return true;
        }
        bool ok = app_host_.Enter(id);
        ESP_LOGI(TAG, "app open '%s' -> %d", id, (int)ok);
        return ok;
    }

    // 语音/外部入口：任意任务可调用（切回主任务并加 LVGL 锁）。
    void LaunchApp(const char* id) {
        std::string app_id = id != nullptr ? id : "";
        Application::GetInstance().Schedule([this, app_id]() {
            if (display_ == nullptr) {
                return;
            }
            DisplayLockGuard lock(display_);
            LaunchAppLocked(app_id.c_str());
        });
    }

    void ExitApp() {
        Application::GetInstance().Schedule([this]() {
            if (display_ == nullptr) {
                return;
            }
            DisplayLockGuard lock(display_);
            app_host_.Exit();
        });
    }

    // ---------- 下拉控制中心（1:1 照搬原厂 sdgoods_cc.c） ----------
    // 视觉语言：满屏纯黑页 + 直径 68 圆形瓷砖（#2C2C2E，按下 #3A3A3C）+ canvas 手绘
    // 白线稿图标（zoom 208）+ 下方 14 号灰 caption(#E0E0E5) + 底部灰色 home 小横条；
    // 一级页「设置 / 关于 / 关机」，设置页「音量 / 亮度 / 网络 / 电池 / 熄屏」，
    // 点开进二级滑块/数据/电量/关于页（文案与几何均对齐原厂）。
    // lv_style_selector_t 是 uint32_t（part<<16 | state），直接用枚举常量组合
    static constexpr lv_style_selector_t kSel() { return LV_PART_MAIN; }
    static constexpr lv_style_selector_t kSelPressed() {
        return static_cast<lv_style_selector_t>(LV_PART_MAIN) |
               static_cast<lv_style_selector_t>(LV_STATE_PRESSED);
    }
    static constexpr lv_style_selector_t kSelIndicator() { return LV_PART_INDICATOR; }
    static constexpr lv_style_selector_t kSelKnob() { return LV_PART_KNOB; }

    // 满屏不透明黑页（原厂：菜单页不漏出主屏）。刻意不做淡入/淡出——
    // 主界面是白底、本页是纯黑，半透明过渡会被部分缓冲逐格刷出，肉眼看到
    // “白→灰→黑”的渐进过程，反而被误判成卡顿；原厂就是瞬间整屏黑，干脆利落。
    lv_obj_t* CreateCcPage() {
        if (display_ != nullptr) {
            display_->SetGrokPaused(true);  // 全屏黑底浮层：无需继续渲染形象
        }
        app_host_.PauseForOverlay(true);    // 应用在控制中心打开期间暂停（规则 4）
        auto page = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(page);
        lv_obj_set_size(page, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_style_bg_color(page, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
        lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_move_foreground(page);
        return page;
    }

    // 页标题：白色 16 号字，y=40（原厂标题同一位置）
    void CreateCcTitle(lv_obj_t* page, const char* text) {
        auto title = lv_label_create(page);
        lv_label_set_text(title, text);
        lv_obj_set_style_text_font(title, &font_cc_noto_16_4, 0);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);  // 触摸穿透，不挡手势
    }

    // 原厂 cc_icon_t：48x48 canvas 手绘白线稿图标种类
    enum CcIconKind {
        CC_ICON_PWR, CC_ICON_BRI, CC_ICON_VOL, CC_ICON_DATA, CC_ICON_BAT,
        CC_ICON_INFO, CC_ICON_SET, CC_ICON_SLEEP,
    };

    // 在 48x48 canvas 上按原厂 sdgoods_cc.c cc_icon_draw 的几何 1:1 绘白线稿。
    // LVGL8 -> LVGL9 API 换算：无 lv_canvas_draw_*，改用 lv_canvas_init_layer/
    // finish_layer 夹住 lv_draw_line/rect/arc/triangle；lv_trigo_sin/cos 仍返回
    // ±32767，必须 >> LV_TRIGO_SHIFT(15) 归一化（否则半径放大 32 倍飞出画布）。
    static void CcIconDraw(lv_obj_t* canvas, CcIconKind kind) {
        lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_TRANSP);
        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);

        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = lv_color_white();
        line.width = 4;
        line.opa = LV_OPA_COVER;

        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.color = lv_color_white();
        arc.width = 4;
        arc.opa = LV_OPA_COVER;

        lv_draw_rect_dsc_t fill;
        lv_draw_rect_dsc_init(&fill);
        fill.bg_color = lv_color_white();
        fill.bg_opa = LV_OPA_COVER;
        fill.radius = 5;
        fill.border_width = 0;

        // 封装小工具：原厂 lv_canvas_draw_rect/line 在 LVGL9 下的等价调用
        auto rect = [&](int32_t x, int32_t y, int32_t w, int32_t h, const lv_draw_rect_dsc_t* d) {
            lv_area_t a{ x, y, x + w - 1, y + h - 1 };
            lv_draw_rect(&layer, d, &a);
        };
        auto seg = [&](int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
            lv_draw_line_dsc_t l = line;
            l.p1.x = x1; l.p1.y = y1; l.p2.x = x2; l.p2.y = y2;
            lv_draw_line(&layer, &l);
        };
        auto dot = [&](int32_t cx, int32_t cy, int32_t r) {
            lv_draw_rect_dsc_t d = fill;
            d.radius = LV_RADIUS_CIRCLE;
            rect(cx - r, cy - r, r * 2, r * 2, &d);
        };

        switch (kind) {
        case CC_ICON_PWR:
            arc.center.x = 24; arc.center.y = 24; arc.radius = 15;
            arc.start_angle = 0; arc.end_angle = 360;
            lv_draw_arc(&layer, &arc);
            seg(24, 5, 24, 17);
            break;
        case CC_ICON_BRI: {
            // 中心实心圆 + 8 向胶囊光芒（线段两端叠 6x6 圆点拼圆头；canvas 下 round 端画不出）
            lv_draw_rect_dsc_t c = fill; c.radius = LV_RADIUS_CIRCLE;
            rect(18, 18, 12, 12, &c);
            for (int i = 0; i < 8; i++) {
                int32_t a = i * 45;
                int32_t s = lv_trigo_sin(a), cc = lv_trigo_cos(a);
                int32_t x0 = 24 + ((11 * cc) >> LV_TRIGO_SHIFT);
                int32_t y0 = 24 + ((11 * s) >> LV_TRIGO_SHIFT);
                int32_t x1 = 24 + ((16 * cc) >> LV_TRIGO_SHIFT);
                int32_t y1 = 24 + ((16 * s) >> LV_TRIGO_SHIFT);
                seg(x0, y0, x1, y1);
                dot(x0, y0, 3);
                dot(x1, y1, 3);
            }
            break;
        }
        case CC_ICON_VOL: {
            lv_draw_rect_dsc_t spk = fill; spk.radius = 2;
            rect(8, 17, 9, 14, &spk);                       // 箱体
            lv_draw_triangle_dsc_t tri;
            lv_draw_triangle_dsc_init(&tri);
            tri.color = lv_color_white(); tri.opa = LV_OPA_COVER;
            tri.p[0].x = 17; tri.p[0].y = 18; tri.p[1].x = 17; tri.p[1].y = 30; tri.p[2].x = 27; tri.p[2].y = 36;
            lv_draw_triangle(&layer, &tri);                 // 梯形=两三角形拼接
            tri.p[0].x = 17; tri.p[0].y = 18; tri.p[1].x = 27; tri.p[1].y = 36; tri.p[2].x = 27; tri.p[2].y = 12;
            lv_draw_triangle(&layer, &tri);
            arc.center.x = 27; arc.center.y = 24; arc.radius = 13; arc.start_angle = 320; arc.end_angle = 40;
            lv_draw_arc(&layer, &arc);                      // 两道声波弧
            arc.radius = 19; arc.start_angle = 315; arc.end_angle = 45;
            lv_draw_arc(&layer, &arc);
            break;
        }
        case CC_ICON_DATA: {
            lv_draw_rect_dsc_t bar = fill; bar.radius = 2;
            rect(11, 28, 8, 10, &bar);
            rect(21, 20, 8, 18, &bar);
            rect(31, 12, 8, 26, &bar);
            break;
        }
        case CC_ICON_BAT:
            rect(6, 14, 30, 3, &fill); rect(6, 31, 30, 3, &fill);   // 外框上/下
            rect(6, 14, 3, 20, &fill); rect(33, 14, 3, 20, &fill);  // 外框左/右
            rect(36, 20, 6, 8, &fill); rect(11, 19, 18, 9, &fill);  // 凸点 + 电量条
            break;
        case CC_ICON_INFO:
            rect(21, 9, 6, 6, &fill); rect(21, 20, 6, 16, &fill);   // 点 + 竖线
            break;
        case CC_ICON_SET: {
            lv_draw_arc_dsc_t ring = arc;
            ring.center.x = 24; ring.center.y = 24; ring.radius = 18;
            ring.start_angle = 0; ring.end_angle = 360;
            lv_draw_arc(&layer, &ring);                             // 外圈
            rect(20, 20, 8, 8, &fill);                              // 中心方孔
            for (int i = 0; i < 8; i++) {
                int32_t a = i * 45;
                int32_t s = lv_trigo_sin(a), cc = lv_trigo_cos(a);
                int32_t cx = 24 + ((21 * cc) >> LV_TRIGO_SHIFT);
                int32_t cy = 24 + ((21 * s) >> LV_TRIGO_SHIFT);
                rect(cx - 3, cy - 3, 6, 6, &fill);                  // 8 齿
            }
            break;
        }
        case CC_ICON_SLEEP: {
            // 熄屏（原厂无对应图标）：自绘月牙 —— 一段开口朝右的粗弧
            lv_draw_arc_dsc_t m = arc;
            m.center.x = 24; m.center.y = 24; m.radius = 13;
            m.start_angle = 300; m.end_angle = 120;
            lv_draw_arc(&layer, &m);
            break;
        }
        default: break;
        }
        lv_canvas_finish_layer(canvas, &layer);
    }

    // 圆形瓷砖（原厂 make_round_btn）：lv_obj 灰底圆 + canvas 手绘线稿（zoom 208）+ 下挂 caption
    void CreateCcTile(lv_obj_t* parent, int cx, int cy, CcIconKind icon_kind, const char* caption,
                      lv_event_cb_t tap_cb) {
        constexpr int d = 68;
        auto btn = lv_obj_create(parent);          // 原厂用 lv_obj 而非 lv_button
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, d, d);
        lv_obj_set_pos(btn, cx - d / 2, cy - d / 2);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A3C), kSelPressed());
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn, tap_cb, LV_EVENT_CLICKED, this);

        // 图标缓冲：PSRAM 48x48 ARGB8888，跟随 canvas 删除而释放（不吃宝贵的内部 RAM）
        auto* buf = static_cast<uint32_t*>(heap_caps_malloc(48 * 48 * sizeof(uint32_t), MALLOC_CAP_SPIRAM));
        if (buf == nullptr) {
            return;
        }
        auto cv = lv_canvas_create(btn);
        lv_canvas_set_buffer(cv, buf, 48, 48, LV_COLOR_FORMAT_ARGB8888);
        CcIconDraw(cv, icon_kind);
        lv_obj_center(cv);
        lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_transform_scale(cv, 208, 0);   // 原厂 zoom 208 ≈ 81% 缩小
        lv_obj_set_style_transform_pivot_x(cv, 24, 0);  // 以中心为缩放轴
        lv_obj_set_style_transform_pivot_y(cv, 24, 0);
        // 缓冲生命周期挂到 canvas DELETE：防图标缓冲泄漏
        lv_obj_add_event_cb(cv, [](lv_event_t* e) {
            auto* b = static_cast<uint32_t*>(lv_event_get_user_data(e));
            if (b != nullptr) {
                heap_caps_free(b);
            }
        }, LV_EVENT_DELETE, buf);

        auto cap = lv_label_create(parent);
        lv_label_set_text(cap, caption);
        lv_obj_set_style_text_font(cap, &font_cc_noto_14_4, 0);  // 原厂 caption 用 14 号
        lv_obj_set_style_text_color(cap, lv_color_hex(0xE0E0E5), 0);
        lv_obj_align_to(cap, btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
        lv_obj_clear_flag(cap, LV_OBJ_FLAG_CLICKABLE);
    }

    // 底部 home 指示小横条：纯装饰（原厂 36x5 灰胶囊，@180,334）
    void CreateCcBottomHint(lv_obj_t* page) {
        auto bar = lv_obj_create(page);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 36, 5);
        lv_obj_set_pos(bar, DISPLAY_WIDTH / 2 - 18, 334);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x9A9A9E), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    }

    // 整页下滑/上滑手势绑定（原厂：底部上滑关全部；边缘右滑返回上级）
    struct PageGestureCtx { SdgoodsDimensionScreen* self; bool down; };
    static void OnPageGesture(lv_event_t* e) {
        auto* ctx = static_cast<PageGestureCtx*>(lv_event_get_user_data(e));
        if (ctx == nullptr || ctx->self == nullptr) {
            return;
        }
        auto indev = lv_indev_get_act();
        if (indev == nullptr) {
            return;
        }
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        auto code = lv_event_get_code(e);
        if (code == LV_EVENT_PRESSED) {
            ctx->self->cc_gesture_start_y_ = pt.y;
        } else if (code == LV_EVENT_PRESSING) {
            if (ctx->down) {
                if (pt.y - ctx->self->cc_gesture_start_y_ >= 50) {
                    // 同样不在按下分发中删对象树，挪到下一拍
                    lv_async_call([](void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->CloseControlCenter(); }, ctx->self);
                }
            } else {
                if (ctx->self->cc_gesture_start_y_ - pt.y >= 50) {
                    lv_async_call([](void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->CloseControlCenter(); }, ctx->self);
                }
            }
        }
    }
    void BindCloseGesture(lv_obj_t* page, bool down) {
        auto* ctx = new PageGestureCtx{this, down};
        lv_obj_add_event_cb(page, OnPageGesture, LV_EVENT_ALL, ctx);
        lv_obj_add_event_cb(page, [](lv_event_t* e) { delete static_cast<PageGestureCtx*>(lv_event_get_user_data(e)); },
                            LV_EVENT_DELETE, ctx);
    }

    // 左边缘右滑返回（原厂 swipe_back 语义，绑在页自身上）
    struct EdgeBackCtx { SdgoodsDimensionScreen* self; std::function<void()> back; lv_coord_t start_x; };
    static void OnEdgeBack(lv_event_t* e) {
        auto* ctx = static_cast<EdgeBackCtx*>(lv_event_get_user_data(e));
        auto code = lv_event_get_code(e);
        if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) {
            return;
        }
        auto indev = lv_indev_get_act();
        if (indev == nullptr) {
            return;
        }
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        if (code == LV_EVENT_PRESSED) {
            ctx->start_x = pt.x;
        } else if (pt.x - ctx->start_x >= 50 && ctx->start_x <= 40) {
            auto back = ctx->back;
            lv_async_call([](void* arg) { (*static_cast<std::function<void()>*>(arg))(); }, new std::function<void()>(back));
        }
    }
    void BindEdgeBack(lv_obj_t* page, std::function<void()> back) {
        auto* ctx = new EdgeBackCtx{this, back, 0};
        lv_obj_add_event_cb(page, OnEdgeBack, LV_EVENT_ALL, ctx);
        lv_obj_add_event_cb(page, [](lv_event_t* e) { delete static_cast<EdgeBackCtx*>(lv_event_get_user_data(e)); },
                            LV_EVENT_DELETE, ctx);
    }

    void DeletePage(lv_obj_t*& page) {
        if (page != nullptr) {
            auto p = page;
            page = nullptr;
            lv_anim_delete(p, nullptr);  // 取消挂在它身上的高度动画，避免删除后回调访问野指针
            lv_obj_delete(p);   // 瞬间关闭，无淡出（理由同 CreateCcPage 注释）
        }
        if (display_ != nullptr && cc_page_ == nullptr && cc_set_page_ == nullptr &&
            cc_slider_page_ == nullptr && cc_data_page_ == nullptr && cc_battery_page_ == nullptr &&
            cc_about_page_ == nullptr) {
            // 应用仍在前台时保持形象暂停（应用模式下形象本就是隐藏的）。
            if (!app_host_.IsActive()) {
                display_->SetGrokPaused(false);
            }
            app_host_.PauseForOverlay(false);
        }
    }
    void DeleteAllCcPages() {
        DeletePage(cc_page_); DeletePage(cc_set_page_); DeletePage(cc_slider_page_);
        DeletePage(cc_data_page_); DeletePage(cc_battery_page_); DeletePage(cc_about_page_);
    }

    // 瓷砖点击分发（LVGL 事件回调里不直接改对象树，统一 async 下一拍执行，原厂同款教训）
    static void AsyncRun(lv_async_cb_t fn, void* arg) { lv_async_call(fn, arg); }
    static void AfterOpenSet(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenSetPage(); }
    static void AfterOpenAbout(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenAboutPage(); }
    static void AfterOpenVolume(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenSliderPage(true); }
    static void AfterOpenBrightness(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenSliderPage(false); }
    static void AfterOpenData(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenDataPage(); }
    static void AfterOpenBattery(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenBatteryPage(); }
    static void AfterPower(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->PowerOff(); }
    static void AfterWifiConfig(void* arg) {
        auto self = static_cast<SdgoodsDimensionScreen*>(arg);
        self->CloseControlCenter();
        self->EnterWifiConfigMode();
    }

    static void OnTileSettings(lv_event_t* e) { AsyncRun(AfterOpenSet, lv_event_get_user_data(e)); }
    static void OnTileAbout(lv_event_t* e) { AsyncRun(AfterOpenAbout, lv_event_get_user_data(e)); }
    static void OnTilePower(lv_event_t* e) { AsyncRun(AfterPower, lv_event_get_user_data(e)); }
    static void OnTileVolume(lv_event_t* e) { AsyncRun(AfterOpenVolume, lv_event_get_user_data(e)); }
    static void OnTileBrightness(lv_event_t* e) { AsyncRun(AfterOpenBrightness, lv_event_get_user_data(e)); }
    static void OnTileData(lv_event_t* e) { AsyncRun(AfterOpenData, lv_event_get_user_data(e)); }
    static void OnTileBattery(lv_event_t* e) { AsyncRun(AfterOpenBattery, lv_event_get_user_data(e)); }
    static void OnNetConfig(lv_event_t* e) { AsyncRun(AfterWifiConfig, lv_event_get_user_data(e)); }

    static void AfterScreenOff(void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->ScreenOff(); }
    static void OnTileScreenOff(lv_event_t* e) { AsyncRun(AfterScreenOff, lv_event_get_user_data(e)); }

    static void OnGestureStrip(lv_event_t* e) {
        auto self = static_cast<SdgoodsDimensionScreen*>(lv_event_get_user_data(e));
        auto code = lv_event_get_code(e);
        lv_indev_t* indev = lv_indev_get_act();
        if (indev == nullptr) {
            return;
        }
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        if (code == LV_EVENT_PRESSED) {
            self->cc_gesture_start_y_ = pt.y;
        } else if (code == LV_EVENT_PRESSING) {
            if (self->cc_page_ == nullptr && pt.y - self->cc_gesture_start_y_ >= 40) {
                // 点开整页也是改对象树，同样 async 下一拍
                lv_async_call([](void* arg) { static_cast<SdgoodsDimensionScreen*>(arg)->OpenControlCenter(); }, self);
            }
        }
    }

    void SetupControlCenter() {
        // 顶部下滑捕获条（平时只存这一个隐藏对象，点开才建整页，省常驻内存）
        auto strip = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(strip);
        lv_obj_set_size(strip, DISPLAY_WIDTH, 50);
        lv_obj_align(strip, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(strip, OnGestureStrip, LV_EVENT_ALL, this);
    }

    // 一级页：原厂三键「Settings / About / Power」同款布局与坐标（cx=84/180/276，cy=169）
    // ===== 控制中心整页上下滑动（整页一起动，帧间无残影） =====
    // 只改整页 y：一帧一次整屏刷新，和面板扫描同节奏；不再做逐行高度展开。
    static void CcSlideYExec(void* obj, int32_t v) {
        lv_obj_set_y(static_cast<lv_obj_t*>(obj), v);
    }
    static void CcCollapseDone(void* arg) {
        static_cast<SdgoodsDimensionScreen*>(arg)->DeleteAllCcPages();
    }
    static void CcCollapseReady(lv_anim_t* a) {
        // 动画回调里不动对象树，下一拍再删
        lv_async_call(CcCollapseDone, static_cast<SdgoodsDimensionScreen*>(lv_anim_get_user_data(a)));
    }
    static void CcSlideYAnimate(lv_obj_t* page, int32_t from, int32_t to, uint32_t ms) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, page);
        lv_anim_set_values(&a, from, to);
        lv_anim_set_duration(&a, ms);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, CcSlideYExec);
        lv_anim_start(&a);
    }
    // 收起：整页抬到 -360，动画结束后下一拍删页
    void CcCollapseAnimate(lv_obj_t* page) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, page);
        lv_anim_set_values(&a, lv_obj_get_y(page), -DISPLAY_HEIGHT);
        lv_anim_set_duration(&a, 100);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, CcSlideYExec);
        lv_anim_set_user_data(&a, this);
        lv_anim_set_completed_cb(&a, CcCollapseReady);
        lv_anim_start(&a);
    }

    void OpenControlCenter() {
        if (cc_page_ != nullptr) {
            return;
        }
        auto page = CreateCcPage();
        cc_page_ = page;
        CreateCcTitle(page, "控制中心");
        CreateCcTile(page, 84, 169, CC_ICON_SET, "设置", OnTileSettings);
        CreateCcTile(page, 180, 169, CC_ICON_INFO, "关于", OnTileAbout);
        CreateCcTile(page, 276, 169, CC_ICON_PWR, "关机", OnTilePower);
        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);  // 整页上滑关闭（下拉打开后，上滑收起，符合直觉且与子页一致）
        // 下拉展开：整页从屏幕上方滑到 0，100ms 减速曲线，像手机下拉控制中心
        lv_obj_set_y(page, -DISPLAY_HEIGHT);
        CcSlideYAnimate(page, -DISPLAY_HEIGHT, 0, 100);
    }

    // 二级设置页：原厂上排 Volume/Brightness/Data（cy=130）+ 下排 Battery（84,230），
    // 熄屏键 Sleep（180,230）为本板扩展（原厂无）
    void OpenSetPage() {
        DeletePage(cc_set_page_);
        auto page = CreateCcPage();
        cc_set_page_ = page;
        CreateCcTitle(page, "设置");
        CreateCcTile(page, 84, 130, CC_ICON_VOL, "音量", OnTileVolume);
        CreateCcTile(page, 180, 130, CC_ICON_BRI, "亮度", OnTileBrightness);
        CreateCcTile(page, 276, 130, CC_ICON_DATA, "网络", OnTileData);
        CreateCcTile(page, 84, 230, CC_ICON_BAT, "电池", OnTileBattery);
        CreateCcTile(page, 180, 230, CC_ICON_SLEEP, "熄屏", OnTileScreenOff);
        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);  // 底部上滑直接关全部
        BindEdgeBack(page, [this]() { DeletePage(cc_set_page_); });  // 左边缘右滑回一级
    }

    // 滑块页（原厂 open_slider 同款）：标题 y=40、数值 y=72、240x24 大滑块居中
    void OpenSliderPage(bool volume) {
        DeletePage(cc_slider_page_);
        cc_slider_is_volume_ = volume;
        int init = volume ? GetAudioCodec()->output_volume() : GetBacklight()->brightness();
        auto page = CreateCcPage();
        cc_slider_page_ = page;
        CreateCcTitle(page, volume ? "音量" : "亮度");

        cc_slider_value_label_ = lv_label_create(page);
        lv_label_set_text_fmt(cc_slider_value_label_, "%d%%", init);
        lv_obj_set_style_text_font(cc_slider_value_label_, &font_cc_noto_16_4, 0);
        lv_obj_set_style_text_color(cc_slider_value_label_, lv_color_white(), 0);
        lv_obj_align(cc_slider_value_label_, LV_ALIGN_TOP_MID, 0, 72);
        lv_obj_clear_flag(cc_slider_value_label_, LV_OBJ_FLAG_CLICKABLE);

        auto slider = lv_slider_create(page);
        lv_obj_set_size(slider, 240, 24);
        lv_obj_align(slider, LV_ALIGN_CENTER, 0, 0);
        lv_slider_set_range(slider, volume ? 0 : 1, 100);  // 亮度下限 1，防存 0 黑屏（原厂 CC_BRI_MIN）
        lv_slider_set_value(slider, init, LV_ANIM_OFF);
        // 原厂不对滑块做任何自定义样式（直接用默认主题）；仅绑事件
        lv_obj_add_event_cb(slider, OnCcSliderChanged, LV_EVENT_VALUE_CHANGED, this);

        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);
        BindEdgeBack(page, [this]() { DeletePage(cc_slider_page_); });
    }

    static void OnCcSliderChanged(lv_event_t* e) {
        auto self = static_cast<SdgoodsDimensionScreen*>(lv_event_get_user_data(e));
        int v = lv_slider_get_value(static_cast<lv_obj_t*>(lv_event_get_target(e)));
        if (self->cc_slider_value_label_ != nullptr) {
            lv_label_set_text_fmt(self->cc_slider_value_label_, "%d%%", v);
        }
        if (self->cc_slider_is_volume_) {
            self->GetAudioCodec()->SetOutputVolume(v);
            ESP_LOGI(TAG, "Volume set to %d", v);
        } else {
            self->GetBacklight()->SetBrightness(static_cast<uint8_t>(v), true);  // permanent：存 settings，开机恢复
        }
    }

    // 页内信息行：垂直居中堆叠（数据/电量/关于页共用，白主灰辅两档）
    lv_obj_t* CreateCcInfoRow(lv_obj_t* page, const char* text, uint32_t color, lv_coord_t y_ofs) {
        auto lbl = lv_label_create(page);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, &font_cc_noto_16_4, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, y_ofs);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return lbl;
    }

    void EnsureBatteryMonitor() {
        if (battery_monitor_ == nullptr) {
            battery_monitor_ = new AdcBatteryMonitor(ADC_UNIT_1, BAT_ADC_CHANNEL,
                                                     BAT_ADC_UPPER_RESISTOR, BAT_ADC_LOWER_RESISTOR);
        }
    }

    std::string BuildWifiLine() {
        std::string ssid = "--";
        std::string rssi_str = "--";
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ssid = reinterpret_cast<const char*>(ap.ssid);
            char rbuf[24];
            snprintf(rbuf, sizeof(rbuf), "%d dBm", ap.rssi);
            rssi_str = rbuf;
        }
        std::string ip_str = "--";
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ipbuf[16];
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ip_info.ip));
            ip_str = ipbuf;
        }
        return "WiFi " + ssid + " · " + rssi_str + "\nIP " + ip_str;
    }

    // 网络页（原厂 Data 页行式，文案中文）：内存/缓存实测 + WiFi 状态 + 重新配网按钮
    void OpenDataPage() {
        DeletePage(cc_data_page_);
        auto page = CreateCcPage();
        cc_data_page_ = page;
        CreateCcTitle(page, "网络");
        // RAM/PSRAM 都由 heap_caps 实测，不写死标称值（对齐原厂「data 里的数据要真实」）
        uint32_t ram_free = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
        uint32_t ram_total = (uint32_t)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024);
        uint32_t psram_mb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024 * 1024));
        char l1[48], l2[48], l3[48];
        snprintf(l1, sizeof(l1), "内存 %lu / %lu KB", (unsigned long)ram_free, (unsigned long)ram_total);
        snprintf(l2, sizeof(l2), "缓存 %lu MB", (unsigned long)psram_mb);
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(l3, sizeof(l3), "WiFi %.20s %d dBm", (const char*)ap.ssid, ap.rssi);
        } else {
            snprintf(l3, sizeof(l3), "WiFi --");
        }
        const char* lines[3] = { l1, l2, l3 };
        for (int i = 0; i < 3; i++) {
            CreateCcInfoRow(page, lines[i], 0xFFFFFF, -48 + i * 40);   // 行距 40px，整块居中（原厂同口径）
        }

        auto btn = lv_button_create(page);
        lv_obj_align(btn, LV_ALIGN_CENTER, 0, 60);
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2C2C2E), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A3C), kSelPressed());
        lv_obj_add_event_cb(btn, OnNetConfig, LV_EVENT_CLICKED, this);
        auto lbl2 = lv_label_create(btn);
        lv_label_set_text(lbl2, "重新配网");
        lv_obj_set_style_text_font(lbl2, &font_cc_noto_16_4, 0);
        lv_obj_set_style_text_color(lbl2, lv_color_white(), 0);
        lv_obj_center(lbl2);

        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);
        BindEdgeBack(page, [this]() { DeletePage(cc_data_page_); });
    }

    // 电量页（原厂 Battery 页行式，文案中文）："电量 n%%" + 充电状态（无 GetVoltage 接口，不显电压行）
    void OpenBatteryPage() {
        DeletePage(cc_battery_page_);
        EnsureBatteryMonitor();
        auto page = CreateCcPage();
        cc_battery_page_ = page;
        CreateCcTitle(page, "电池");
        unsigned level = battery_monitor_->GetBatteryLevel();
        char l1[32], l2[32];
        snprintf(l1, sizeof(l1), "电量 %u%%", level);
        snprintf(l2, sizeof(l2), "%s", battery_monitor_->IsCharging() ? "充电中" : "电池供电");
        const char* lines[2] = { l1, l2 };
        for (int i = 0; i < 2; i++) {
            CreateCcInfoRow(page, lines[i], 0xFFFFFF, -28 + i * 40);   // 与数据页行距口径一致
        }
        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);
        BindEdgeBack(page, [this]() { DeletePage(cc_battery_page_); });
    }

    // 关于页（原厂 About 页行式）：name / v%s / 编译日期 时间，行距 34px
    void OpenAboutPage() {
        DeletePage(cc_about_page_);
        auto page = CreateCcPage();
        cc_about_page_ = page;
        CreateCcTitle(page, "关于");
        const esp_app_desc_t* d = esp_app_get_description();
        char l2[64], l3[64];
        const char* l1 = "sdgoods-xiaozhi";   // 固定展示移植标识，不用上游 project_name "xiaozhi"
        snprintf(l2, sizeof(l2), "v%s", d->version);
        snprintf(l3, sizeof(l3), "%s %s", d->date, d->time);
        const char* lines[3] = { l1, l2, l3 };
        for (int i = 0; i < 3; i++) {
            CreateCcInfoRow(page, lines[i], 0xFFFFFF, -64 + i * 34);   // 整块视觉居中（原厂同款）
        }
        CreateCcBottomHint(page);
        BindCloseGesture(page, /*down=*/false);
        BindEdgeBack(page, [this]() { DeletePage(cc_about_page_); });
    }

    void CloseControlCenter() {
        if (cc_page_ == nullptr) {
            return;
        }
        // 有二级页时直接整栈收掉（二级页不做动画，收得更干净）
        if (cc_set_page_ != nullptr || cc_slider_page_ != nullptr || cc_data_page_ != nullptr ||
            cc_battery_page_ != nullptr || cc_about_page_ != nullptr) {
            DeleteAllCcPages();
            return;
        }
        // 先取消可能还在跑的展开动画，再从当前 y 整体抬起
        lv_anim_delete(cc_page_, CcSlideYExec);
        CcCollapseAnimate(cc_page_);
    }

    std::string BuildInfoText() {
        char buf[224];
        const char* version = esp_app_get_description()->version;

        std::string ip_str = "--";
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info;
        if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ipbuf[16];
            snprintf(ipbuf, sizeof(ipbuf), IPSTR, IP2STR(&ip_info.ip));
            ip_str = ipbuf;
        }

        std::string rssi_str = "--";
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            char rbuf[24];
            snprintf(rbuf, sizeof(rbuf), "%d dBm", ap.rssi);
            rssi_str = rbuf;
        }

        unsigned heap_kb = static_cast<unsigned>(SystemInfo::GetFreeHeapSize() / 1024);
        snprintf(buf, sizeof(buf), "固件 v%s\nIP %s · WiFi %s\nMAC %s · 内存 %uKB",
                 version, ip_str.c_str(), rssi_str.c_str(),
                 SystemInfo::GetMacAddress().c_str(), heap_kb);
        return buf;
    }

public:
    SdgoodsDimensionScreen() :
        boot_button_(BOOT_BUTTON_GPIO, false, 1500) {
        InitializePowerLatch();
        InitializeDisplayPower();
        InitializeSpi();
        Initializest77916Display();
        InitializeI2c();
        InitializeTouch();
        InitializeButtons();
        InitializeTools();
        // 背光保持关闭直到 SetupUI 完成：见 SdgoodsRoundLcdDisplay::BootLightUp()。
        // 开机阶段面板里可能还有厂方残留画面，背光不开就什么也看不到。
    }

    virtual AudioCodec* GetAudioCodec() override {
        static SdgoodsAudioCodec audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_MIC_GPIO_BCLK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_SPK_GPIO_MCLK,
            AUDIO_PA_GPIO);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    // 陀螺仪角速度（度/秒，±64dps 量程）。IMU 不存在时返回 false。
    virtual bool ReadGyro(int& x, int& y, int& z) override {
        if (!imu_ready_ || imu_dev_ == nullptr) {
            return false;
        }
        uint8_t reg = 0x3B;   // GX_L
        uint8_t buf[6] = {0};
        if (i2c_master_transmit_receive(imu_dev_, &reg, 1, buf, sizeof(buf), 50) != ESP_OK) {
            return false;
        }
        const int16_t gx = static_cast<int16_t>((static_cast<uint16_t>(buf[1]) << 8) | buf[0]);
        const int16_t gy = static_cast<int16_t>((static_cast<uint16_t>(buf[3]) << 8) | buf[2]);
        const int16_t gz = static_cast<int16_t>((static_cast<uint16_t>(buf[5]) << 8) | buf[4]);
        constexpr float kScale = 64.0f / 32768.0f;
        x = static_cast<int>(gx * kScale);
        y = static_cast<int>(gy * kScale);
        z = static_cast<int>(gz * kScale);
        return true;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(SdgoodsDimensionScreen);
