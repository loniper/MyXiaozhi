#include "sdgoods_app_shell.h"

static sdgoods_shell_cb_t s_exit_cb;
static sdgoods_shell_cb_t s_pause_cb;
static sdgoods_shell_cb_t s_resume_cb;

void sdgoods_app_shell_bind(lv_obj_t *root) { (void)root; }
void sdgoods_app_shell_set_exit_cb(sdgoods_shell_cb_t cb) { s_exit_cb = cb; }
void sdgoods_app_shell_set_pause_cb(sdgoods_shell_cb_t cb) { s_pause_cb = cb; }
void sdgoods_app_shell_set_resume_cb(sdgoods_shell_cb_t cb) { s_resume_cb = cb; }

void sdgoods_app_shell_leave(bool back_to_launcher)
{
    (void)back_to_launcher;
    if (s_exit_cb) {
        s_exit_cb();
    }
}
