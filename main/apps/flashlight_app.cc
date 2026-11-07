// 强光手电：纯白照明 / 红蓝警示灯 / 呼吸灯（摇一摇换色）。
//
// 三页按环形排列，默认停在“纯白”：
//   纯白 --右滑--> 警示灯 --右滑--> 呼吸灯 --右滑--> 纯白
//   纯白 --左滑--> 呼吸灯 --左滑--> 警示灯 --左滑--> 纯白
// 手电/警示灯/呼吸灯都保持背光 100%，退出应用时恢复进入前的亮度。
//
// 实现要点：全程只有一个固定周期的定时器（进入时创建、退出时删除），
// 三种效果都由“当前时间 - 进入该页的时间”推算，切换页面只改状态不增删定时器，
// 也不创建/删除对象，避免在事件回调里动 LVGL 的定时器链表。
#include "app.h"
#include "app_icons.h"

#include <lvgl.h>

#include <cmath>

#include "boards/common/backlight.h"
#include "boards/common/board.h"

extern const AppInfo kFlashlightInfo = {"flashlight", "强光手电", 0xFFF1D6, DrawFlashlightIcon};

namespace {

constexpr uint32_t kStrobeRed = 0xD02A1E;
constexpr uint32_t kStrobeBlue = 0x1028D0;
constexpr uint32_t kStrobeWhite = 0xFFFFFF;

// 警示灯按参考视频逐帧量出的序列，约 3 秒一个大循环：
//   ① 半蓝半红：右半蓝 300ms → 左半红 117 → 全暗 67 → 左半红 116，重复 3 次
//   ② 全蓝 → 全白 → 全蓝 → 全暗
//   ③ 全红 → 全白 → 全红 → 全暗
// ①中任意时刻只有一侧亮，另一侧为黑，与视频一致。
enum StrobeVisual {
    kVisOff,        // 两侧全黑
    kVisLeftRed,    // 左半红，右半黑
    kVisRightBlue,  // 右半蓝，左半黑
    kVisFullBlue,   // 全蓝
    kVisFullRed,    // 全红
    kVisFullWhite,  // 全白
};
struct StrobeSeg {
    uint16_t ms;
    StrobeVisual vis;
};
constexpr StrobeSeg kStrobeSegs[] = {
    {300, kVisRightBlue}, {117, kVisLeftRed}, {67, kVisOff}, {116, kVisLeftRed},
    {300, kVisRightBlue}, {117, kVisLeftRed}, {67, kVisOff}, {116, kVisLeftRed},
    {300, kVisRightBlue}, {117, kVisLeftRed}, {67, kVisOff}, {116, kVisLeftRed},
    {120, kVisFullBlue},  {120, kVisFullWhite}, {120, kVisFullBlue}, {240, kVisOff},
    {120, kVisFullRed},   {120, kVisFullWhite}, {120, kVisFullRed},  {240, kVisOff},
};
constexpr int kStrobeSegCount = static_cast<int>(sizeof(kStrobeSegs) / sizeof(kStrobeSegs[0]));
constexpr uint32_t kStrobeMacroMs = 3000;

// 呼吸灯可选颜色：0 号为白色，其余按需求顺序循环。
constexpr uint32_t kBreathColors[] = {
    0xFFFFFF,  // 白色（默认）
    0xD75F28,  // 215, 95, 40
    0x81D8CF,  // 129, 216, 207
    0xD12C25,  // 209, 44, 37
    0xFF6347,  // 255, 99, 71
    0xF7E14D,  // 247, 225, 77
    0x0D3869,  // 13, 56, 105
    0x5E0004,  // 94, 0, 4
    0x432915,  // 67, 41, 19
    0x39855E,  // 57, 133, 94
};
constexpr int kBreathColorCount = static_cast<int>(sizeof(kBreathColors) / sizeof(kBreathColors[0]));
constexpr uint32_t kBreathPeriodMs = 4000;   // 一个呼吸循环 4 秒
constexpr uint32_t kBreathFrameMs = 40;      // 呼吸灯重绘间隔（25fps）
constexpr uint32_t kTickMs = 10;             // 统一状态定时器周期

// 摇一摇判定：相邻两次采样的角速度跳变总和超过阈值才算一次，且 1 秒内只认一次。
constexpr int kShakeDelta = 150;
constexpr uint32_t kShakeCooldownMs = 1000;
// 陀螺仪采样节流：100ms 读一次。ReadGyro 走共享 I2C（与触摸同总线），若在
// LVGL 线程里高频阻塞读，会和触摸驱动争总线、拉长 input 事件处理，加重整机卡顿。
constexpr uint32_t kGyroReadMs = 100;

enum Mode {
    kModeTorch = 0,   // 纯白
    kModeStrobe = 1,  // 红蓝警示灯
    kModeBreath = 2,  // 呼吸灯
};

class FlashlightApp : public App {
public:
    const AppInfo& info() const override { return kFlashlightInfo; }

