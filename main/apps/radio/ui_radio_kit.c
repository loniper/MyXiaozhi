/*
 * sdgoods-radio · UI 工具箱实现
 *
 * 全部自绘。为什么不用 lv_canvas：canvas 需要一块 w*h*2 的常驻缓冲，
 * 每次改内容都要整块重传，圆屏上反而更慢；而且 16bit swap 下还要额外验证字节序。
 * 自绘（LV_EVENT_DRAW_MAIN）只在失效区域重画，没有缓冲、没有字节序风险。
 */

#include "ui_radio_kit.h"

#include "radio_lvgl_compat.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
/* sdgoods_lcd.h: not needed on this platform */
#include "sdgoods_cc.h"

/* ------------------------------------------------------------------ 主题 */
static rk_theme_t s_theme;
static bool       s_night;
static bool       s_theme_ready;

static void theme_build(void)
{
    if (s_night) {
        s_theme = (rk_theme_t){
            .bg        = lv_color_hex(0x0C0C0D),
            .ink       = lv_color_hex(0xF4F1E9),
            .muted     = lv_color_hex(0x8B8B85),
            .hair      = lv_color_hex(0x2A2A28),
            .accent    = lv_color_hex(0xFF4436),
            .card      = lv_color_hex(0x17171A),
            .on_accent = lv_color_hex(0x0C0C0D),
        };
    } else {
        s_theme = (rk_theme_t){
            .bg        = lv_color_hex(0xF4F1E8),
            .ink       = lv_color_hex(0x14140F),
            .muted     = lv_color_hex(0x8A877C),
            .hair      = lv_color_hex(0xD8D3C4),
            .accent    = lv_color_hex(0xD8362A),
            .card      = lv_color_hex(0xEAE5D8),
            .on_accent = lv_color_hex(0xFFFDF6),
        };
    }
    s_theme_ready = true;
}

const rk_theme_t *rk_theme(void)
{
    if (!s_theme_ready) {
        theme_build();
    }
    return &s_theme;
}

void rk_theme_set(bool night)
{
    s_night = night;
    theme_build();
}

bool rk_theme_is_night(void)
{
    return s_night;
}

void rk_theme_apply(const rk_theme_t *palette)
{
    if (!palette) return;
    s_theme = *palette;
    s_theme_ready = true;
    s_night = lv_color_brightness(palette->bg) < 128;
}

/* ------------------------------------------------------------------ UTF-8 */
int rk_utf8_next(const char *s, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!p || p[0] == 0) {
        if (cp) *cp = 0;
        return 0;
    }
    uint32_t c = p[0];
    int len = 1;
    if (c < 0x80) {
        /* ascii */
    } else if ((c & 0xE0) == 0xC0) {
        c &= 0x1F;
        len = 2;
    } else if ((c & 0xF0) == 0xE0) {
        c &= 0x0F;
        len = 3;
    } else if ((c & 0xF8) == 0xF0) {
        c &= 0x07;
        len = 4;
    } else {
        if (cp) *cp = 0xFFFD;
        return 1;
    }
    for (int i = 1; i < len; i++) {
        if ((p[i] & 0xC0) != 0x80) {
            if (cp) *cp = 0xFFFD;
            return i;
        }
        c = (c << 6) | (p[i] & 0x3F);
    }
    if (cp) *cp = c;
    return len;
}

