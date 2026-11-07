// Network radio app: ports the sdgoods-radio screens into the app framework.
#include "app.h"
#include "app_icons.h"

#include <lvgl.h>

#include "ui_radio.h"

extern const AppInfo kRadioInfo = {"radio", "网络电台", 0xFFB45C, DrawRadioIcon};

namespace {

class RadioApp : public App {
public:
    const AppInfo& info() const override { return kRadioInfo; }

    void OnEnter(AppContext& ctx) override {
        ui_radio_start(ctx.root());
    }

    void OnPause() override { ui_radio_pause(); }
    void OnResume() override { ui_radio_resume(); }

    void OnExit() override { ui_radio_close(); }
};

}  // namespace

App* CreateRadioApp() {
    return new RadioApp();
}
