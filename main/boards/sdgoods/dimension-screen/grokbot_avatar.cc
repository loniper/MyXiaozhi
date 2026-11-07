// Procedural full-screen GrokBot renderer.
// The bot is a near-black screen with two white eyes. The fill covers the whole
// round panel (screen = body), so there is no stage ring and nothing decorative
// near the top edge. Every shape is a solid primitive: capsules are a chain of
// opaque discs, arcs are single round-capped arcs, hearts are opaque circles and
// a triangle. Because nothing is drawn at partial opacity, no seam can appear
// between overlapping parts, and expressions morph through smoothed parameter
// springs plus a short squish-through-shape morph instead of a hard cut.
#include "grokbot_avatar.h"

#include <esp_heap_caps.h>
#include <esp_random.h>
#include <lvgl.h>

#include <cmath>
#include <cstdio>
#include <cstring>

static constexpr float kPi = 3.14159265358979f;
static constexpr float kDeg = kPi / 180.0f;

// Pure black stage: the round panel is the face itself, nothing else shows.
static constexpr uint32_t kBodyColor = 0x000000;

static inline float Clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Snap to a step so sub-pixel spring motion does not invalidate cached masks.
static inline float Q(float v, float step) {
    return step > 0.0f ? roundf(v / step) * step : v;
}

// Exponential approach: frame-rate independent, always smooth, no jitter.
static inline float Smooth(float cur, float target, float rate, float dt) {
    return cur + (target - cur) * (1.0f - expf(-rate * dt));
}

static inline lv_opa_t MixOpa(float mix, float base) {
    float v = base * Clampf(mix, 0.0f, 1.0f);
    return static_cast<lv_opa_t>(Clampf(v, 0.0f, 255.0f));
}

// -------- draw primitives (integrates into the current LVGL draw layer) --------

// Solid rounded rectangle. One primitive: no fan seams, no interior gaps.
static void AddRRect(lv_layer_t* layer, float cx, float cy, float w, float h, int32_t radius,
                     lv_color_t color, lv_opa_t opa) {
    if (w < 1.0f || h < 1.0f || opa == 0) {
        return;
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = color;
    d.bg_opa = opa;
    d.radius = radius;
    int32_t wp = static_cast<int32_t>(lroundf(w));
    int32_t hp = static_cast<int32_t>(lroundf(h));
    lv_area_t a;
    a.x1 = static_cast<int32_t>(lroundf(cx)) - wp / 2;
    a.y1 = static_cast<int32_t>(lroundf(cy)) - hp / 2;
    a.x2 = a.x1 + wp - 1;
    a.y2 = a.y1 + hp - 1;
    lv_draw_rect(layer, &d, &a);
}

static void AddCircle(lv_layer_t* layer, float cx, float cy, float r, lv_color_t color,
                      lv_opa_t opa) {
    AddRRect(layer, cx, cy, r * 2.0f, r * 2.0f, LV_RADIUS_CIRCLE, color, opa);
}

// LVGL has no rotated rectangle, but a thick line with round caps IS a rotated
// stadium. The software renderer antialiases the line sides (line masks) and the
// caps (radius masks), so the outline stays smooth at any tilt - unlike a chain
// of discs, whose integer-rounded circles produce a wavy border.
static void AddCapsule(lv_layer_t* layer, float cx, float cy, float w, float h, float rot,
                       lv_color_t color, lv_opa_t opa) {
    float minor = w < h ? w : h;
    float major = w < h ? h : w;
    if (minor < 1.0f || opa == 0) {
        return;
    }
    if (major - minor < 1.5f) {
        // Nearly round: a single disc is smoother than a zero-length line.
        AddCircle(layer, cx, cy, minor * 0.5f, color, opa);
        return;
    }
    // Long axis direction; positive rotation is counter-clockwise on screen.
    float ax, ay;
    if (h >= w) {
        ax = -sinf(rot);
        ay = -cosf(rot);
    } else {
        ax = cosf(rot);
        ay = -sinf(rot);
    }
    float half = (major - minor) * 0.5f;

    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = color;
    d.opa = opa;
    d.width = static_cast<int32_t>(lroundf(minor));
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = static_cast<lv_value_precise_t>(cx - ax * half);
    d.p1.y = static_cast<lv_value_precise_t>(cy - ay * half);
    d.p2.x = static_cast<lv_value_precise_t>(cx + ax * half);
    d.p2.y = static_cast<lv_value_precise_t>(cy + ay * half);
    lv_draw_line(layer, &d);
}

// lv_draw_triangle fills through hard line masks, so its boundary is NOT
// antialiased and shows pixel stairs. Tracing the two long sides with a thick
// round-capped line (which IS antialiased) hides the polygon edge under a smooth
// band and rounds the bottom tip at the same time.
static void AddSmoothEdge(lv_layer_t* layer, float tx, float ty, float ax, float ay,
                          float cxh, float cyh, float stroke, lv_color_t color,
                          lv_opa_t opa) {
    float dx = ax - tx, dy = ay - ty;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < stroke + 2.0f || stroke < 1.5f) {
        return;
    }
    float ux = dx / len, uy = dy / len;
    float nx = -uy, ny = ux;
    // Normal pointing into the shape: towards the eye centre.
    if (nx * (cxh - tx) + ny * (cyh - ty) < 0.0f) {
        nx = -nx;
        ny = -ny;
    }
    float major = len - stroke;
    float mx = (tx + ax) * 0.5f + nx * stroke * 0.5f;
    float my = (ty + ay) * 0.5f + ny * stroke * 0.5f;
    float rot = atan2f(-uy, ux);
    AddCapsule(layer, mx, my, major, stroke, rot, color, opa);
}

// LVGL's sw arc mask clamps negative start angles to 0, which would silently
// shorten a tilted arc and leave its start cap detached. Normalising here keeps
// the drawn span exactly 180 degrees for every tilt.
static inline float NormAngle(float a) {
    a = fmodf(a, 360.0f);
    if (a < 0.0f) {
        a += 360.0f;
    }
    return a;
}

static void AddArc(lv_layer_t* layer, float cx, float cy, float radius, float width, float start,
                   float end, lv_color_t color, lv_opa_t opa) {
    if (radius < 1.0f || width < 0.5f || opa == 0) {
        return;
    }
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.color = color;
    d.opa = opa;
    d.width = static_cast<int32_t>(width);
    d.center.x = static_cast<int32_t>(cx);
    d.center.y = static_cast<int32_t>(cy);
    d.radius = static_cast<uint16_t>(radius);
    d.start_angle = static_cast<lv_value_precise_t>(NormAngle(start));
    d.end_angle = static_cast<lv_value_precise_t>(NormAngle(end));
    d.rounded = 1;
    lv_draw_arc(layer, &d);
}

