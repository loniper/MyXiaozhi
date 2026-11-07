// Procedural full-screen GrokBot renderer for the 360x360 SDGOODS round screen.
// Everything is drawn in LVGL's draw layer each frame; there are no image assets.
#pragma once

#include "lvgl.h"

class GrokbotAvatar {
public:
    enum class Mode {
        Standby,
        Listening,
        Speaking,
        Provisioning,
        Other,
    };

    GrokbotAvatar() = default;
    ~GrokbotAvatar();

    void Create(lv_obj_t* parent, int screen_w, int screen_h);
    // Starts the one-shot boot entrance (lids open once). Called by the board
    // after the panel has been cleared and the backlight is about to come on,
    // so the opening is never hidden behind the backlight ramp.
    void StartIntro();
    void SetMode(Mode mode);
    void SetEmotion(const char* emotion);
    void SetPaused(bool paused);
    lv_obj_t* obj() const { return obj_; }

private:
    // Seam-free primitives only: capsules are painted as a chain of fully
    // opaque discs (equivalent to a rotated stadium), arcs are single
    // round-capped arcs, hearts are opaque circles + triangle. No primitive is
    // ever drawn at partial opacity, so no seam can appear between them.
    enum class EyeKind : uint8_t {
        Bean,
        Beam,
        Dot,
        ArcUp,
        ArcDown,
        Heart,
    };

    // Design canvas is 360x360 with the eye centre at (180,180). Every eye owns
    // its own size, offset and tilt; the expressions rely on that asymmetry.
    struct FaceState {
        EyeKind eye_l;
        EyeKind eye_r;
        float w_l, h_l, dx_l, dy_l, rot_l;
        float w_r, h_r, dx_r, dy_r, rot_r;
        float bounce, bounce_period;
        bool tears, blush, sweat, bridge;
    };

    static void DrawEvent(lv_event_t* e);
    static void TimerEvent(lv_timer_t* t);

    void Draw(lv_layer_t* layer);
    void Update(float dt);
    float BlinkAmount() const;
    void ApplyState(const char* key);
    void ApplyModeState();
    void PickStandbyState();
    void DrawEye(lv_layer_t* layer, float cx, float cy, bool left, EyeKind kind, float factor,
                 float blink);
    void DrawBody(lv_layer_t* layer, float cx, float cy);
    void DrawTears(lv_layer_t* layer, float cx, float cy);
    void DrawBlush(lv_layer_t* layer, float cx, float cy);
    void DrawSweat(lv_layer_t* layer, float cx, float cy);
    void DrawBridge(lv_layer_t* layer, float cx, float cy);

    static const FaceState* FindState(const char* key);
    static const char* MapEmotion(const char* emotion);

    lv_obj_t* obj_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    int width_ = 360;
    int height_ = 360;
    float u_ = 1.0f;  // design-to-screen scale (design canvas is 360x360)

    Mode mode_ = Mode::Other;
    char active_key_[24] = {0};
    FaceState target_{};
    const FaceState* target_ptr_ = nullptr;
    float emotion_hold_ = 0.0f;
    const char* last_standby_ = nullptr;
    bool paused_ = false;

    // Eye kinds with a short squish morph so expressions change shape without a cut.
    EyeKind eye_l_ = EyeKind::Bean;
    EyeKind eye_r_ = EyeKind::Bean;
    EyeKind prev_eye_l_ = EyeKind::Bean;
    EyeKind prev_eye_r_ = EyeKind::Bean;
    float blend_l_ = 1.0f;
    float blend_r_ = 1.0f;

    // Numerically animated parameters (springs towards the state targets).
    float eye_w_l_ = 78, eye_h_l_ = 120, eye_dx_l_ = -66, eye_dy_l_ = 0, eye_rot_l_ = 0;
    float eye_w_r_ = 78, eye_h_r_ = 120, eye_dx_r_ = 66, eye_dy_r_ = 0, eye_rot_r_ = 0;
    float bounce_ = 5.0f, bounce_period_ = 3.2f;
    float tears_a_ = 0.0f, blush_a_ = 0.0f, sweat_a_ = 0.0f, bridge_a_ = 0.0f;

    float time_ = 0.0f;
    float bounce_phase_ = 0.0f;
    float breath_phase_ = 0.0f;

    // Previous frame's invalidated rect per eye: shapes shrink/move between
    // frames, so the fresh rect alone leaves stale pixels behind (ghosts).
    lv_area_t prev_area_l_ = {};
    lv_area_t prev_area_r_ = {};
    bool has_prev_area_l_ = false;
    bool has_prev_area_r_ = false;

    // Number of full-panel repaints done right after boot (used to wipe any
    // leftover pixels from the boot stage without paying a full flush forever).
    uint8_t boot_full_frames_ = 0;

    // Seconds since StartIntro(). Negative means "not started yet": the lids
    // stay shut while the backlight is still off during boot.
    float intro_t_ = -1.0f;

    uint32_t last_ms_ = 0;
    uint32_t next_swap_ms_ = 0;
    uint32_t next_blink_ms_ = 0;
    float blink_t_ = -1.0f;
};
