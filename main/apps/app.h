// App framework for the GrokBot round screen.
//
// An "app" owns the whole 360x360 screen while it runs: the assistant face is
// hidden, the wake word is gated off, and the single physical button means
// "back to the desktop". Apps build their UI into an LVGL container supplied by
// AppHost; nothing is kept running behind the desktop.
#pragma once

#include <lvgl.h>

class AppHost;

// Draws an 80x80 icon at (x, y) inside the given parent. Icons are plain LVGL
// draw calls (see app_icons.cc); apps without one get a plain accent dot.
using AppIconDrawer = void (*)(lv_obj_t* parent, int x, int y);

struct AppInfo {
    const char* id;     // stable key used by voice commands (e.g. "demo")
    const char* name;   // shown to the user / returned by self.apps.list
    uint32_t accent;    // fallback accent colour in the app center (0xRRGGBB)
    AppIconDrawer draw_icon = nullptr;
};

// Passed to App::OnEnter. The scene root is a full-screen transparent container
// owned by AppHost; the app creates its widgets inside it.
class AppContext {
public:
    AppContext(AppHost* host, lv_obj_t* root, int width, int height, const lv_font_t* font)
        : host_(host), root_(root), width_(width), height_(height), font_(font) {}

    lv_obj_t* root() const { return root_; }
    int width() const { return width_; }
    int height() const { return height_; }
    // UI text font from the current LVGL theme (supports Chinese); may be null.
    const lv_font_t* font() const { return font_; }

    // Leave the app and go back to the desktop. Deferred to the next LVGL tick,
    // so it is safe to call from inside an event handler or a background task
    // (as long as the LVGL lock is held when calling).
    void RequestExit();

    // Switch to another app while keeping the assistant suspended.
    void SwitchTo(const char* id);

private:
    AppHost* host_;
    lv_obj_t* root_;
    int width_;
    int height_;
    const lv_font_t* font_;
};

class App {
public:
    virtual ~App() = default;

    virtual const AppInfo& info() const = 0;

    // Build the UI and start any work. Called once per entry, with the LVGL
    // lock held. The root is already empty and sized to the screen.
    virtual void OnEnter(AppContext& ctx) = 0;

    // The control center is covering the app (pause cameras, previews, timers).
    virtual void OnPause() {}
    virtual void OnResume() {}

    // Stop work and release resources. Widgets are deleted by AppHost afterwards,
    // so OnExit only needs to handle non-LVGL resources and tasks.
    virtual void OnExit() {}
};

struct AppEntry {
    const AppInfo* info;
    App* (*create)();
};

// Built-in app registry, defined in apps_manifest.cc (one line per app).
const AppEntry* GetAppRegistry(int* count);
