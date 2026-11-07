// Owns the full-screen app layer and the lifecycle of at most one running app.
// All methods must be called with the LVGL display lock held.
#pragma once

#include "app.h"

#include <functional>
#include <lvgl.h>
#include <string>

class AppHost {
public:
    struct Hooks {
        // Called before the app builds its UI: hide the assistant surface and
        // close the wake word gate.
        std::function<void()> on_enter;
        // Called after the scene has been destroyed: restore the desktop.
        std::function<void()> on_exit;
    };

    bool Init(lv_obj_t* parent, int width, int height, const lv_font_t* font, Hooks hooks);
    // The theme font can be swapped at runtime (dynamic font assets), so the board
    // refreshes it right before every entry instead of relying on the Init value.
    void SetFont(const lv_font_t* font) { font_ = font; }

    bool Enter(const char* id);
    void Exit();
    void RequestExitAsync();
    // Switch directly from one app to another without flashing the desktop:
    // the assistant stays suspended and the avatar stays hidden.
    void SwitchToAsync(const char* id);

    bool IsActive() const { return app_ != nullptr; }
    bool exit_pending() const { return exit_pending_; }
    const AppInfo* active_info() const;
    lv_obj_t* layer() const { return layer_; }
    const AppEntry* Find(const char* id) const;

    // Control center opened (true) / closed (false) while an app is running.
    void PauseForOverlay(bool paused);

private:
    static void DeferredExit(void* arg);
    static void DeferredSwitch(void* arg);
    void DoExit();
    bool EnterInternal(const char* id);

    lv_obj_t* layer_ = nullptr;
    App* app_ = nullptr;
    const AppEntry* entry_ = nullptr;
    AppContext* ctx_ = nullptr;
    Hooks hooks_;
    const lv_font_t* font_ = nullptr;
    std::string pending_id_;
    int width_ = 360;
    int height_ = 360;
    bool overlay_paused_ = false;
    bool exit_pending_ = false;
    bool suppress_exit_hook_ = false;  // app-to-app switch: keep the desktop suspended
};
