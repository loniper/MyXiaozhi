// Legacy app-shell hooks expected by the ported radio code.
// AppHost owns screen switching and the BOOT button, so these are thin
// callbacks only (the radio app calls them itself).
#pragma once

#include <stdbool.h>

#include "lvgl.h"

typedef void (*sdgoods_shell_cb_t)(void);

void sdgoods_app_shell_bind(lv_obj_t *root);
void sdgoods_app_shell_set_exit_cb(sdgoods_shell_cb_t cb);
void sdgoods_app_shell_set_pause_cb(sdgoods_shell_cb_t cb);
void sdgoods_app_shell_set_resume_cb(sdgoods_shell_cb_t cb);
void sdgoods_app_shell_leave(bool back_to_launcher);