static size_t utf8_put(char *dst, size_t cap, size_t o, uint32_t cp)
{
    if (cp < 0x80) {
        if (o + 1 < cap) dst[o++] = (char)cp;
    } else if (cp < 0x800) {
        if (o + 2 < cap) {
            dst[o++] = (char)(0xC0 | (cp >> 6));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        }
    } else if (cp < 0x10000) {
        if (o + 3 < cap) {
            dst[o++] = (char)(0xE0 | (cp >> 12));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        }
    } else {
        if (o + 4 < cap) {
            dst[o++] = (char)(0xF0 | (cp >> 18));
            dst[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            dst[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    return o;
}

void rk_text_safe(const char *src, char *dst, size_t cap, const lv_font_t *font)
{
    if (!dst || cap == 0) {
        return;
    }
    size_t o = 0;
    if (src) {
        const char *p = src;
        while (*p && o + 6 < cap) {
            uint32_t cp = 0;
            int n = rk_utf8_next(p, &cp);
            if (n <= 0) {
                break;
            }
            p += n;
            if (cp == '\n' || cp == '\r' || cp == '\t') {
                if (o > 0 && dst[o - 1] != ' ') {
                    dst[o++] = ' ';
                }
                continue;
            }
            if (cp < 0x20) {
                continue;
            }
            if (font) {
                lv_font_glyph_dsc_t gd;
                if (!lv_font_get_glyph_dsc(font, &gd, cp, 0)) {
                    /* 字体外的字符直接跳过：GB2312 字体已覆盖常用汉字，
                     * 极少数生僻字/特殊符号宁可不显示，也不要用问号代替。 */
                    continue;
                }
            }
            o = utf8_put(dst, cap, o, cp);
        }
        while (o > 0 && dst[o - 1] == ' ') {
            o--;
        }
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------------ 5×7 点阵 */
/* 列优先，bit0 = 最上行。索引由 GLYPH_MAP 决定。 */
static const uint8_t GLYPHS[][5] = {
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, /* 0 */
    {0x00, 0x42, 0x7F, 0x40, 0x00}, /* 1 */
    {0x42, 0x61, 0x51, 0x49, 0x46}, /* 2 */
    {0x21, 0x41, 0x45, 0x4B, 0x31}, /* 3 */
    {0x18, 0x14, 0x12, 0x7F, 0x10}, /* 4 */
    {0x27, 0x45, 0x45, 0x45, 0x39}, /* 5 */
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, /* 6 */
    {0x01, 0x71, 0x09, 0x05, 0x03}, /* 7 */
    {0x36, 0x49, 0x49, 0x49, 0x36}, /* 8 */
    {0x06, 0x49, 0x49, 0x29, 0x1E}, /* 9 */
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x49, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x09, 0x01}, /* F */
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x07, 0x08, 0x70, 0x08, 0x07}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
    {0x00, 0x00, 0x40, 0x00, 0x00}, /* . */
    {0x08, 0x08, 0x08, 0x08, 0x08}, /* - */
    {0x00, 0x36, 0x36, 0x00, 0x00}, /* : */
    {0x00, 0x00, 0x00, 0x00, 0x00}, /* space */
    {0x20, 0x10, 0x08, 0x04, 0x02}, /* / */
    {0x08, 0x08, 0x3E, 0x08, 0x08}, /* + */
};

static const char GLYPH_MAP[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.-: /+";

static const uint8_t *glyph_of(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    const char *hit = strchr(GLYPH_MAP, c);
    if (!hit) {
        return NULL;
    }
    return GLYPHS[(int)(hit - GLYPH_MAP)];
}

static void dot_draw_cb(lv_event_t *e)
{
    rk_dot_t *d = (rk_dot_t *)lv_event_get_user_data(e);
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    if (!d || !dc || d->dot == 0) {
        return;
    }
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t co;
    lv_obj_get_coords(obj, &co);

    int n = (int)strlen(d->text);
    if (n > 6) {
        n = 6;
    }
    int gh = 7 * d->dot + 6 * d->gap;          /* 字高 */
    int total = 0;
    for (int i = 0; i < n; ++i) {
        int cols = d->text[i] == '.' ? 1 : 5;
        total += cols * d->dot + (cols - 1) * d->gap + (i ? d->gap * 2 : 0);
    }
    int x0 = co.x1 + (lv_area_get_width(&co) - total) / 2;
    int y0 = co.y1 + (lv_area_get_height(&co) - gh) / 2;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = d->color;
    rd.bg_opa = LV_OPA_COVER;
    rd.border_width = 0;
    rd.shadow_width = 0;
    rd.radius = LV_RADIUS_CIRCLE;

    int gx = x0;
    for (int i = 0; i < n; i++) {
        const uint8_t *g = glyph_of(d->text[i]);
        if (!g) {
            continue;
        }
        int cols = d->text[i] == '.' ? 1 : 5;
        for (int col = 0; col < cols; col++) {
            int source_col = cols == 1 ? 2 : col;
            for (int row = 0; row < 7; row++) {
                if (!(g[source_col] & (1 << row))) {
                    continue;
                }
                lv_area_t a;
                a.x1 = gx + col * (d->dot + d->gap);
                a.y1 = y0 + row * (d->dot + d->gap);
                a.x2 = a.x1 + d->dot - 1;
                a.y2 = a.y1 + d->dot - 1;
                lv_draw_rect(dc, &rd, &a);
            }
        }
        gx += cols * d->dot + (cols - 1) * d->gap + d->gap * 2;
    }
}

void rk_dot_create(rk_dot_t *d, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                   lv_coord_t w, lv_coord_t h, uint8_t dot, uint8_t gap)
{
    if (!d || !parent) {
        return;
    }
    memset(d, 0, sizeof(*d));
    d->dot = dot;
    d->gap = gap;
    d->color = rk_theme()->ink;
    d->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(d->obj);
    lv_obj_set_pos(d->obj, x, y);
    lv_obj_set_size(d->obj, w, h);
    lv_obj_clear_flag(d->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(d->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(d->obj, dot_draw_cb, LV_EVENT_DRAW_MAIN, d);
}

void rk_dot_set_text(rk_dot_t *d, const char *text)
{
    if (!d || !d->obj) {
        return;
    }
    char buf[sizeof(d->text)];
    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    if (strcmp(buf, d->text) == 0) {
        return;
    }
    strncpy(d->text, buf, sizeof(d->text) - 1);
    d->text[sizeof(d->text) - 1] = '\0';
    lv_obj_invalidate(d->obj);
}

void rk_dot_set_color(rk_dot_t *d, lv_color_t color)
{
    if (!d || !d->obj) {
        return;
    }
    d->color = color;
    lv_obj_invalidate(d->obj);
}

/* -------------------------------------------------------------- 表盘环 */
#define RING_A0     120     /* 起点：左下 120°（LVGL：0°在 3 点钟、顺时针） */
#define RING_SWEEP  300     /* 顺时针 300°，底部留 60° 缺口给 dock */

static uint16_t ring_angle(int idx, int count)
{
    if (idx < 0) {
        return RING_A0;
    }
    if (count <= 1) {
        return RING_A0 + RING_SWEEP / 2;
    }
    if (idx >= count) {
        idx = count - 1;
    }
    return (uint16_t)(RING_A0 + (RING_SWEEP * idx) / (count - 1));
}

static void ring_point(lv_point_t center, lv_coord_t r, uint16_t angle, lv_point_t *out)
{
    int32_t s = lv_trigo_sin((int16_t)(angle % 360));
    int32_t c = lv_trigo_sin((int16_t)((angle + 90) % 360));
    out->x = center.x + (lv_coord_t)((r * c) >> LV_TRIGO_SHIFT);
    out->y = center.y + (lv_coord_t)((r * s) >> LV_TRIGO_SHIFT);
}

static void ring_draw_cb(lv_event_t *e)
{
    rk_ring_t *r = (rk_ring_t *)lv_event_get_user_data(e);
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    if (!r || !dc || r->radius <= 0) {
        return;
    }
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t co;
    lv_obj_get_coords(obj, &co);
    lv_point_t center = {co.x1 + r->size / 2, co.y1 + r->size / 2};

    const rk_theme_t *th = rk_theme();
    lv_draw_arc_dsc_t ad;
    lv_draw_arc_dsc_init(&ad);
    ad.rounded = 1;
    ad.opa = LV_OPA_COVER;

    /* 轨道：整段 120°→420°，2px 细线。
     * 曾经是 3px 轨道 + 4px 进度（从起点一路染红到当前），实测红弧会盖住 77%
     * 的周长，整屏被一圈亮红抢走焦点，点阵反而变弱。现在红色只留给端点那一颗
     * 针：已走 muted、未走 hair，靠两种灰的深浅分进度。
     * 轨道取 hair↔muted 中间：纯 hair 在暗底上对比只有 1.6:1，实拍几乎看不见，
     * 环就退化成一颗浮在空中的红点，丢了「26 个台围成一圈」的语义。 */
    ad.color = lv_color_mix(th->muted, th->hair, 110);
    ad.width = 2;
    lv_draw_arc(dc, &ad, &center, (uint16_t)r->radius, RING_A0, RING_A0 + RING_SWEEP);

    if (r->idx < 0 || r->count <= 0) {
        return;
    }

    uint16_t end = ring_angle(r->idx, r->count);
    ad.color = r->dim ? th->hair : th->muted;
    ad.width = 2;
    if (end > RING_A0) {
        lv_draw_arc(dc, &ad, &center, (uint16_t)r->radius, RING_A0, end);
    }

    /* 端点：一颗实心圆，整屏上唯一的红 */
    lv_point_t p;
    ring_point(center, r->radius, end, &p);
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = r->dim ? th->muted : th->accent;
    rd.bg_opa = LV_OPA_COVER;
    rd.border_width = 0;
    rd.shadow_width = 0;
    rd.radius = LV_RADIUS_CIRCLE;
    lv_area_t da = {p.x - 5, p.y - 5, p.x + 5, p.y + 5};
    lv_draw_rect(dc, &rd, &da);

    /* 起点小帽：让弧的起始有个「刻度原点」，比轨道亮一档但不抢端点 */
    ring_point(center, r->radius, RING_A0, &p);
    lv_area_t sa = {p.x - 2, p.y - 2, p.x + 2, p.y + 2};
    rd.radius = LV_RADIUS_CIRCLE;
    rd.bg_color = r->dim ? th->hair : th->muted;
    lv_draw_rect(dc, &rd, &sa);
}

void rk_ring_create(rk_ring_t *r, lv_obj_t *parent, lv_coord_t size, lv_coord_t radius)
{
    if (!r || !parent) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->size = size;
    r->radius = radius;
    r->idx = -1;
    r->obj = lv_obj_create(parent);
    lv_obj_remove_style_all(r->obj);
    lv_obj_set_pos(r->obj, (LCD_WIDTH - size) / 2, (LCD_HEIGHT - size) / 2);
    lv_obj_set_size(r->obj, size, size);
    lv_obj_clear_flag(r->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->obj, ring_draw_cb, LV_EVENT_DRAW_MAIN, r);
}

void rk_ring_set(rk_ring_t *r, int idx, int count)
{
    if (!r || !r->obj) {
        return;
    }
    if (r->idx == idx && r->count == count) {
        return;
    }
    r->idx = idx;
    r->count = count;
    lv_obj_invalidate(r->obj);
}

void rk_ring_set_dim(rk_ring_t *r, bool dim)
{
    if (!r || !r->obj || r->dim == dim) {
        return;
    }
    r->dim = dim;
    lv_obj_invalidate(r->obj);
}

/* ---------------------------------------------------------------- 图标 */
static void icon_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    if (!dc) {
        return;
    }
    rk_icon_t kind = (rk_icon_t)(uintptr_t)lv_obj_get_user_data(obj);
    lv_color_t col = lv_obj_get_style_bg_color(obj, 0);

    lv_area_t co;
    lv_obj_get_coords(obj, &co);
    lv_coord_t w = lv_area_get_width(&co);
    lv_coord_t h = lv_area_get_height(&co);
    lv_coord_t s = LV_MIN(w, h);
    lv_coord_t cx = co.x1 + w / 2;
    lv_coord_t cy = co.y1 + h / 2;

    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = col;
    rd.bg_opa = LV_OPA_COVER;
    rd.border_width = 0;
    rd.shadow_width = 0;
    rd.radius = 0;

    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = col;
    ld.width = 2;
    ld.opa = LV_OPA_COVER;
    ld.round_start = 1;
    ld.round_end = 1;

    switch (kind) {
    case RK_ICON_PLAY: {
        lv_point_t p[3] = {
            {cx - s * 26 / 100, cy - s * 34 / 100},
            {cx + s * 36 / 100, cy},
            {cx - s * 26 / 100, cy + s * 34 / 100},
        };
        lv_draw_triangle(dc, &rd, p);
        break;
    }
    case RK_ICON_PAUSE: {
        rd.radius = 2;
        lv_area_t a1 = {cx - s * 22 / 100, cy - s * 32 / 100, cx - s * 6 / 100, cy + s * 32 / 100};
        lv_area_t a2 = {cx + s * 6 / 100, cy - s * 32 / 100, cx + s * 22 / 100, cy + s * 32 / 100};
        lv_draw_rect(dc, &rd, &a1);
        lv_draw_rect(dc, &rd, &a2);
        break;
    }
    case RK_ICON_PREV:
    case RK_ICON_NEXT: {
        bool prev = (kind == RK_ICON_PREV);
        lv_coord_t tx = prev ? (cx + s * 20 / 100) : (cx - s * 20 / 100);
        lv_point_t p[3] = {
            {tx, cy - s * 32 / 100},
            {tx, cy + s * 32 / 100},
            {prev ? (cx - s * 26 / 100) : (cx + s * 26 / 100), cy},
        };
        lv_draw_triangle(dc, &rd, p);
        rd.radius = 1;
        lv_area_t bar = prev
            ? (lv_area_t){cx - s * 34 / 100, cy - s * 30 / 100, cx - s * 26 / 100, cy + s * 30 / 100}
            : (lv_area_t){cx + s * 26 / 100, cy - s * 30 / 100, cx + s * 34 / 100, cy + s * 30 / 100};
        lv_draw_rect(dc, &rd, &bar);
        break;
    }
    case RK_ICON_CLOSE: {
        lv_point_t a = {cx - s * 20 / 100, cy - s * 20 / 100};
        lv_point_t b = {cx + s * 20 / 100, cy + s * 20 / 100};
        lv_point_t c1 = {cx + s * 20 / 100, cy - s * 20 / 100};
        lv_point_t d1 = {cx - s * 20 / 100, cy + s * 20 / 100};
        lv_draw_line(dc, &ld, &a, &b);
        lv_draw_line(dc, &ld, &c1, &d1);
        break;
    }
    case RK_ICON_BACK:
    case RK_ICON_CHEVRON: {
        int sign=kind==RK_ICON_BACK?-1:1;
        lv_point_t a = {cx - sign*s * 8 / 100, cy - s * 24 / 100};
        lv_point_t b = {cx + sign*s * 14 / 100, cy};
        lv_point_t c1 = {cx + sign*s * 14 / 100, cy};
        lv_point_t d1 = {cx - sign*s * 8 / 100, cy + s * 24 / 100};
        lv_draw_line(dc, &ld, &a, &b);
        lv_draw_line(dc, &ld, &c1, &d1);
        break;
    }
    case RK_ICON_LIST:
    default: {
        rd.radius = LV_RADIUS_CIRCLE;
        for (int i = 0; i < 3; i++) {
            lv_coord_t yy = cy - s * 22 / 100 + i * s * 22 / 100;
            lv_area_t dot = {cx - s * 30 / 100, yy - 1, cx - s * 28 / 100, yy + 1};
            lv_draw_rect(dc, &rd, &dot);
            lv_point_t a = {cx - s * 18 / 100, yy};
            lv_point_t b = {cx + s * 30 / 100, yy};
            lv_draw_line(dc, &ld, &a, &b);
        }
        break;
    }
    }
}

lv_obj_t *rk_icon(lv_obj_t *parent, lv_coord_t size, rk_icon_t kind, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_user_data(o, (void *)(uintptr_t)kind);
    lv_obj_add_event_cb(o, icon_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

lv_obj_t *rk_round_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t size,
                       rk_icon_t kind, lv_color_t bg, lv_color_t fg)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, size, size);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    /* 按下反馈：整块（含图标）压到 60% 不透明。
     * 原来只改 bg_opa，图标仍全亮 —— 反色的播放键按下去只是「底色淡一点」，
     * 实拍几乎看不出这一下有没有被接住，手感就成了「点了没反应」。
     * 改 obj 级别 opa 后，底色和图标一起下沉，一帧内就能看到回应。 */
    lv_obj_set_style_opa(btn, LV_OPA_60, LV_STATE_PRESSED);

    lv_obj_t *ic = rk_icon(btn, size, kind, fg);
    lv_obj_center(ic);
    return btn;
}

/* ------------------------------------------------------------ 基础构件 */
lv_obj_t *rk_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

void rk_hairline(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_color_t c)
{
    lv_obj_t *o = rk_box(parent, x, y, w, 1);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

lv_obj_t *rk_pill(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                  const char *text, lv_color_t bg, lv_color_t fg)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(p, h / 2, 0);
    lv_obj_set_style_bg_color(p, bg, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_70, LV_STATE_PRESSED);

    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return p;
}

void rk_pill_set_text(lv_obj_t *pill, const char *text)
{
    if (!pill) {
        return;
    }
    lv_obj_t *l = lv_obj_get_child(pill, 0);
    if (l) {
        lv_label_set_text(l, text ? text : "");
        lv_obj_center(l);
    }
}

static void fade_exec(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

void rk_fade_in(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 180);
    lv_anim_set_exec_cb(&a, fade_exec);
    lv_anim_start(&a);
}

/* ------------------------------------------------------------ 后台任务 */
bool rk_task_spawn(void (*fn)(void *), const char *name, uint32_t stack, void *arg, unsigned prio)
{
    return xTaskCreateWithCaps((TaskFunction_t)fn, name, stack, arg, (UBaseType_t)prio, NULL,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
}

void rk_task_exit(void)
{
    vTaskDeleteWithCaps(NULL);
}

/* ------------------------------------------------------------ 页面骨架 */
static void page_close_async(void *ud)
{
    rk_page_t *p=ud;
    if(p->closing)rk_page_close(p);
}

static void page_close_cb(void *ud)
{
    rk_page_t *p=ud;
    if(p->back_cb){p->back_cb(p->back_ud);return;}
    if(!p->closing){p->closing=true;lv_async_call(page_close_async,p);}
}

bool rk_page_is_open(const rk_page_t *p)
{
    return p && p->root != NULL;
}

void rk_page_open(rk_page_t *p, lv_obj_t *parent, const char *title)
{
    if (!p || !parent) {
        return;
    }
    if(p->root)rk_page_close(p);
    memset(p, 0, sizeof(*p));
    const rk_theme_t *th = rk_theme();

    p->root = lv_obj_create(parent);
    lv_obj_remove_style_all(p->root);
    lv_obj_set_size(p->root, LCD_WIDTH, LCD_HEIGHT);
    lv_obj_set_pos(p->root, 0, 0);
    lv_obj_set_style_bg_color(p->root, th->bg, 0);
    lv_obj_set_style_bg_opa(p->root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(p->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p->root, LV_OBJ_FLAG_CLICKABLE);   /* 吞掉落空触摸 */

    p->title = lv_label_create(p->root);
    lv_label_set_text(p->title, title ? title : "");
    lv_obj_set_style_text_font(p->title, RK_FONT_BODY, 0);
    lv_obj_set_style_text_color(p->title, th->ink, 0);
    lv_obj_set_pos(p->title, 108, 86);
    lv_obj_set_width(p->title, 136);
    lv_obj_set_style_text_align(p->title,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_long_mode(p->title, LV_LABEL_LONG_DOT);
    lv_obj_clear_flag(p->title, LV_OBJ_FLAG_CLICKABLE);

    /* Keep the header below the platform capture strip (y < 70). */
    lv_obj_t *close = rk_round_btn(p->root, 60, 74, 40, RK_ICON_BACK,
                                   lv_color_mix(th->muted, th->bg, 70), th->muted);
    sdgoods_tap_bind(close, &p->close_ctx, page_close_cb, p);
    p->back=close;

    rk_hairline(p->root, 60, 118, 240, th->hair);

    p->body = lv_obj_create(p->root);
    lv_obj_remove_style_all(p->body);
    lv_obj_set_pos(p->body, 60, 124);
    /* 176 只能露 3.6 行（48px/行）——设置页第 5 项要滚动才能看到，而圆屏上
     * 根本没有「这页可以滚」的提示。放到 200：4 整行 + 第 5 行露出小半截，
     * 下半圆的弦宽在 y=300 处仍有 134px，内容不会被圆角切掉。 */
    lv_obj_set_size(p->body, 240, 180);
    lv_obj_set_flex_flow(p->body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(p->body, 0, 0);
    lv_obj_set_style_pad_row(p->body, 0, 0);
    lv_obj_set_scrollbar_mode(p->body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(p->body, LV_DIR_VER);
    lv_obj_add_flag(p->body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_move_foreground(p->root);
    sdgoods_cc_bind(p->root);
}

void rk_page_close(rk_page_t *p)
{
    if (!p || !p->root) {
        return;
    }
    lv_obj_del(p->root);
    p->root = NULL;
    p->body = NULL;
    p->title = NULL;
    p->back = NULL;
    p->rows = 0;
    p->closing=false;
    p->back_cb=NULL;
    p->back_ud=NULL;
}

void rk_page_action(rk_page_t *p, const char *text, sdgoods_tap_ctx_t *ctx,
                    sdgoods_tap_cb_t cb, void *ud)
{
    if (!p || !p->root) {
        return;
    }
    const rk_theme_t *th = rk_theme();
    lv_obj_t *b = lv_obj_create(p->root);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, 244, 74);
    lv_obj_set_size(b, 56, 40);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(b, 13, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, th->card, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, th->accent, 0);
    lv_obj_center(l);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);

    sdgoods_tap_bind(b, ctx, cb, ud);
}

static void page_row_impl(rk_page_t *p, const char *label, const char *value,
                          sdgoods_tap_ctx_t *ctx, sdgoods_tap_cb_t cb, void *ud,
                          const lv_font_t *font, lv_coord_t label_w, lv_coord_t label_y,
                          lv_coord_t row_h);

void rk_page_row(rk_page_t *p, const char *label, const char *value,
                 sdgoods_tap_ctx_t *ctx, sdgoods_tap_cb_t cb, void *ud)
{
    page_row_impl(p, label, value, ctx, cb, ud, RK_FONT_BODY, 110, 14, 48);
}

void rk_page_row_net(rk_page_t *p, const char *label, const char *value,
                     sdgoods_tap_ctx_t *ctx, sdgoods_tap_cb_t cb, void *ud)
{
    if (!p || !p->body) return;
    const rk_theme_t *th = rk_theme();
    lv_obj_t *row = lv_obj_create(p->body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 48);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_color(row, th->card, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

    /* Full station name above metadata; separate bands cannot overlap. */
    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_style_text_font(name, RK_FONT_TITLE, 0);
    lv_obj_set_style_text_color(name, th->ink, 0);
    lv_obj_set_size(name, 204, 26);
    lv_obj_set_pos(name, 10, 1);
    /* 长台名用省略号，不做循环滚动：滚动会让整行每帧重绘，
     * 一页 3 行滚动动画能把触摸响应拖到几百毫秒。 */
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, label ? label : "");
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *meta = lv_label_create(row);
    lv_obj_set_style_text_font(meta, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(meta, th->muted, 0);
    lv_obj_set_size(meta, 204, 18);
    lv_obj_set_pos(meta, 10, 28);
    lv_label_set_long_mode(meta, LV_LABEL_LONG_DOT);
    lv_label_set_text(meta, value ? value : "");
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *arrow = rk_icon(row, 16, RK_ICON_CHEVRON, th->muted);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -6, 0);
    rk_hairline(row, 10, 47, 220, th->hair);
    sdgoods_tap_bind(row, ctx, cb, ud);
    p->rows++;
}

void rk_page_row_compact(rk_page_t *p, const char *label, const char *value,
                         sdgoods_tap_ctx_t *ctx, sdgoods_tap_cb_t cb, void *ud)
{
    /* 设置页专用：5 行 × 40px = 200px 正好塞进 body，不需要滚动。
     * 圆屏上「能滚」是不可见的，把内容藏到折行外就等于用户找不到。 */
    page_row_impl(p, label, value, ctx, cb, ud, RK_FONT_BODY, 100, 8, 36);
}

static void page_row_impl(rk_page_t *p, const char *label, const char *value,
                          sdgoods_tap_ctx_t *ctx, sdgoods_tap_cb_t cb, void *ud,
                          const lv_font_t *font, lv_coord_t label_w, lv_coord_t label_y,
                          lv_coord_t row_h)
{
    if (!p || !p->body) {
        return;
    }
    const rk_theme_t *th = rk_theme();
    bool has_val = value && value[0];

    lv_obj_t *row = lv_obj_create(p->body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), row_h);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, th->card, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);

    lv_obj_t *l = lv_label_create(row);
    /* 先定尺寸 + LONG_DOT 再设文本：反过来的话 LVGL 已按默认 WRAP 排好行，
     * 长台名会折成两行、顶出 48px 行高 —— 列表里就是这么糊的。
     * 而 LV_LABEL_LONG_DOT 的截断条件是「换行后仍超出**高度**」，
     * 所以高度必须锁在一行（否则它只会折行，不会变省略号）。 */
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, th->ink, 0);
    lv_obj_set_size(l, has_val ? label_w : 186, 26);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_label_set_text(l, label ? label : "");
    lv_obj_set_pos(l, 10, label_y);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);

    if (has_val) {
        lv_obj_t *v = lv_label_create(row);
        lv_label_set_text(v, value);
        lv_obj_set_style_text_font(v, RK_FONT_SMALL, 0);
        lv_obj_set_style_text_color(v, th->muted, 0);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(v, 96);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, -24, 0);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *ch = rk_icon(row, 16, RK_ICON_CHEVRON, th->muted);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -6, 0);

    rk_hairline(row, 10, row_h - 1, 236, th->hair);

    sdgoods_tap_bind(row, ctx, cb, ud);
    p->rows++;
}

void rk_page_hint(rk_page_t *p, const char *text)
{
    if (!p || !p->body) {
        return;
    }
    lv_obj_t *l = lv_label_create(p->body);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, RK_FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, rk_theme()->muted, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_pad_top(l, 40, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
}