// Triangle helper. Only used for opaque fills (white on white overlaps stay
// white; the sweat drop is the only translucent use and it fades as a whole).
static void AddTri(lv_layer_t* layer, lv_color_t color, float x0, float y0, float x1, float y1,
                   float x2, float y2, lv_opa_t opa) {
    if (opa == 0) {
        return;
    }
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = color;
    t.opa = opa;
    t.p[0].x = static_cast<lv_value_precise_t>(x0);
    t.p[0].y = static_cast<lv_value_precise_t>(y0);
    t.p[1].x = static_cast<lv_value_precise_t>(x1);
    t.p[1].y = static_cast<lv_value_precise_t>(y1);
    t.p[2].x = static_cast<lv_value_precise_t>(x2);
    t.p[2].y = static_cast<lv_value_precise_t>(y2);
    lv_draw_triangle(layer, &t);
}

// Counter-clockwise rotation of an offset vector on screen coordinates (y down).
static inline void RotVec(float x, float y, float c, float s, float* ox, float* oy) {
    *ox = x * c + y * s;
    *oy = -x * s + y * c;
}

// ---------------------------------------------------------------------------

GrokbotAvatar::~GrokbotAvatar() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (obj_ != nullptr) {
        lv_obj_delete(obj_);
        obj_ = nullptr;
    }
}

void GrokbotAvatar::Create(lv_obj_t* parent, int screen_w, int screen_h) {
    width_ = screen_w;
    height_ = screen_h;
    u_ = static_cast<float>((screen_w < screen_h ? screen_w : screen_h)) / 360.0f;

    obj_ = lv_obj_create(parent);
    lv_obj_remove_style_all(obj_);
    lv_obj_set_size(obj_, screen_w, screen_h);
    lv_obj_align(obj_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(obj_, DrawEvent, LV_EVENT_DRAW_MAIN, this);

    last_ms_ = lv_tick_get();
    next_blink_ms_ = last_ms_ + 1400 + (esp_random() % 2200);
    next_swap_ms_ = last_ms_ + 3000 + (esp_random() % 2600);

    // 60 fps: the body is static and only the eye rect is invalidated, so a
    // 16 ms tick still leaves the QSPI flush far below its bandwidth budget.
    timer_ = lv_timer_create(TimerEvent, 16, this);

    // Boot starts on the calm neutral face: no random showcase and no shape
    // morph while the status text is still churning. The lids open once by
    // themselves (see BlinkAmount) and the showcase begins after a beat.
    ApplyState("neutral");
    mode_ = Mode::Standby;
    next_swap_ms_ = lv_tick_get() + 4200 + (esp_random() % 1600);
    lv_obj_invalidate(lv_screen_active());
}

void GrokbotAvatar::SetMode(Mode mode) {
    if (mode_ == mode) {
        return;
    }
    mode_ = mode;
    emotion_hold_ = 0.0f;
    ApplyModeState();
}

void GrokbotAvatar::SetEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return;
    }
    // Standby runs its own random showcase, listening keeps the attentive face
    // and provisioning keeps the searching face; only speaking/other honour it.
    if (mode_ == Mode::Standby || mode_ == Mode::Listening || mode_ == Mode::Provisioning) {
        return;
    }
    const char* key = MapEmotion(emotion);
    ApplyState(key);
    if (mode_ == Mode::Speaking) {
        emotion_hold_ = 1.8f;  // fall back to the talking face after a beat
    }
}

void GrokbotAvatar::SetPaused(bool paused) {
    if (paused_ == paused) {
        return;
    }
    paused_ = paused;
    if (timer_ != nullptr) {
        if (paused) {
            lv_timer_pause(timer_);
        } else {
            last_ms_ = lv_tick_get();
            lv_timer_resume(timer_);
        }
    }
}

void GrokbotAvatar::DrawEvent(lv_event_t* e) {
    auto self = static_cast<GrokbotAvatar*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (self != nullptr && layer != nullptr) {
        self->Draw(layer);
    }
}

