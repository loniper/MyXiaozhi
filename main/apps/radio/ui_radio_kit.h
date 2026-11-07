/*
 * sdgoods-radio · UI 工具箱（应用层）
 *
 * 设计系统落地：主题 token、5×7 点阵大字、表盘环、矢量图标、胶囊、页面骨架。
 * 全部自绘（LV_EVENT_DRAW_MAIN + lv_draw_rect/arc/line），不依赖 canvas 缓冲，
 * 也不依赖任何图标字体 —— 网络台名缺字、图标字体缺字形的问题从根上没有了。
 *
 * 约束：
 *   · 所有可交互对象仍必须走 sdgoods_tap_bind()（位移守卫 + PRESS_LOCK）；
 *   · 自绘对象一律清掉 CLICKABLE，触摸穿透给下层；
 *   · 配色只在绘制时读 rk_theme() ⇒ 切主题必须重建界面（见 ui_radio.c）。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"
#include "sdgoods_tap.h"

/* 自带的 GB2312 全量 16px 字体（4bpp，见 radio_font_16.c）：覆盖 6763 个
 * 常用汉字与全角标点，网络台名不会再出现缺字问号。 */
LV_FONT_DECLARE(radio_font_16);

#define RK_FONT_SMALL  (&radio_font_16)
#define RK_FONT_BODY   (&radio_font_16)
#define RK_FONT_TITLE  (&radio_font_16)

/* ------------------------------------------------------------------ 主题 */
typedef struct {
    lv_color_t bg;
    lv_color_t ink;
    lv_color_t muted;
    lv_color_t hair;
    lv_color_t accent;
    lv_color_t card;
    lv_color_t on_accent;
} rk_theme_t;

void              rk_theme_set(bool night);
void              rk_theme_apply(const rk_theme_t *palette);
bool              rk_theme_is_night(void);
const rk_theme_t *rk_theme(void);

/* ------------------------------------------------------------------ 文本 */
/* 逐码点校验字形（会自动查 fallback），缺字形替换为 '?'，去控制符。
 * 所有来自网络的台名都必须先过这一层，否则会出现方框。 */
void rk_text_safe(const char *src, char *dst, size_t cap, const lv_font_t *font);

/* 以 UTF-8 追加一个码点（内部用，导出便于测试） */
int  rk_utf8_next(const char *s, uint32_t *cp);

/* ------------------------------------------------------------ 点阵大字 */
typedef struct {
    lv_obj_t *obj;
    char      text[12];
    uint8_t   dot;          /* 点边长 px */
    uint8_t   gap;          /* 点间距 px */
    lv_color_t color;
} rk_dot_t;

/* 在 parent 上建一个 w×h 的点阵显示区（居中绘制），dot 建议 6、gap 4。 */
void rk_dot_create(rk_dot_t *d, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                   lv_coord_t w, lv_coord_t h, uint8_t dot, uint8_t gap);
void rk_dot_set_text(rk_dot_t *d, const char *text);
void rk_dot_set_color(rk_dot_t *d, lv_color_t color);

/* -------------------------------------------------------------- 表盘环 */
typedef struct {
    lv_obj_t *obj;
    int       idx;          /* -1 = 不画进度 */
    int       count;
    bool      dim;          /* true：进度弧用 muted（暂停/异常） */
    lv_coord_t size;
    lv_coord_t radius;
} rk_ring_t;

/* 全屏覆盖（size×size，放 (0,0)），底部留 60° 缺口。 */
void rk_ring_create(rk_ring_t *r, lv_obj_t *parent, lv_coord_t size, lv_coord_t radius);
void rk_ring_set(rk_ring_t *r, int idx, int count);
void rk_ring_set_dim(rk_ring_t *r, bool dim);

/* ---------------------------------------------------------------- 图标 */
typedef enum {
    RK_ICON_PLAY = 0,
    RK_ICON_PAUSE,
    RK_ICON_PREV,
    RK_ICON_NEXT,
    RK_ICON_CLOSE,
    RK_ICON_CHEVRON,
    RK_ICON_BACK,
    RK_ICON_LIST,
} rk_icon_t;

