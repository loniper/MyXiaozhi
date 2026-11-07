#pragma once

#include "lvgl.h"

/* The control-centre overlay is provided by AppHost; radio pages only need the
 * legacy hook so the ported code keeps compiling. */
void sdgoods_cc_bind(lv_obj_t *root);