void GrokbotAvatar::TimerEvent(lv_timer_t* t) {
    auto self = static_cast<GrokbotAvatar*>(lv_timer_get_user_data(t));
    if (self == nullptr || self->obj_ == nullptr || self->paused_) {
        return;
    }
    uint32_t now = lv_tick_get();
    float dt = static_cast<float>(now - self->last_ms_) / 1000.0f;
    self->last_ms_ = now;
    self->Update(Clampf(dt, 0.001f, 0.100f));

    // Only the face area changes (the body is static). Each eye is invalidated
    // on its own: one merged rect spanning both eyes is ~300x300 px on this
    // panel (~170 KB of flush per frame), which cannot fit a 16 ms frame
    // together with the SDF rasterisation. Two per-eye rects roughly halve the
    // transferred pixels and are what makes the opening lids animate smoothly.
    lv_area_t c;
    lv_obj_get_coords(self->obj_, &c);
    float u = self->u_;
    float cx = static_cast<float>(c.x1) + static_cast<float>(self->width_) * 0.5f;
    float cy = static_cast<float>(c.y1) + static_cast<float>(self->height_) * 0.5f;

    // Rect per eye, from the *actual* drawn geometry: a rotated shape's box is
    // (|w cos| + |h sin|) x (|w sin| + |h cos|), and a blinking eye is squashed
    // vertically. The old max(w,h)/2 estimate clipped the tilted "cool" beams
    // horizontally (their inner ends fell outside the rect), which is what
    // truncated the sunglasses bridge and left slivers behind on the next state.
    float blink = self->BlinkAmount();
    float sq = 1.0f - 0.92f * Clampf(blink, 0.0f, 1.0f);
    float pad = 12.0f + self->bounce_;

    float ex_l, ey_l, ex_r, ey_r;
    {
        float r = self->eye_rot_l_ * kDeg;
        float ac = fabsf(cosf(r)), as = fabsf(sinf(r));
        float hh = self->eye_h_l_ * sq;
        ex_l = (self->eye_w_l_ * ac + hh * as) * 0.5f;
        ey_l = (self->eye_w_l_ * as + (hh > self->eye_w_l_ ? hh : self->eye_w_l_) * ac) * 0.5f;
    }
    {
        float r = self->eye_rot_r_ * kDeg;
        float ac = fabsf(cosf(r)), as = fabsf(sinf(r));
        float hh = self->eye_h_r_ * sq;
        ex_r = (self->eye_w_r_ * ac + hh * as) * 0.5f;
        ey_r = (self->eye_w_r_ * as + (hh > self->eye_w_r_ ? hh : self->eye_w_r_) * ac) * 0.5f;
    }
    if (self->blend_l_ < 0.999f) {
        ex_l *= 1.25f;
        ey_l *= 1.25f;
    }
    if (self->blend_r_ < 0.999f) {
        ex_r *= 1.25f;
        ey_r *= 1.25f;
    }

    // Each rect is centred on its own eye, not on the screen: centring both on
    // the screen centre made the left rect span the whole face again.
    float ex_c_l = cx + self->eye_dx_l_ * u;
    float ex_c_r = cx + self->eye_dx_r_ * u;
    lv_area_t face_l, face_r;
    face_l.x1 = static_cast<int32_t>(ex_c_l - (ex_l + pad) * u);
    face_l.x2 = static_cast<int32_t>(ex_c_l + (ex_l + pad) * u);
    face_l.y1 = static_cast<int32_t>(cy + (self->eye_dy_l_ - ey_l - pad) * u);
    face_l.y2 = static_cast<int32_t>(cy + (self->eye_dy_l_ + ey_l + pad) * u);
    face_r.x1 = static_cast<int32_t>(ex_c_r - (ex_r + pad) * u);
    face_r.x2 = static_cast<int32_t>(ex_c_r + (ex_r + pad) * u);
    face_r.y1 = static_cast<int32_t>(cy + (self->eye_dy_r_ - ey_r - pad) * u);
    face_r.y2 = static_cast<int32_t>(cy + (self->eye_dy_r_ + ey_r + pad) * u);

    // The sunglasses bridge sits between the eyes, so both rects must reach
    // over it; otherwise its outer ends are never repainted and stay on screen
    // (a grey stub) after switching to another expression.
    if (self->bridge_a_ > 0.03f) {
        float bx = (20.0f + pad) * u;
        if (cx + bx > face_l.x2) face_l.x2 = static_cast<int32_t>(cx + bx);
        if (cx - bx < face_r.x1) face_r.x1 = static_cast<int32_t>(cx - bx);
    }
    if (self->tears_a_ > 0.03f || self->blush_a_ > 0.03f) {
        float aux_x = (100.0f + pad) * u;
        float aux_y = (90.0f + pad) * u;
        if (cx - aux_x < face_l.x1) face_l.x1 = static_cast<int32_t>(cx - aux_x);
        if (cx + aux_x > face_r.x2) face_r.x2 = static_cast<int32_t>(cx + aux_x);
        if (cy + aux_y > face_l.y2) face_l.y2 = static_cast<int32_t>(cy + aux_y);
        if (cy + aux_y > face_r.y2) face_r.y2 = static_cast<int32_t>(cy + aux_y);
    }
    if (self->sweat_a_ > 0.03f) {
        float sx = (140.0f + pad) * u;
        float sy = (118.0f + pad) * u;
        if (cx + sx > face_r.x2) face_r.x2 = static_cast<int32_t>(cx + sx);
        if (cy - sy < face_r.y1) face_r.y1 = static_cast<int32_t>(cy - sy);
    }

    // Union with the previous frame's rect: the morph draws the outgoing shape
    // at its own (wider) offsets while the incoming state defines this rect, and
    // the bounce moves the eyes vertically. Repainting only the new rect leaves
    // the parts that fall outside it on screen as ghost fragments.
    lv_area_t inval_l = face_l;
    if (self->has_prev_area_l_) {
        if (self->prev_area_l_.x1 < inval_l.x1) inval_l.x1 = self->prev_area_l_.x1;
        if (self->prev_area_l_.y1 < inval_l.y1) inval_l.y1 = self->prev_area_l_.y1;
        if (self->prev_area_l_.x2 > inval_l.x2) inval_l.x2 = self->prev_area_l_.x2;
        if (self->prev_area_l_.y2 > inval_l.y2) inval_l.y2 = self->prev_area_l_.y2;
    }
    lv_area_t inval_r = face_r;
    if (self->has_prev_area_r_) {
        if (self->prev_area_r_.x1 < inval_r.x1) inval_r.x1 = self->prev_area_r_.x1;
        if (self->prev_area_r_.y1 < inval_r.y1) inval_r.y1 = self->prev_area_r_.y1;
        if (self->prev_area_r_.x2 > inval_r.x2) inval_r.x2 = self->prev_area_r_.x2;
        if (self->prev_area_r_.y2 > inval_r.y2) inval_r.y2 = self->prev_area_r_.y2;
    }
    self->prev_area_l_ = face_l;
    self->has_prev_area_l_ = true;
    self->prev_area_r_ = face_r;
    self->has_prev_area_r_ = true;

    if (self->boot_full_frames_ < 2) {
        // First couple of frames after boot: repaint everything once so any
        // pixel the panel still holds from the boot stage is overwritten by the
        // body. A full 360x360 flush does not fit 60 fps, so stop right after.
        self->boot_full_frames_++;
        lv_obj_invalidate(self->obj_);
        return;
    }
    lv_obj_invalidate_area(self->obj_, &inval_l);
    lv_obj_invalidate_area(self->obj_, &inval_r);
}    

float GrokbotAvatar::BlinkAmount() const {
    float blink = 0.0f;
    if (blink_t_ >= 0.0f) {
        blink = Clampf(sinf(kPi * blink_t_ / 0.26f), 0.0f, 1.0f);
    }
    // Boot entrance: lids shut until StartIntro(), then a normal-speed open
    // (~0.34 s) instead of a slow cinematic one. Ease-out so the very first
    // frame already moves: with an ease-in the start looked frozen and then
    // snapped open.
    if (intro_t_ < 0.0f) {
        return 1.0f;
    }
    if (intro_t_ < 0.6f) {
        float p = Clampf((intro_t_ - 0.03f) / 0.30f, 0.0f, 1.0f);
        float e = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
        float shut = 1.0f - e;
        if (shut > blink) {
            blink = shut;
        }
    }
    return blink;
}

void GrokbotAvatar::StartIntro() {
    if (intro_t_ < 0.0f) {
        intro_t_ = 0.0f;
    }
}