    void OnEnter(AppContext& ctx) override {
        root_ = ctx.root();
        width_ = ctx.width();
        height_ = ctx.height();

        Backlight* bl = Board::GetInstance().GetBacklight();
        if (bl != nullptr) {
            saved_brightness_ = bl->brightness();
            bl->SetBrightness(100);
        }

        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
        lv_obj_set_style_radius(root_, 0, 0);
        lv_obj_set_style_border_width(root_, 0, 0);
        lv_obj_set_style_pad_all(root_, 0, 0);
        lv_obj_set_style_shadow_width(root_, 0, 0);
        lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);

        // 只用一块右半屏色块：左半屏直接用根对象的背景色，整屏只有两层要画。
        right_ = lv_obj_create(root_);
        lv_obj_remove_style_all(right_);
        lv_obj_set_pos(right_, width_ / 2, 0);
        lv_obj_set_size(right_, width_ - width_ / 2, height_);
        lv_obj_set_style_bg_opa(right_, LV_OPA_COVER, 0);
        lv_obj_remove_flag(right_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(right_, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_add_event_cb(root_, OnPressed, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(root_, OnReleased, LV_EVENT_RELEASED, this);
        lv_obj_add_event_cb(root_, OnPressLost, LV_EVENT_PRESS_LOST, this);

        EnterMode(kModeTorch);
        timer_ = lv_timer_create(OnTick, kTickMs, this);
    }

    void OnPause() override {
        if (timer_ != nullptr) {
            lv_timer_pause(timer_);
        }
    }

    void OnResume() override {
        if (timer_ != nullptr) {
            lv_timer_resume(timer_);
        }
    }

    void OnExit() override {
        if (timer_ != nullptr) {
            lv_timer_delete(timer_);
            timer_ = nullptr;
        }
        if (root_ != nullptr) {
            lv_obj_remove_event_cb(root_, OnPressed);
            lv_obj_remove_event_cb(root_, OnReleased);
            lv_obj_remove_event_cb(root_, OnPressLost);
            root_ = nullptr;
        }
        right_ = nullptr;

        Backlight* bl = Board::GetInstance().GetBacklight();
        if (bl != nullptr) {
            if (saved_brightness_ == 0) {
                bl->RestoreBrightness();
            } else {
                bl->SetBrightness(saved_brightness_, false);
            }
        }
    }

private:
    static int Abs(int v) { return v < 0 ? -v : v; }

    static void OnPressed(lv_event_t* e) {
        auto* self = static_cast<FlashlightApp*>(lv_event_get_user_data(e));
        lv_indev_t* indev = lv_event_get_indev(e);
        if (indev != nullptr) {
            lv_indev_get_point(indev, &self->press_pt_);
            self->press_valid_ = true;
        }
    }

    static void OnPressLost(lv_event_t* e) {
        auto* self = static_cast<FlashlightApp*>(lv_event_get_user_data(e));
        self->press_valid_ = false;
    }

    static void OnReleased(lv_event_t* e) {
        auto* self = static_cast<FlashlightApp*>(lv_event_get_user_data(e));
        if (!self->press_valid_) {
            return;
        }
        self->press_valid_ = false;

        lv_indev_t* indev = lv_event_get_indev(e);
        if (indev == nullptr) {
            return;
        }
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        const int dx = static_cast<int>(p.x) - static_cast<int>(self->press_pt_.x);
        const int dy = static_cast<int>(p.y) - static_cast<int>(self->press_pt_.y);
        const int adx = Abs(dx);
        const int ady = Abs(dy);
        if (adx < 50 || adx <= ady) {
            return;
        }
        // 不在这里直接 EnterMode：LV_EVENT_RELEASED 处在 input 事件分发栈上，
        // 立刻整屏重绘（警示灯/呼吸灯都全屏变色）+ 读陀螺仪（共享 I2C）会把
        // 大量工作压在事件回调里，容易触发栈溢出 / 争夺总线而死机。
        // 改为只记录目标页，交给下一拍定时器 OnTick 执行，与进入应用时的路径一致。
        if (dx > 0) {
            self->pending_mode_ = (self->mode_ + 1) % 3;   // 右滑：下一页
        } else {
            self->pending_mode_ = (self->mode_ + 2) % 3;   // 左滑：上一页
        }
    }

    void EnterMode(int mode) {
        mode_ = mode;
        mode_start_ms_ = lv_tick_get();
        last_breath_paint_ms_ = 0;

        if (mode == kModeTorch) {
            SetScreenColors(kStrobeWhite, kStrobeWhite);
        } else if (mode == kModeStrobe) {
            strobe_seg_ = -1;
            TickStrobe();
        } else {
            // 每次进入呼吸灯都从白色重新开始。
            breath_index_ = 0;
            last_gyro_valid_ = false;
            last_shake_ms_ = 0;
            TickBreath();
        }
    }

    static void OnTick(lv_timer_t* t) {
        auto* self = static_cast<FlashlightApp*>(lv_timer_get_user_data(t));
        if (self->pending_mode_ >= 0) {
            const int next = self->pending_mode_;
            self->pending_mode_ = -1;
            self->EnterMode(next);
            return;
        }
        if (self->mode_ == kModeStrobe) {
            self->TickStrobe();
        } else if (self->mode_ == kModeBreath) {
            self->TickBreath();
        }
    }

    void SetScreenColors(uint32_t left, uint32_t right) {
        if (root_ == nullptr || right_ == nullptr) {
            return;
        }
        if (colors_valid_ && left == last_left_ && right == last_right_) {
            return;
        }
        colors_valid_ = true;
        last_left_ = left;
        last_right_ = right;
        lv_obj_set_style_bg_color(root_, lv_color_hex(left), 0);
        lv_obj_set_style_bg_color(right_, lv_color_hex(right), 0);
    }

    // 按“进入警示灯页后的时间”在 3 秒循环表里定位当前段，只在段变化时重绘。
    void TickStrobe() {
        const uint32_t elapsed = lv_tick_elaps(mode_start_ms_) % kStrobeMacroMs;
        uint32_t acc = 0;
        int idx = 0;
        for (; idx < kStrobeSegCount; ++idx) {
            acc += kStrobeSegs[idx].ms;
            if (elapsed < acc) {
                break;
            }
        }
        if (idx >= kStrobeSegCount) {
            idx = kStrobeSegCount - 1;
        }
        if (idx == strobe_seg_) {
            return;
        }
        strobe_seg_ = idx;

        uint32_t left = 0x000000;
        uint32_t right = 0x000000;
        switch (kStrobeSegs[idx].vis) {
            case kVisLeftRed:
                left = kStrobeRed;
                break;
            case kVisRightBlue:
                right = kStrobeBlue;
                break;
            case kVisFullBlue:
                left = kStrobeBlue;
                right = kStrobeBlue;
                break;
            case kVisFullRed:
                left = kStrobeRed;
                right = kStrobeRed;
                break;
            case kVisFullWhite:
                left = kStrobeWhite;
                right = kStrobeWhite;
                break;
            case kVisOff:
            default:
                break;
        }
        SetScreenColors(left, right);
    }

    void TickBreath() {
        const uint32_t now = lv_tick_get();
        if (last_breath_paint_ms_ != 0 && now - last_breath_paint_ms_ < kBreathFrameMs) {
            return;
        }
        last_breath_paint_ms_ = now;

        PollShake();

        // 4 秒一个呼吸周期，亮度在 8%~100% 之间平滑起伏。
        constexpr float kTwoPi = 6.2831853f;
        const float phase =
            kTwoPi * static_cast<float>(lv_tick_elaps(mode_start_ms_) % kBreathPeriodMs) / kBreathPeriodMs;
        const float factor = 0.08f + 0.92f * (0.5f - 0.5f * std::cos(phase));

        const lv_color_t base = lv_color_hex(kBreathColors[breath_index_]);
        const auto scale = [factor](uint8_t v) {
            int out = static_cast<int>(v * factor + 0.5f);
            return static_cast<uint8_t>(out > 255 ? 255 : out);
        };
        const uint32_t r = scale(base.red);
        const uint32_t g = scale(base.green);
        const uint32_t b = scale(base.blue);
        SetScreenColors((r << 16) | (g << 8) | b, (r << 16) | (g << 8) | b);
    }

    // 陀螺仪检测“摇一摇”：比较相邻采样，动作越猛跳变越大。
    // 采样被节流到 100ms 一次，避免高频争抢共享 I2C 总线。
    void PollShake() {
        const uint32_t now = lv_tick_get();
        if (last_gyro_read_ms_ != 0 && now - last_gyro_read_ms_ < kGyroReadMs) {
            return;
        }
        last_gyro_read_ms_ = now;

        int gx = 0, gy = 0, gz = 0;
        if (!Board::GetInstance().ReadGyro(gx, gy, gz)) {
            last_gyro_valid_ = false;
            return;
        }
        if (!last_gyro_valid_) {
            last_gyro_[0] = gx;
            last_gyro_[1] = gy;
            last_gyro_[2] = gz;
            last_gyro_valid_ = true;
            return;
        }
        const int delta =
            Abs(gx - last_gyro_[0]) + Abs(gy - last_gyro_[1]) + Abs(gz - last_gyro_[2]);
        last_gyro_[0] = gx;
        last_gyro_[1] = gy;
        last_gyro_[2] = gz;
        if (delta < kShakeDelta) {
            return;
        }
        if (last_shake_ms_ != 0 && now - last_shake_ms_ < kShakeCooldownMs) {
            return;
        }
        last_shake_ms_ = now;
        breath_index_ = (breath_index_ + 1) % kBreathColorCount;
    }

    lv_obj_t* root_ = nullptr;
    lv_obj_t* right_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int mode_ = kModeTorch;
    int pending_mode_ = -1;
    uint8_t saved_brightness_ = 0;

    uint32_t mode_start_ms_ = 0;
    int strobe_seg_ = -1;

    uint32_t last_left_ = 0;
    uint32_t last_right_ = 0;
    bool colors_valid_ = false;

    int breath_index_ = 0;
    uint32_t last_breath_paint_ms_ = 0;
    int last_gyro_[3] = {0, 0, 0};
    bool last_gyro_valid_ = false;
    uint32_t last_shake_ms_ = 0;
    uint32_t last_gyro_read_ms_ = 0;

    lv_point_t press_pt_ = {0, 0};
    bool press_valid_ = false;
};

}  // namespace

App* CreateFlashlightApp() {
    return new FlashlightApp();
}
