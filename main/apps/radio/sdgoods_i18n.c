#include "sdgoods_i18n.h"

static sdg_lang_t s_lang = SDG_LANG_ZH;
static uint32_t s_seq = 1;

void sdg_i18n_init(void) {}
sdg_lang_t sdg_i18n_get(void) { return s_lang; }
bool sdg_i18n_is_zh(void) { return s_lang == SDG_LANG_ZH; }

void sdg_i18n_set(sdg_lang_t lang)
{
    if (lang != s_lang) {
        s_lang = lang;
        ++s_seq;
    }
}

void sdg_i18n_toggle(void)
{
    sdg_i18n_set(s_lang == SDG_LANG_ZH ? SDG_LANG_EN : SDG_LANG_ZH);
}

uint32_t sdg_i18n_seq(void) { return s_seq; }

const char *sdg_i18n_pick(const char *zh, const char *en)
{
    return s_lang == SDG_LANG_ZH ? zh : en;
}

const char *sdg_i18n_code(void) { return s_lang == SDG_LANG_ZH ? "zh" : "en"; }