void GrokbotAvatar::Update(float dt) {
    time_ += dt;
    if (intro_t_ >= 0.0f && intro_t_ < 2.0f) {
        intro_t_ += dt;  // boot entrance progress, saturates after 2 s
    }
    bounce_phase_ += dt * 2.0f * kPi / (bounce_period_ > 0.4f ? bounce_period_ : 1.0f);
    breath_phase_ += dt * 2.0f * kPi / 4.6f;

    uint32_t now = lv_tick_get();

    // Blink: quick close/open envelope, scheduled with jitter.
    if (blink_t_ >= 0.0f) {
        blink_t_ += dt;
        if (blink_t_ > 0.26f) {
            blink_t_ = -1.0f;
            next_blink_ms_ = now + 2200 + (esp_random() % 3800);
        }
    } else if (now >= next_blink_ms_) {
        blink_t_ = 0.0f;
    }

    if (mode_ == Mode::Standby && now >= next_swap_ms_) {
        PickStandbyState();
    }
    if (mode_ == Mode::Speaking && emotion_hold_ > 0.0f) {
        emotion_hold_ -= dt;
        if (emotion_hold_ <= 0.0f) {
            ApplyState("streaming");
        }
    }

    // Parameter springs. Sizes/positions are calmer than the motion.
    eye_w_l_ = Smooth(eye_w_l_, target_.w_l, 11.0f, dt);
    eye_h_l_ = Smooth(eye_h_l_, target_.h_l, 11.0f, dt);
    eye_dx_l_ = Smooth(eye_dx_l_, target_.dx_l, 10.0f, dt);
    eye_dy_l_ = Smooth(eye_dy_l_, target_.dy_l, 10.0f, dt);
    eye_rot_l_ = Smooth(eye_rot_l_, target_.rot_l, 9.0f, dt);
    eye_w_r_ = Smooth(eye_w_r_, target_.w_r, 11.0f, dt);
    eye_h_r_ = Smooth(eye_h_r_, target_.h_r, 11.0f, dt);
    eye_dx_r_ = Smooth(eye_dx_r_, target_.dx_r, 10.0f, dt);
    eye_dy_r_ = Smooth(eye_dy_r_, target_.dy_r, 10.0f, dt);
    eye_rot_r_ = Smooth(eye_rot_r_, target_.rot_r, 9.0f, dt);
    bounce_ = Smooth(bounce_, target_.bounce, 6.0f, dt);
    bounce_period_ = Smooth(bounce_period_, target_.bounce_period, 5.0f, dt);

    tears_a_ = Smooth(tears_a_, target_.tears ? 1.0f : 0.0f, target_.tears ? 4.0f : 3.0f, dt);
    blush_a_ = Smooth(blush_a_, target_.blush ? 1.0f : 0.0f, 4.0f, dt);
    sweat_a_ = Smooth(sweat_a_, target_.sweat ? 1.0f : 0.0f, 4.0f, dt);
    bridge_a_ = Smooth(bridge_a_, target_.bridge ? 1.0f : 0.0f, 12.0f, dt);

    float morph_rate = dt / 0.24f;
    if (blend_l_ < 1.0f) {
        blend_l_ = Clampf(blend_l_ + morph_rate, 0.0f, 1.0f);
    }
    if (blend_r_ < 1.0f) {
        blend_r_ = Clampf(blend_r_ + morph_rate, 0.0f, 1.0f);
    }
}

void GrokbotAvatar::ApplyModeState() {
    switch (mode_) {
        case Mode::Standby:
            if (intro_t_ < 1.0f) {
                // Still booting: keep the settled neutral face for a beat. The
                // random showcase morphing in mid-status-churn was the clutter.
                next_swap_ms_ = lv_tick_get() + 2800 + (esp_random() % 2400);
            } else {
                PickStandbyState();
            }
            break;
        case Mode::Listening:
            ApplyState("listening");
            break;
        case Mode::Speaking:
            ApplyState("streaming");
            break;
        case Mode::Provisioning:
            ApplyState("searching");
            break;
        case Mode::Other:
            ApplyState("neutral");
            break;
    }
}

void GrokbotAvatar::ApplyState(const char* key) {
    if (key == nullptr) {
        return;
    }
    const FaceState* f = FindState(key);
    if (f == nullptr) {
        f = FindState("neutral");
    }
    if (f == nullptr) {
        return;
    }
    if (strcmp(active_key_, key) == 0) {
        target_ = *f;
        target_ptr_ = f;
        return;
    }
    snprintf(active_key_, sizeof(active_key_), "%s", key);
    // While the boot lids are still shut the squish morph would be invisible
    // anyway; skipping it keeps the first frames calm.
    bool instant = intro_t_ < 1.05f;
    // Shape change: squish the old shape down, then grow the new one. Both are
    // drawn fully opaque, so overlapping primitives never show seams.
    if (!instant && f->eye_l != eye_l_) {
        prev_eye_l_ = eye_l_;
        blend_l_ = 0.0f;
    } else {
        blend_l_ = 1.0f;
    }
    if (!instant && f->eye_r != eye_r_) {
        prev_eye_r_ = eye_r_;
        blend_r_ = 0.0f;
    } else {
        blend_r_ = 1.0f;
    }
    eye_l_ = f->eye_l;
    eye_r_ = f->eye_r;
    target_ = *f;
    target_ptr_ = f;
}

void GrokbotAvatar::PickStandbyState() {
    // 14 distinct looks; never repeat the immediately previous one.
    static const char* kPool[] = {
        "happy",  "laughing", "funny",   "loving",  "winking",   "cool",   "relaxed",
        "delicious", "kissy", "confident", "silly", "surprised", "thinking", "embarrassed",
    };
    const int count = static_cast<int>(sizeof(kPool) / sizeof(kPool[0]));
    const char* pick = nullptr;
    for (int i = 0; i < 8; i++) {
        int idx = static_cast<int>(esp_random() % count);
        pick = kPool[idx];
        if (pick != last_standby_) {
            break;
        }
    }
    if (pick == nullptr) {
        pick = kPool[0];
    }
    last_standby_ = pick;
    ApplyState(pick);
    next_swap_ms_ = lv_tick_get() + 2600 + (esp_random() % 3400);
}

const char* GrokbotAvatar::MapEmotion(const char* emotion) {
    if (emotion == nullptr) {
        return "neutral";
    }
    static const char* kKnown[] = {
        "neutral",    "happy",   "laughing", "funny",      "sad",      "angry",   "crying",
        "loving",     "embarrassed", "surprised", "shocked", "thinking", "winking", "cool",
        "relaxed",    "delicious", "kissy", "confident", "sleepy", "silly", "confused",
    };
    for (auto name : kKnown) {
        if (strcmp(emotion, name) == 0) {
            return name;
        }
    }
    if (strcmp(emotion, "robot_2") == 0) {
        return "cool";
    }
    return "neutral";
}

