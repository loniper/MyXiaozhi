// Chinese-only i18n shim for the ported radio app (no NVS switching).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { SDG_LANG_EN = 0, SDG_LANG_ZH = 1 } sdg_lang_t;

void        sdg_i18n_init(void);
sdg_lang_t  sdg_i18n_get(void);
bool        sdg_i18n_is_zh(void);
void        sdg_i18n_set(sdg_lang_t lang);
void        sdg_i18n_toggle(void);
uint32_t    sdg_i18n_seq(void);
const char *sdg_i18n_pick(const char *zh, const char *en);
const char *sdg_i18n_code(void);

#define SDG_T(zh, en) sdg_i18n_pick((zh), (en))
