// Vector app icons for the round-screen app center.
//
// Each icon draws itself into an 80x80 transparent container supplied by the
// caller. The artwork is plain LVGL draw calls, so no bitmap assets or icon
// fonts are needed and the whole set stays a few hundred bytes of flash.
#pragma once

#include <lvgl.h>

void DrawRadioIcon(lv_obj_t* parent, int x, int y);
void DrawDeviceInfoIcon(lv_obj_t* parent, int x, int y);
void DrawDemoIcon(lv_obj_t* parent, int x, int y);