const GrokbotAvatar::FaceState* GrokbotAvatar::FindState(const char* key) {
    typedef GrokbotAvatar::FaceState F;
    // Design pixels on a 360x360 canvas, eye centres at (180,180).
    // Each entry: kind/size/offset/tilt for the left eye, then the right eye,
    // then the bounce, then the accessory flags.
    static const struct NamedState {
        const char* name;
        F f;
    } kStates[] = {
        {"neutral", {EyeKind::Bean, EyeKind::Bean, 78, 120, -66, 0, 0, 78, 120, 66, 0, 0, 5.0f, 3.2f, false, false, false, false}},
        {"happy", {EyeKind::ArcUp, EyeKind::ArcUp, 100, 54, -70, -8, 0, 100, 54, 70, -8, 0, 7.0f, 2.6f, false, false, false, false}},
        {"laughing", {EyeKind::ArcUp, EyeKind::ArcUp, 118, 66, -76, -10, 0, 118, 66, 76, -10, 0, 11.0f, 1.8f, false, false, false, false}},
        {"funny", {EyeKind::ArcUp, EyeKind::Bean, 84, 42, -64, -8, 0, 74, 100, 62, 6, 10, 8.0f, 2.1f, false, true, false, false}},
        {"sad", {EyeKind::ArcDown, EyeKind::ArcDown, 74, 36, -54, 16, 18, 74, 36, 54, 16, -18, 1.5f, 4.5f, false, false, false, false}},
        {"angry", {EyeKind::Beam, EyeKind::Beam, 92, 30, -60, -6, -20, 92, 30, 60, -6, 20, 6.0f, 2.2f, false, false, false, false}},
        {"crying", {EyeKind::ArcDown, EyeKind::ArcDown, 96, 48, -60, 8, 10, 96, 48, 60, 8, -10, 1.2f, 5.0f, true, false, false, false}},
        {"loving", {EyeKind::Heart, EyeKind::Heart, 84, 76, -64, -2, 0, 84, 76, 64, -2, 0, 4.0f, 2.8f, false, true, false, false}},
        {"embarrassed", {EyeKind::Bean, EyeKind::Bean, 62, 80, -66, 10, -8, 62, 80, 66, 10, 8, 2.0f, 3.8f, false, true, false, false}},
        {"surprised", {EyeKind::Bean, EyeKind::Bean, 88, 140, -64, -10, 0, 88, 140, 64, -10, 0, 7.0f, 2.0f, false, false, false, false}},
        {"shocked", {EyeKind::Bean, EyeKind::Bean, 98, 160, -68, -14, 0, 98, 160, 68, -14, 0, 2.0f, 1.2f, false, false, true, false}},
        {"thinking", {EyeKind::Bean, EyeKind::Beam, 54, 70, -74, -20, -10, 76, 26, 56, -12, 8, 2.0f, 3.0f, false, false, false, false}},
        {"winking", {EyeKind::ArcDown, EyeKind::Bean, 88, 26, -60, -2, 0, 84, 118, 64, -2, 0, 4.0f, 2.9f, false, false, false, false}},
        {"cool", {EyeKind::Beam, EyeKind::Beam, 84, 46, -70, -2, -14, 84, 46, 70, -2, 14, 2.0f, 4.0f, false, false, false, true}},
        {"relaxed", {EyeKind::Bean, EyeKind::Bean, 70, 48, -64, 8, -12, 70, 48, 64, 8, 12, 2.0f, 4.8f, false, false, false, false}},
        {"delicious", {EyeKind::ArcUp, EyeKind::ArcUp, 96, 50, -68, -6, 0, 74, 38, 62, 2, 0, 7.0f, 1.9f, false, true, false, false}},
        {"kissy", {EyeKind::Heart, EyeKind::ArcDown, 78, 70, -64, -2, 0, 72, 24, 62, 4, 0, 4.0f, 2.2f, false, true, false, false}},
        {"confident", {EyeKind::Bean, EyeKind::Bean, 88, 68, -62, -6, -8, 74, 42, 64, -4, 10, 2.0f, 3.6f, false, false, false, false}},
        {"sleepy", {EyeKind::ArcDown, EyeKind::ArcDown, 82, 18, -60, 20, 0, 82, 18, 60, 20, 0, 1.0f, 5.5f, false, false, false, false}},
        {"silly", {EyeKind::Dot, EyeKind::Bean, 48, 48, -68, -8, 0, 92, 64, 62, -2, 14, 9.0f, 1.6f, false, true, false, false}},
        {"confused", {EyeKind::ArcDown, EyeKind::ArcUp, 80, 28, -60, -6, -12, 92, 40, 64, 6, 10, 3.0f, 3.2f, false, false, false, false}},
        // interaction-only faces
        {"listening", {EyeKind::Bean, EyeKind::Bean, 80, 124, -66, -4, -7, 80, 124, 66, -4, 7, 3.0f, 3.0f, false, false, false, false}},
        {"streaming", {EyeKind::Bean, EyeKind::Bean, 88, 130, -70, -6, -3, 88, 130, 70, -6, 3, 6.0f, 1.3f, false, false, false, false}},
        {"searching", {EyeKind::Bean, EyeKind::Beam, 56, 72, -74, -18, -10, 76, 26, 56, -12, 8, 3.0f, 2.4f, false, false, false, false}},
    };

    for (const auto& s : kStates) {
        if (strcmp(key, s.name) == 0) {
            return &s.f;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Coverage-mask rasteriser
//
// LVGL's software renderer antialiases rounded rects and arcs, but NOT lines
// (lv_draw_line fills every step with a hard rect) and not triangles. That is
// why hearts and rotated capsules kept showing pixel stairs however the opaque
// primitives were stacked. The face is therefore rasterised here into an 8-bit
// coverage mask from signed distance fields: one analytic 1px edge, the same
// formula for every expression, and rotation/squash fall out of the sampling.
// The mask is then blitted as an A8 image (recolor + opa), which also makes the
// blush a true ellipse instead of the pill that LV_RADIUS_CIRCLE produced.
constexpr int kMaskCap = 248;

// A rasterised mask is expensive (tens of thousands of square roots), so each
// slot keeps its buffer and the exact parameters it was built from. Idle frames
// only move the mask, which turns into a blit; only a real change re-rasterises.
struct MaskSlot {
    uint8_t* buf = nullptr;
    int cap = 0;
    int bw = 0;
    int bh = 0;
    int kind = -1;
    float w = 0.0f;
    float h = 0.0f;
    float rot = 0.0f;
    float sq = 0.0f;
    float bl = 0.0f;
    bool valid = false;
};

enum MaskKind : int {
    kMaskCapsule,
    kMaskDot,
    kMaskArcUp,
    kMaskArcDown,
    kMaskHeart,
    kMaskEllipse,
    kMaskDrop,
};

struct MaskShape {
    int kind = kMaskCapsule;
    float w = 0.0f;
    float h = 0.0f;
    float rot = 0.0f;
    float squash = 1.0f;  // vertical collapse about the centre (blink)
    float cosr = 1.0f;
    float sinr = 0.0f;
    float stroke = 0.0f;
    float lobe = 0.0f;
    float lox = 0.0f;
    float loy = 0.0f;
    float vx[3] = {0.0f, 0.0f, 0.0f};
    float vy[3] = {0.0f, 0.0f, 0.0f};
    float ew = 0.0f;     // ellipse half extents, used while blinking
    float eh = 0.0f;
    float tipy = 0.0f;   // heart tip disk centre
    float tradius = 0.0f;
    float blend = 0.0f;  // 0 = the expression's own shape, 1 = flat lid lens
    int bw = 0;
    int bh = 0;
};

// Ellipse as a normalised implicit gradient, good for antialiasing near the rim.
static inline float SdEllipse(float x, float y, float a, float b) {
    float fx = x / a;
    float fy = y / b;
    float f = fx * fx + fy * fy - 1.0f;
    float gx = 2.0f * fx / a;
    float gy = 2.0f * fy / b;
    float g = sqrtf(gx * gx + gy * gy);
    return g > 1e-4f ? f / g : -1e9f;
}

static inline float SdRoundBox(float x, float y, float hw, float hh, float r) {
    float qx = fabsf(x) - (hw - r);
    float qy = fabsf(y) - (hh - r);
    float ox = qx > 0.0f ? qx : 0.0f;
    float oy = qy > 0.0f ? qy : 0.0f;
    float m = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (m > 0.0f ? 0.0f : m) - r;
}

static inline float SdConvexTri(const MaskShape& s, float x, float y) {
    float best = -1e9f;
    for (int i = 0; i < 3; i++) {
        int j = (i + 1) % 3;
        int k = (i + 2) % 3;
        float ax = s.vx[i];
        float ay = s.vy[i];
        float ex = s.vx[j] - ax;
        float ey = s.vy[j] - ay;
        float len = sqrtf(ex * ex + ey * ey);
        if (len < 0.01f) {
            continue;
        }
        float nx = ey / len;
        float ny = -ex / len;
        if (nx * (s.vx[k] - ax) + ny * (s.vy[k] - ay) > 0.0f) {
            nx = -nx;
            ny = -ny;
        }
        float d = nx * (x - ax) + ny * (y - ay);
        if (d > best) {
            best = d;
        }
    }
    return best;
}

static float MaskDistance(const MaskShape& s, float px, float py) {
    // Screen -> shape space: undo the rotation, then undo the vertical squash.
    float lx = px * s.cosr - py * s.sinr;
    float ly = (px * s.sinr + py * s.cosr) / (s.squash > 0.02f ? s.squash : 0.02f);
    float d;
    switch (s.kind) {
        case kMaskCapsule: {
            float r = (s.w < s.h ? s.w : s.h) * 0.5f;
            d = SdRoundBox(lx, ly, s.w * 0.5f, s.h * 0.5f, r);
            break;
        }
        case kMaskDot: {
            float r = (s.w < s.h ? s.w : s.h) * 0.5f;
            d = sqrtf(lx * lx + ly * ly) - r;
            break;
        }
        case kMaskArcUp:
        case kMaskArcDown: {
            float len = sqrtf(lx * lx + ly * ly);
            float half = s.stroke * 0.5f;
            float rc = s.w * 0.5f - half;
            bool up = (s.kind == kMaskArcUp);
            if (up ? (ly <= 0.0f) : (ly >= 0.0f)) {
                d = fabsf(len - rc) - half;
            } else {
                // Outside the half circle: distance to the nearer round cap.
                float dx1 = lx - rc;
                float dx2 = lx + rc;
                float d1 = sqrtf(dx1 * dx1 + ly * ly) - half;
                float d2 = sqrtf(dx2 * dx2 + ly * ly) - half;
                d = d1 < d2 ? d1 : d2;
            }
            break;
        }
        case kMaskHeart: {
            // Implicit heart (u^2 + v^2 - 1)^3 = u^2 v^3: convex lobes, a soft
            // cleft and sides that are curves all the way down, unlike the
            // straight tangents of a circle-plus-triangle heart.
            float a = s.w * 0.4545f;  // 1/2.2: the curve spans about +/-1.1
            float b = s.h * 0.4545f;
            float u = lx / a;
            float v = -ly / b;
            float q = u * u + v * v - 1.0f;
            float q2 = q * q;
            float f = q2 * q - u * u * v * v * v;
            float gu = 6.0f * u * q2 - 2.0f * u * v * v * v;
            float gv = 6.0f * v * q2 - 3.0f * u * u * v * v;
            float g = sqrtf(gu * gu + gv * gv);
            d = g > 1e-5f ? (f / g) * (a < b ? a : b) : -1e9f;
            // The implicit curve is singular at its bottom cusp; without this cut
            // the normaliser leaks a one pixel spike below the tip.
            float cut = ly - b;
            d = d > cut ? d : cut;
            break;
        }
        case kMaskEllipse: {
            d = SdEllipse(lx, ly, s.w * 0.5f, s.h * 0.5f);
            break;
        }
        case kMaskDrop: {
            float d1 = sqrtf(lx * lx + (ly - s.loy) * (ly - s.loy)) - s.lobe;
            float d2 = SdConvexTri(s, lx, ly);
            d = d1 < d2 ? d1 : d2;
            break;
        }
        default:
            d = 1e9f;
            break;
    }

    // Blink closing: morph the field towards a flat lens so every expression
    // collapses into the same symmetric lid instead of a distorted blob.
    if (s.blend > 0.01f && s.ew > 0.5f && s.eh > 0.5f) {
        float de = SdEllipse(lx, ly, s.ew * 0.5f, s.eh * 0.5f);
        d = d * (1.0f - s.blend) + de * s.blend;
    }
    return d;

}

static void RasterMask(const MaskShape& s, uint8_t* buf) {
    for (int y = 0; y < s.bh; y++) {
        uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(s.bw);
        float py = static_cast<float>(y) - static_cast<float>(s.bh) * 0.5f + 0.5f;
        for (int x = 0; x < s.bw; x++) {
            float px = static_cast<float>(x) - static_cast<float>(s.bw) * 0.5f + 0.5f;
            float cov = 0.5f - MaskDistance(s, px, py);
            cov = cov < 0.0f ? 0.0f : (cov > 1.0f ? 1.0f : cov);
            row[x] = static_cast<uint8_t>(cov * 255.0f + 0.5f);
        }
    }
}

static void BlitMask(lv_layer_t* layer, const uint8_t* buf, int bw, int bh, float cx, float cy,
                     lv_color_t color, lv_opa_t opa) {
    if (bw <= 0 || bh <= 0 || opa == 0) {
        return;
    }
    lv_image_dsc_t img;
    lv_memzero(&img, sizeof(img));
    img.header.magic = LV_IMAGE_HEADER_MAGIC;
    img.header.cf = LV_COLOR_FORMAT_A8;
    img.header.w = static_cast<uint16_t>(bw);
    img.header.h = static_cast<uint16_t>(bh);
    img.header.stride = static_cast<uint16_t>(bw);
    img.data_size = static_cast<uint32_t>(bw) * static_cast<uint32_t>(bh);
    img.data = buf;

    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &img;
    d.recolor = color;
    d.recolor_opa = LV_OPA_COVER;
    d.opa = opa;

    lv_area_t coords;
    coords.x1 = static_cast<int32_t>(lroundf(cx - static_cast<float>(bw) * 0.5f));
    coords.y1 = static_cast<int32_t>(lroundf(cy - static_cast<float>(bh) * 0.5f));
    coords.x2 = coords.x1 + bw - 1;
    coords.y2 = coords.y1 + bh - 1;
    lv_draw_image(layer, &d, &coords);
}

static MaskSlot& EyeSlot(bool left) {
    static MaskSlot slots[2];
    return slots[left ? 0 : 1];
}

static MaskSlot& AuxSlot(int index) {
    static MaskSlot slots[3];
    return slots[index < 0 ? 0 : (index > 2 ? 2 : index)];
}

// Rasterise only when the shape actually changed, then blit it at (cx, cy).
static void DrawShape(lv_layer_t* layer, MaskSlot& slot, const MaskShape& s, float cx, float cy,
                     lv_color_t color, lv_opa_t opa) {
    if (opa == 0 || s.bw <= 0 || s.bh <= 0 || s.bw > kMaskCap || s.bh > kMaskCap) {
        return;
    }
    size_t bytes = static_cast<size_t>(s.bw) * static_cast<size_t>(s.bh);
    // Grow only, with headroom: morphing changes the box by a pixel every frame
    // and exact-fit reallocs would churn the heap on every transition frame.
    if (slot.cap < static_cast<int>(bytes)) {
        size_t want = bytes + bytes / 2;
        if (slot.buf != nullptr) {
            heap_caps_free(slot.buf);
            slot.buf = nullptr;
        }
        // Internal RAM first: the blend reads this mask every frame, and reading
        // it from PSRAM was a large part of the cost.
        slot.buf = static_cast<uint8_t*>(
            heap_caps_aligned_alloc(4, want, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (slot.buf == nullptr) {
            slot.buf = static_cast<uint8_t*>(
                heap_caps_aligned_alloc(4, want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        slot.cap = slot.buf != nullptr ? static_cast<int>(want) : 0;
        slot.valid = false;
    }
    if (slot.buf == nullptr) {
        return;
    }
    bool same = slot.valid && slot.kind == s.kind && slot.bw == s.bw && slot.bh == s.bh &&
                slot.w == s.w && slot.h == s.h && slot.rot == s.rot && slot.sq == s.squash &&
                slot.bl == s.blend;
    if (!same) {
        RasterMask(s, slot.buf);
        slot.valid = true;
        slot.kind = s.kind;
        slot.bw = s.bw;
        slot.bh = s.bh;
        slot.w = s.w;
        slot.h = s.h;
        slot.rot = s.rot;
        slot.sq = s.squash;
        slot.bl = s.blend;
    }
    BlitMask(layer, slot.buf, s.bw, s.bh, cx, cy, color, opa);
}

// Size the mask box from the shape's half extents (already squashed).
static void SizeMask(MaskShape* s, float ew, float eh) {
    float hw = fabsf(ew * s->cosr) + fabsf(eh * s->sinr) + 1.5f;
    float hh = fabsf(ew * s->sinr) + fabsf(eh * s->cosr) + 1.5f;
    int bw = static_cast<int>(ceilf(2.0f * hw));
    int bh = static_cast<int>(ceilf(2.0f * hh));
    if (bw < 2) {
        bw = 2;
    }
    if (bh < 2) {
        bh = 2;
    }
    if (bw > kMaskCap) {
        bw = kMaskCap;
    }
    if (bh > kMaskCap) {
        bh = kMaskCap;
    }
    s->bw = bw;
    s->bh = bh;
}

void GrokbotAvatar::DrawEye(lv_layer_t* layer, float cx, float cy, bool left, EyeKind kind,
                            float factor, float blink) {
    if (factor <= 0.01f) {
        return;
    }
    float u = u_;
    // Quantise the animation inputs: the springs never quite settle, and without
    // this every idle frame would look "different" and force a re-raster.
    float w = Q( (left ? eye_w_l_ : eye_w_r_) * factor * u, 0.5f);
    float h = Q( (left ? eye_h_l_ : eye_h_r_) * factor * u, 0.5f);
    float ex = cx + (left ? eye_dx_l_ : eye_dx_r_) * u;
    float ey = cy + (left ? eye_dy_l_ : eye_dy_r_) * u + sinf(bounce_phase_) * bounce_ * u;
    float rot_deg = Q( (left ? eye_rot_l_ : eye_rot_r_), 0.5f);

    MaskShape s;
    s.w = w;
    s.h = h;
    s.rot = rot_deg * kDeg;
    s.cosr = cosf(s.rot);
    s.sinr = sinf(s.rot);
    // Symmetric blink: the whole shape collapses about its own centre, so the
    // lid edge moves the same amount from the top and the bottom and the eye
    // reopens along exactly the same path. No per-kind special casing.
    s.squash = Q(1.0f - 0.92f * Clampf(blink, 0.0f, 1.0f), 0.01f);
    s.blend = Q(Clampf((Clampf(blink, 0.0f, 1.0f) - 0.30f) * 1.43f, 0.0f, 1.0f), 0.02f);

    float ew;
    float eh;
    switch (kind) {
        case EyeKind::Bean:
        case EyeKind::Beam:
            s.kind = kMaskCapsule;
            break;
        case EyeKind::Dot:
            s.kind = kMaskDot;
            break;
        case EyeKind::ArcUp:
            s.kind = kMaskArcUp;
            break;
        case EyeKind::ArcDown:
            s.kind = kMaskArcDown;
            break;
        case EyeKind::Heart:
            s.kind = kMaskHeart;
            break;
        default:
            s.kind = kMaskCapsule;
            break;
    }

    if (s.kind == kMaskArcUp || s.kind == kMaskArcDown) {
        s.stroke = (h > 19.5f * u ? h * 0.46f : 9.0f * u);
        if (s.stroke > w * 0.5f) {
            s.stroke = w * 0.5f;
        }
        ew = w * 0.5f;
        eh = w * 0.5f;
    } else {
        ew = w * 0.5f;
        eh = h * 0.5f;
    }

    if (s.kind == kMaskHeart) {
        // Two lobes plus a V whose sides are the tangents from the tip to the
        // lobes: the union has no corner, and the rasteriser antialiases it.
        s.lobe = w * 0.29f;
        s.lox = w * 0.19f;
        s.loy = -h * 0.12f;
        float tipy = h * 0.52f;
        float rt = w * 0.030f;             // tip rounding
        float tipy_in = tipy - rt;         // V polygon ends where the disk starts
        float txl = 0.0f, tyl = 0.0f, txr = 0.0f, tyr = 0.0f;
        for (int side = -1; side <= 1; side += 2) {
            float ccx = side * s.lox;
            float dx = -ccx;
            float dy = tipy_in - s.loy;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist <= s.lobe + 0.5f) {
                continue;
            }
            float th = atan2f(dy, dx);
            float al = acosf(s.lobe / dist);
            float ax = ccx + s.lobe * cosf(th + al);
            float ay = s.loy + s.lobe * sinf(th + al);
            float bx = ccx + s.lobe * cosf(th - al);
            float by = s.loy + s.lobe * sinf(th - al);
            bool use_a = fabsf(ax) > fabsf(bx);
            float px = use_a ? ax : bx;
            float py = use_a ? ay : by;
            if (side < 0) {
                txl = px;
                tyl = py;
            } else {
                txr = px;
                tyr = py;
            }
        }
        s.vx[0] = txl;
        s.vy[0] = tyl;
        s.vx[1] = txr;
        s.vy[1] = tyr;
        s.vx[2] = 0.0f;
        s.vy[2] = tipy_in;
        s.tipy = tipy_in;
        s.tradius = rt;
        float top = -s.loy + s.lobe;   // above the centre
        float bot = tipy;              // below the centre
        eh = (top > bot ? top : bot) + 1.0f;
        ew = w * 0.5f;
    }

    s.ew = ew * 2.0f;
    s.eh = eh * 2.0f;
    SizeMask(&s, ew, eh * s.squash);
    DrawShape(layer, EyeSlot(left), s, ex, ey, lv_color_white(), LV_OPA_COVER);
}

void GrokbotAvatar::DrawBody(lv_layer_t* layer, float cx, float cy) {
    // The blob is the screen: one flat near-black fill, edge to edge. Nothing is
    // drawn near the top rim, so no bar or cap can appear there.
    (void)cx;
    (void)cy;
    AddRRect(layer, static_cast<float>(width_) * 0.5f, static_cast<float>(height_) * 0.5f,
             static_cast<float>(width_), static_cast<float>(height_), 0,
             lv_color_hex(kBodyColor), LV_OPA_COVER);
}

void GrokbotAvatar::DrawBlush(lv_layer_t* layer, float cx, float cy) {
    if (blush_a_ < 0.03f) {
        return;
    }
    float u = u_;
    lv_color_t blush = lv_color_hex(0xFF6E91);
    lv_opa_t opa = MixOpa(blush_a_, 92.0f);
    float bob = sinf(time_ * 2.2f) * 1.2f * u;

    // A real ellipse: LV_RADIUS_CIRCLE on a 36x22 rect only ever made a pill.
    MaskShape s;
    s.kind = kMaskEllipse;
    s.w = 36.0f * u;
    s.h = 22.0f * u;
    SizeMask(&s, s.w * 0.5f, s.h * 0.5f);
    MaskSlot& slot = AuxSlot(0);

    for (int side = 0; side < 2; side++) {
        float dx = (side == 0 ? eye_dx_l_ : eye_dx_r_) * u;
        float h = (side == 0 ? eye_h_l_ : eye_h_r_) * u;
        float dy = (side == 0 ? eye_dy_l_ : eye_dy_r_) * u;
        DrawShape(layer, slot, s, cx + dx, cy + dy + h * 0.5f + 22.0f * u + bob, blush, opa);
    }
}

void GrokbotAvatar::DrawTears(lv_layer_t* layer, float cx, float cy) {
    if (tears_a_ < 0.03f) {
        return;
    }
    float u = u_;
    lv_color_t tear = lv_color_hex(0xAAE0FF);
    lv_opa_t opa = MixOpa(tears_a_, 245.0f);

    MaskShape s;
    s.kind = kMaskEllipse;
    s.w = 18.0f * u;
    s.h = 34.0f * u;
    SizeMask(&s, s.w * 0.5f, s.h * 0.5f);
    MaskSlot& slot = AuxSlot(1);

    for (int side = -1; side <= 1; side += 2) {
        float bob = sinf(time_ * 3.2f + (side > 0 ? 0.9f : 0.0f)) * 2.0f * u;
        DrawShape(layer, slot, s, cx + side * 74.0f * u, cy + 56.0f * u + bob, tear, opa);
    }
}

void GrokbotAvatar::DrawSweat(lv_layer_t* layer, float cx, float cy) {
    if (sweat_a_ < 0.03f) {
        return;
    }
    float u = u_;
    lv_color_t color = lv_color_hex(0xAADCFF);
    lv_opa_t opa = MixOpa(sweat_a_, 220.0f);

    // Teardrop: circle at the bottom, triangle from the top tip to its widest
    // point, unioned in the distance field so the sides are one smooth edge.
    MaskShape s;
    s.kind = kMaskDrop;
    s.w = 20.0f * u;
    s.h = 36.0f * u;
    s.lobe = s.w * 0.5f;
    s.loy = s.h * 0.5f - s.lobe;
    s.vx[0] = 0.0f;
    s.vy[0] = -s.h * 0.5f;
    s.vx[1] = -s.lobe;
    s.vy[1] = s.loy;
    s.vx[2] = s.lobe;
    s.vy[2] = s.loy;
    SizeMask(&s, s.w * 0.5f, s.h * 0.5f);

    DrawShape(layer, AuxSlot(2), s, cx + 126.0f * u, cy - 100.0f * u, color, opa);
}

void GrokbotAvatar::DrawBridge(lv_layer_t* layer, float cx, float cy) {
    if (bridge_a_ < 0.05f) {
        return;
    }
    float u = u_;
    // Solid, never faded: a half-opaque white bar over the near-black body
    // reads as grey, which is how the "cool" sunglasses looked while the
    // prop faded in/out.
    AddRRect(layer, cx, cy - 2.0f * u, 26.0f * u, 10.0f * u, 5.0f * u, lv_color_white(),
             LV_OPA_COVER);
}

void GrokbotAvatar::Draw(lv_layer_t* layer) {
    lv_area_t c;
    lv_obj_get_coords(obj_, &c);
    // The body is static on purpose: only the face area is invalidated each
    // frame, so the blob must not drift underneath it.
    float cx = static_cast<float>(c.x1) + static_cast<float>(width_) * 0.5f;
    float cy = static_cast<float>(c.y1) + static_cast<float>(height_) * 0.5f;

    DrawBody(layer, cx, cy);

    float blink = BlinkAmount();

    // Shape morph: shrink the outgoing shape to 0.4, then grow the incoming one
    // from 0.4. Both halves are fully opaque, which is what removes the seams
    // that a transparent cross-fade produced on multi-primitive eyes.
    if (blend_l_ < 0.999f) {
        if (blend_l_ < 0.5f) {
            DrawEye(layer, cx, cy, true, prev_eye_l_, 1.0f - 1.2f * blend_l_, blink);
        } else {
            DrawEye(layer, cx, cy, true, eye_l_, 0.4f + 1.2f * (blend_l_ - 0.5f), blink);
        }
    } else {
        DrawEye(layer, cx, cy, true, eye_l_, 1.0f, blink);
    }
    if (blend_r_ < 0.999f) {
        if (blend_r_ < 0.5f) {
            DrawEye(layer, cx, cy, false, prev_eye_r_, 1.0f - 1.2f * blend_r_, blink);
        } else {
            DrawEye(layer, cx, cy, false, eye_r_, 0.4f + 1.2f * (blend_r_ - 0.5f), blink);
        }
    } else {
        DrawEye(layer, cx, cy, false, eye_r_, 1.0f, blink);
    }

    DrawBlush(layer, cx, cy);
    DrawBridge(layer, cx, cy);
    DrawTears(layer, cx, cy);
    DrawSweat(layer, cx, cy);
}