/* 纯矢量图标（三角形/圆角矩形/线），color 为图标颜色。 */
lv_obj_t *rk_icon(lv_obj_t *parent, lv_coord_t size, rk_icon_t kind, lv_color_t color);

/* 圆形 dock 按钮：返回按钮对象，内部已放好图标。 */
lv_obj_t *rk_round_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t size,
                       rk_icon_t kind, lv_color_t bg, lv_color_t fg);

/* ------------------------------------------------------------ 基础构件 */
/* 胶囊（可点，由调用方 sdgoods_tap_bind 绑定动作）。 */
lv_obj_t *rk_pill(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                  const char *text, lv_color_t bg, lv_color_t fg);
void      rk_pill_set_text(lv_obj_t *pill, const char *text);

/* 1px 分隔线 */
void rk_hairline(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_color_t c);

/* 无边框透明容器（统一清样式，省得到处重复写） */
lv_obj_t *rk_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h);

/* 透明度淡入 + 上移归位（只重绘自身小区域） */
void rk_fade_in(lv_obj_t *obj);

/* ------------------------------------------------------------ 页面骨架 */
typedef struct {
    lv_obj_t *root;
    lv_obj_t *body;
    lv_obj_t *title;
    lv_obj_t *back;
    sdgoods_tap_cb_t back_cb;
    void *back_ud;
    bool closing;
    int       rows;
    sdgoods_tap_ctx_t close_ctx;    /* 页头关闭按钮的位移守卫上下文（调用方静态存储） */
    sdgoods_tap_ctx_t action_ctx;
} rk_page_t;

/* 打开一个全屏页面：纸底 + 页头（标题/关闭）+ 可滚动 body。 */
void rk_page_open(rk_page_t *p, lv_obj_t *parent, const char *title);
void rk_page_close(rk_page_t *p);
bool rk_page_is_open(const rk_page_t *p);

/* 页头右侧文字按钮（例：设置页的「设置」入口）。*/
void rk_page_action(rk_page_t *p, const char *text, sdgoods_tap_ctx_t *ctx,
                    sdgoods_tap_cb_t cb, void *ud);

/* 列表行：label 主行，value 右侧次行（可 NULL），整行可点。 */
void rk_page_row(rk_page_t *p, const char *label, const char *value,
                 sdgoods_tap_ctx_t *ctx, sdgoods_tap_cb_t cb, void *ud);

/* 同上，但主行用网络字体（net_font_20）：台名这类运行时字符串必须用它，
 * 否则缺字会被 rk_text_safe 换成 '?'。 */
void rk_page_row_net(rk_page_t *p, const char *label, const char *value,
                     sdgoods_tap_ctx_t *ctx, sdgoods_tap_cb_t cb, void *ud);

/* 紧凑行（40px）：设置页用，5 行正好落在可视区内不靠滚动。 */
void rk_page_row_compact(rk_page_t *p, const char *label, const char *value,
                         sdgoods_tap_ctx_t *ctx, sdgoods_tap_cb_t cb, void *ud);

/* 空态提示（居中一行 muted 文本）。 */
void rk_page_hint(rk_page_t *p, const char *text);

/* ------------------------------------------------------------ 后台任务 */
/* ★ 为什么要有这层：Wi-Fi/BT/LVGL 已经把内部 RAM 吃干，xTaskCreate 拿 8KB 栈
 * 会静默返回 pdFAIL（实测 heap_caps 内部最大空闲块只剩 ~7KB），任务根本不跑。
 * 这里统一把栈放到 PSRAM（需 CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY）。
 * ⚠️ 用本函数起的任务，退出时必须调 rk_task_exit()（不可直接 vTaskDelete）。 */
bool rk_task_spawn(void (*fn)(void *), const char *name, uint32_t stack, void *arg, unsigned prio);
void rk_task_exit(void);
