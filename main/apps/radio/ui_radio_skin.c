/*
 * 谷仓共创计划 · 谷仓 SDGOODS 开放平台基础工程
 * 应用层示例
 * https://github.com/SDGOODS/SDGOODS-ESP32S3
 *
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * 「谷仓共创计划」与「谷仓 SDGOODS 开放平台」项目、谷仓次元屏（谷仓电子徽章）设备，
 *   以及本基础代码的著作权与相关权利，均归深圳希德创新网络有限公司所有。
 * SPDX-License-Identifier: Apache-2.0
 *
 * 本文件属于应用层，以 Apache-2.0 发布：可自由商用、可闭源分发，
 * 只需保留本声明并携带 NOTICE 文件。详见 LICENSING.md。
 */

/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 深圳希德创新网络有限公司 (SDGOODS)
 * Shared live UI over compact AI-generated art; no full-screen bitmap buffers.
 */
#include "ui_radio_skin.h"

#include "radio_lvgl_compat.h"
#include <string.h>
#include <stdio.h>
#include "sdgoods_i18n.h"
#include "ui_radio_assets.h"
#include "ui_radio_redraw_assets.h"
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "radio_station_identity.h"
#include "radio_catalog.h"

static const radio_skin_t s_skins[] = {
    {"dot-matrix", "点阵电波", "Dot Matrix", 0x101112, 0xF1F0E8, 0xB0B2AA, 0xD44A34, 0x252829, RADIO_LAYOUT_DOTS, NULL},
};
_Static_assert(sizeof(s_skins)/sizeof(s_skins[0])==RADIO_SKIN_COUNT, "skin registry count mismatch");

/* A scaled title font reuses existing glyphs instead of embedding
 * another full Chinese font. One small scratch glyph, UI-thread access only. */
/* 标题字体：与原项目同一套（Noto Sans SC Bold），24/28/32px 三档，
 * 分别用于台名、波形皮肤标题、纸感皮肤大标题；不再是 16px 拉伸。 */
LV_FONT_DECLARE(radio_title_24);
LV_FONT_DECLARE(radio_title_28);
LV_FONT_DECLARE(radio_title_32);
#define s_medium_title_font radio_title_32
#define s_heading_font radio_title_28
#define s_station_font radio_title_24

const radio_skin_t *radio_skin_get(int index)
{
    if (index < 0 || index >= RADIO_SKIN_COUNT) index = RADIO_SKIN_DEFAULT;
    return &s_skins[index];
}

int radio_skin_find(const char *id)
{
    if(id && !strcmp(id,"editorial"))id="paper-radio";
    if(id && !strcmp(id,"vinyl-club"))id="vinyl-pixel";
    if (id) for (int i = 0; i < RADIO_SKIN_COUNT; ++i)
        if (strcmp(id, s_skins[i].id) == 0) return i;
    return RADIO_SKIN_DEFAULT;
}

const char *radio_skin_name(int index)
{
    const radio_skin_t *s = radio_skin_get(index);
    return SDG_T(s->zh, s->en);
}

void radio_skin_apply_palette(int index)
{
    const radio_skin_t *s = radio_skin_get(index);
    const rk_theme_t palette = {
        .bg = lv_color_hex(s->bg), .ink = lv_color_hex(s->ink),
        .muted = lv_color_hex(s->muted), .hair = lv_color_hex(s->card),
        .accent = lv_color_hex(s->accent), .card = lv_color_hex(s->card),
        .on_accent = lv_color_hex(s->bg),
    };
    rk_theme_apply(&palette);
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int w, int h,
                       const lv_font_t *font, uint32_t color, const char *text)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_DOT);
    lv_label_set_text(o, text);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *art_create(lv_obj_t *parent, const radio_skin_t *skin)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, skin->art);
    lv_obj_set_pos(img, (360 - RADIO_SKIN_ART_W) / 2, 158 - RADIO_SKIN_ART_H / 2);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

/* Paper scenery is a bitmap; scale and pointer are live UI objects. */
static lv_obj_t *paper_dial_create(lv_obj_t *parent, const radio_skin_t *skin,
                                    lv_obj_t **needle)
{
    lv_obj_t *dial=rk_box(parent,36,164,288,82);
    lv_obj_set_style_bg_color(dial,lv_color_hex(skin->card),0);
    lv_obj_set_style_bg_opa(dial,LV_OPA_COVER,0);
    lv_obj_set_style_radius(dial,14,0);
    lv_obj_set_style_clip_corner(dial,true,0);
    lv_obj_set_style_border_width(dial,1,0);
    lv_obj_set_style_border_color(dial,lv_color_hex(0xC7BCA8),0);
    /* Keep the bitmap inside the card; draw its outline after children. */
    lv_obj_set_style_border_post(dial,true,0);
    lv_obj_t *window=rk_box(dial,RADIO_PAPER_SCENE_INSET,RADIO_PAPER_SCENE_INSET,
                           RADIO_PAPER_SCENE_W,RADIO_PAPER_SCENE_H);
    lv_obj_set_style_radius(window,8,0);
    lv_obj_set_style_clip_corner(window,true,0);
    lv_obj_t *scene=lv_img_create(window);
    lv_img_set_src(scene,&radio_art_paper_scene);
    lv_obj_set_pos(scene,0,0);
    lv_obj_clear_flag(scene,LV_OBJ_FLAG_CLICKABLE);
    for(int i=0;i<=40;++i){
        lv_obj_t *tick=rk_box(dial,24+i*6,44,i%8?1:2,i%8?5:10);
        lv_obj_set_style_bg_color(tick,lv_color_hex(skin->muted),0);
        lv_obj_set_style_bg_opa(tick,LV_OPA_COVER,0);
    }
    for(int i=0;i<=5;++i){
        char value[8];snprintf(value,sizeof(value),"%d",88+i*4);
        label(dial,12+i*48,57,26,18,RK_FONT_SMALL,skin->muted,value);
    }
    lv_obj_t *pin=rk_box(dial,144,36,3,20);
    lv_obj_set_style_bg_color(pin,lv_color_hex(skin->accent),0);
    lv_obj_set_style_bg_opa(pin,LV_OPA_COVER,0);
    if(needle)*needle=pin;
    else lv_obj_add_flag(pin,LV_OBJ_FLAG_HIDDEN);
    return dial;
}

static void dot_wave_draw(lv_event_t *e)
{
    radio_skin_view_t *v = lv_event_get_user_data(e);
    lv_draw_ctx_t *dc = lv_event_get_draw_ctx(e);
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_target(e), &area);
    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = lv_color_hex(radio_skin_get(v->skin)->ink);
    dot.bg_opa = LV_OPA_COVER;
    dot.radius = LV_RADIUS_CIRCLE;
    for (int col = 0; col < 47; ++col) {
        int pos = col * 17 * 256 / 46;
        int bin = pos / 256, frac = pos % 256;
        int value = v->dot_levels[bin];
        if (bin < 17) value = (value * (256 - frac) + v->dot_levels[bin + 1] * frac) / 256;
        int height = value * 3 / 255;
        int edge=LV_MIN(col,46-col);
        if(edge<6)height=height*edge/6;
        for (int row = -height; row <= height; ++row) {
            lv_area_t a = {area.x1 + col * 5, area.y1 + 15 + row * 4,
                           area.x1 + col * 5 + 1, area.y1 + 16 + row * 4};
            lv_draw_rect(dc, &dot, &a);
        }
    }
}

/* Small procedural layers reuse the approved artwork; no frame buffers,
 * additional bitmap assets, animation tasks or timers are allocated. */
static int motion_triangle(unsigned frame, unsigned period, int height)
{
    unsigned p=frame%period;
    return (int)(p<period/2?p:period-p)*height/(int)(period/2);
}

static void motion_pixel(lv_draw_ctx_t *dc, const lv_area_t *a, int x, int y,
                         int w, int h, uint32_t color, lv_opa_t opacity)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color=lv_color_hex(color); d.bg_opa=opacity;
    /* 动效坐标随画布放大，仍使用整数像素与原有色板。 */
    int width=lv_area_get_width(a),height=lv_area_get_height(a);
    lv_area_t box={a->x1+x*width/208,a->y1+y*height/118,
                   a->x1+(x+w)*width/208-1,a->y1+(y+h)*height/118-1};
    lv_draw_rect(dc,&d,&box);
}

static void motion_note(lv_draw_ctx_t *dc, const lv_area_t *a, int x, int y,
                        uint32_t color, lv_opa_t opacity)
{
    motion_pixel(dc,a,x,y+7,5,4,color,opacity);
    motion_pixel(dc,a,x+3,y,2,9,color,opacity);
    motion_pixel(dc,a,x+5,y,4,2,color,opacity);
}

static void art_motion_draw(lv_event_t *e)
{
    radio_skin_view_t *v=lv_event_get_user_data(e);
    const radio_skin_t *skin=radio_skin_get(v->skin);
    if(!v->motion_running) return;
    lv_draw_ctx_t *dc=lv_event_get_draw_ctx(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e),&a);
    unsigned f=v->motion_frame;
    if(skin->layout==RADIO_LAYOUT_VINYL){
        /* Keep the approved bitmap intact; only small musical notes move. */
        for(int i=0;i<2;++i){
            unsigned phase=(f+(unsigned)i*15)%30;
            motion_note(dc,&a,i?190:10,65-(int)phase,skin->accent,
                        (lv_opa_t)motion_triangle(phase,30,160));
        }
        return;
    }
    if(v->skin==2 || v->skin==4 || v->skin==8){
        for(int i=0;i<2;++i){
            unsigned p=(f+i*15)%30;
            lv_opa_t opacity=(lv_opa_t)(motion_triangle(p,30,200));
            motion_note(dc,&a,i?182:14,83-(int)p*2,skin->ink,opacity);
        }
    } else if(v->skin==5){
        static const uint8_t stars[][2]={{17,17},{33,55},{178,27},{190,78},{168,102}};
        for(unsigned i=0;i<5;++i){
            lv_opa_t opacity=(lv_opa_t)(40+motion_triangle(f+i*4,20,200));
            int x=stars[i][0],y=stars[i][1];
            motion_pixel(dc,&a,x-2,y,5,1,skin->accent,opacity);
            motion_pixel(dc,&a,x,y-2,1,5,skin->accent,opacity);
        }
    } else if(v->skin==6){
        for(int i=0;i<6;++i){
            int x=28+i*17+motion_triangle(f+(unsigned)i*4,20,10);
            int y=87+(i%4)*7;
            lv_opa_t opacity=(lv_opa_t)(70+motion_triangle(f+(unsigned)i*3,20,130));
            motion_pixel(dc,&a,x,y,10+(i%3)*4,2,i%2?skin->ink:0x58C9CE,opacity);
        }
    } else if(v->skin==7){
        for(int i=0;i<10;++i){
            int x=i<5?8+i*8:167+(i-5)*8;
            int y=7+(int)((f*5+(unsigned)i*17)%100);
            motion_pixel(dc,&a,x,y,1,6,0x75CDE3,150);
            motion_pixel(dc,&a,x-1,y+5,1,2,0x75CDE3,90);
        }
        for(int i=0;i<3;++i){
            int w=3+(int)((f+(unsigned)i*5)%10);
            motion_pixel(dc,&a,25+i*74-w/2,112-i%2*3,w,1,skin->accent,140);
        }
    } else if(v->skin==9){
        for(int i=0;i<4;++i){
            unsigned p=(f+(unsigned)i*15)%60;
            int x=(i%2?176:14)+motion_triangle(p,20,9);
            int y=12+(int)p*3/2;
            motion_pixel(dc,&a,x,y,4,2,skin->accent,190);
            motion_pixel(dc,&a,x+2,y+2,3,2,skin->accent,190);
        }
        for(int i=0;i<3;++i){
            lv_opa_t opacity=(lv_opa_t)(30+motion_triangle(f+(unsigned)i*7,20,210));
            motion_pixel(dc,&a,22+i*80,24+(i%2)*47,3,3,0xD0AD30,opacity);
        }
    }
}

void radio_skin_view_animate(radio_skin_view_t *v, uint32_t now, bool foreground)
{
    if(!v || !v->art_fx) return;
    uint32_t elapsed=now-v->motion_last;
    v->motion_last=now;
    if(!foreground) return;
    if(!v->motion_live){
        if(v->motion_running){
            v->motion_running=false;
            if(v->image)lv_obj_set_pos(v->image,RADIO_SKIN_ART_INSET,RADIO_SKIN_ART_INSET);
            lv_obj_invalidate(v->art_fx);
        }
        return;
    }
    if(elapsed>240) elapsed=240;
    v->motion_ms=(v->motion_ms+elapsed)%7200;
    uint8_t frame=(uint8_t)(v->motion_ms/120);
    if(v->motion_running && frame==v->motion_frame) return;
    v->motion_frame=frame;
    v->motion_running=true;
    int x=RADIO_SKIN_ART_INSET,y=RADIO_SKIN_ART_INSET;
    if(v->skin==2) y+=motion_triangle(frame,20,6)-3;
    else if(v->skin==4 || v->skin==8) y+=motion_triangle(frame,12,2)-1;
    else if(v->skin==5){x+=motion_triangle(frame,30,4)-2;y+=motion_triangle(frame,20,6)-3;}
    if(v->image)lv_obj_set_pos(v->image,x,y);
    lv_obj_invalidate(v->art_fx);
}

void radio_skin_preview(lv_obj_t *parent, int index)
{
    const radio_skin_t *s = radio_skin_get(index);
    if(s->layout==RADIO_LAYOUT_PAPER){
        label(parent,60,96,240,42,&s_medium_title_font,s->ink,SDG_T("城市音乐台","City Radio"));
        label(parent,60,140,240,20,RK_FONT_BODY,s->muted,"FM 103.7");
        paper_dial_create(parent,s,NULL);
    } else if (s->art) {
        art_create(parent, s);
    } else if (s->layout == RADIO_LAYOUT_DOTS) {
        static rk_dot_t preview;
        rk_dot_create(&preview, parent, 60, 108, 240, 72, 7, 3);
        rk_dot_set_color(&preview, lv_color_hex(s->ink));
        rk_dot_set_text(&preview, "103.7");
    } else if (s->layout == RADIO_LAYOUT_WAVE) {
        for (int i = 0; i < 18; ++i) {
            int h = 8 + (i * 17 % 41);
            lv_obj_t *bar = rk_box(parent, 74 + i * 12, 162 - h / 2, 6, h);
            lv_obj_set_style_bg_color(bar, lv_color_hex(i < 6 ? 0x536EFF : i < 12 ? 0x54DBCD : 0xADA2F2), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bar, 3, 0);
        }
    }
}

void radio_skin_view_create(radio_skin_view_t *v, lv_obj_t *parent, int index,
                            const radio_skin_actions_t *a)
{
    memset(v,0,sizeof(*v));
    v->skin=index;
    const radio_skin_t *s=radio_skin_get(index);
    /* 容器是用 rk_box 风格（remove_style_all）建的，bg_opa 默认是 0：
     * 只设 bg_color 不会画任何底色，屏幕透出宿主层的黑色，深色台名在
     * 纯黑上等于隐形（原版是 lv_obj_create(NULL) 的默认样式，天然 COVER）。 */
    lv_obj_set_style_bg_color(parent,lv_color_hex(s->bg),0);
    lv_obj_set_style_bg_opa(parent,LV_OPA_COVER,0);
    v->clock=label(parent,130,RADIO_SKIN_CLOCK_Y,100,RADIO_SKIN_CLOCK_H,RK_FONT_BODY,s->ink,"--:--");
    v->status=label(parent,60,RADIO_SKIN_STATUS_Y,240,RADIO_SKIN_STATUS_H,RK_FONT_SMALL,s->ink,"");
    v->live_dot=rk_box(parent,140,RADIO_SKIN_STATUS_Y+8,6,6);
    lv_obj_set_style_bg_color(v->live_dot,lv_color_hex(0xE1362D),0);
    lv_obj_set_style_bg_opa(v->live_dot,LV_OPA_COVER,0);
    if(s->art && s->layout!=RADIO_LAYOUT_PAPER){
        v->art_root=rk_box(parent,RADIO_SKIN_ART_ROOT_X,RADIO_SKIN_ART_ROOT_Y,RADIO_SKIN_ART_ROOT_W,RADIO_SKIN_ART_ROOT_H);
        if(s->art){
            v->image=lv_img_create(v->art_root);
            lv_img_set_src(v->image,s->art);
            lv_obj_set_pos(v->image,RADIO_SKIN_ART_INSET,RADIO_SKIN_ART_INSET);
            lv_obj_clear_flag(v->image,LV_OBJ_FLAG_CLICKABLE);
        }
        v->art_fx=rk_box(v->art_root,RADIO_SKIN_ART_INSET,2,RADIO_SKIN_ART_W,RADIO_SKIN_ART_H+2);
        lv_obj_add_event_cb(v->art_fx,art_motion_draw,LV_EVENT_DRAW_MAIN,v);
        v->motion_last=lv_tick_get();
    }
    if(s->layout==RADIO_LAYOUT_DOTS){
        rk_dot_create(&v->dots,parent,60,92,240,68,7,3);
        rk_dot_set_color(&v->dots,lv_color_hex(s->ink));
    }
    bool paper=s->layout==RADIO_LAYOUT_PAPER;
    if(paper)v->paper_dial=paper_dial_create(parent,s,&v->paper_needle);
    int name_y=paper?92:s->layout==RADIO_LAYOUT_WAVE?118:s->layout==RADIO_LAYOUT_DOTS?198:222;
    int name_h=paper?42:s->layout==RADIO_LAYOUT_WAVE?36:30;
    const lv_font_t *font=paper?&s_medium_title_font:s->layout==RADIO_LAYOUT_WAVE?&s_heading_font:&s_station_font;
    v->name=label(parent,60,name_y,240,name_h,font,s->ink,"");
    /* 省略号代替循环滚动，避免标题每帧重绘拖慢触摸。 */
    lv_label_set_long_mode(v->name,LV_LABEL_LONG_DOT);
    v->meta=label(parent,60,paper?140:s->layout==RADIO_LAYOUT_DOTS?172:252,
                  240,paper?20:18,paper?RK_FONT_BODY:RK_FONT_SMALL,s->muted,"");
    if(s->layout==RADIO_LAYOUT_DOTS){
        v->dot_wave=rk_box(parent,64,232,234,32);
        lv_obj_add_event_cb(v->dot_wave,dot_wave_draw,LV_EVENT_DRAW_MAIN,v);
    }
    bool meter=s->layout==RADIO_LAYOUT_WAVE;
    for(int i=0;meter&&i<18;i++){
        v->bars[i]=rk_box(parent,66+i*13,s->layout==RADIO_LAYOUT_WAVE?198:234,6,2);
        lv_obj_set_style_bg_color(v->bars[i],lv_color_hex(s->layout==RADIO_LAYOUT_WAVE?(i<6?0x536EFF:i<12?0x54DBCD:0xADA2F2):s->ink),0);
        lv_obj_set_style_bg_opa(v->bars[i],LV_OPA_COVER,0);
        lv_obj_set_style_radius(v->bars[i],s->layout==RADIO_LAYOUT_DOTS?0:3,0);
    }
    const int xs[]={76,158,240};
    const rk_icon_t icons[]={RK_ICON_PREV,RK_ICON_PLAY,RK_ICON_NEXT};
    const sdgoods_tap_cb_t callbacks[]={a->prev,a->play,a->next};
    for(int i=0;i<3;i++){
        /* 三键统一：无底色、无描边，图标一律用当前皮肤的前景色 ink，
         * 和上一首/下一首完全一致（原来中间键是实心圆/描边圆，显得突兀）。 */
        lv_obj_t *b=rk_round_btn(parent,xs[i],272,44,icons[i],
                                 lv_color_hex(s->ink),lv_color_hex(s->ink));
        lv_obj_set_style_bg_opa(b,LV_OPA_TRANSP,0);
        lv_obj_set_style_border_width(b,0,0);
        /* 圆屏下缘手指落点常比图标中心低十几像素；点击区四周各放大 14px
         * （视觉尺寸不变），点偏一点也能接住。 */
        /* 再把热区向下延展 1/3 按钮高度（44/3≈15px）：只加高对象、把图标钉回顶部，
         * 视觉位置不变，但屏幕下缘那片“点偏了没反应”的区域也被接住。 */
        lv_obj_set_height(b, 44 + 44 / 3);
        lv_obj_align(lv_obj_get_child(b, 0), LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_ext_click_area(b,14);
        sdgoods_tap_bind(b,&v->taps[i],callbacks[i],NULL);
        lv_obj_add_flag(b,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE);
        if(i==1)v->play_icon=lv_obj_get_child(b,0);
    }
}

static void text_update(lv_obj_t *o, const char *s)
{
    if (o && s && strcmp(lv_label_get_text(o), s) != 0) lv_label_set_text(o, s);
}

void radio_skin_view_update(radio_skin_view_t *v, const char *name, const char *frequency,
                            const char *status, const char *clock, bool playing,
                            const uint8_t *levels, int level_count)
{
    if (!v || !v->name) return;
    const radio_skin_t *s = radio_skin_get(v->skin);
    char short_name[RADIO_NAME_MAX+4];
    radio_station_short_name(name,short_name,sizeof(short_name));
    text_update(v->name,short_name);
    bool live=playing&&levels&&level_count>0;
    /* 只在真正播放/连接时跑封面动效：暂停或待机时静止，
     * 避免 8fps 的整块重绘拖慢触摸和页面操作。 */
    v->motion_live=playing;
    bool show_live=live && strcmp(status,SDG_T("直播中","Live"))==0;
    const char *word=show_live?"LIVE":status;
    text_update(v->status,word);
    lv_point_t size;
    lv_txt_get_size(&size,word,RK_FONT_SMALL,0,0,240,LV_TEXT_FLAG_NONE);
    /* “红点 + 文字”要作为一整组居中。文字单独居中在 x=180 时，左侧红点会把
     * 整组推左 6px；所以播放时把状态标签右移 6px，红点再按整组宽度左移，
     * 整组中心才落在屏幕中线上。 */
    lv_coord_t status_x=60+(show_live?6:0);
    if(lv_obj_get_x(v->status)!=status_x)lv_obj_set_x(v->status,status_x);
    lv_coord_t dot_x=174-size.x/2;
    lv_coord_t dot_y=RADIO_SKIN_STATUS_Y+8;
    if(lv_obj_get_x(v->live_dot)!=dot_x)lv_obj_set_x(v->live_dot,dot_x);
    if(lv_obj_get_y(v->live_dot)!=dot_y)lv_obj_set_y(v->live_dot,dot_y);
    if(show_live)lv_obj_clear_flag(v->live_dot,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(v->live_dot,LV_OBJ_FLAG_HIDDEN);
    text_update(v->clock,clock);
    bool fm=strncmp(frequency,"FM ",3)==0;
    bool channel=strncmp(frequency,"CH ",3)==0;
    if(v->paper_needle){
        float mhz=fm?strtof(frequency+3,NULL):0.0f;
        if(mhz>=88.0f && mhz<=108.0f){
            lv_obj_set_x(v->paper_needle,24+(lv_coord_t)((mhz-88.0f)*12.0f));
            lv_obj_clear_flag(v->paper_needle,LV_OBJ_FLAG_HIDDEN);
        }else lv_obj_add_flag(v->paper_needle,LV_OBJ_FLAG_HIDDEN);
    }
    if(s->layout==RADIO_LAYOUT_PAPER){
        text_update(v->meta,fm || channel?frequency:SDG_T("网络电台","Internet radio"));
    }else if(s->layout==RADIO_LAYOUT_DOTS){
        text_update(v->meta,fm?"FM · MHz":channel?SDG_T("网络频道","Web channel"):SDG_T("网络电台","Internet radio"));
    }else if(channel){
        char meta[48];snprintf(meta,sizeof(meta),SDG_T("频道 %s","Channel %s"),frequency+3);
        text_update(v->meta,meta);
    }else text_update(v->meta,fm?frequency:SDG_T("网络电台","Internet radio"));
    if (v->image && v->skin == 2) {
        const lv_img_dsc_t *art = live ? &radio_art_host : &radio_art_host_paused;
        if (lv_img_get_src(v->image) != art) lv_img_set_src(v->image, art);
    }
    if (v->dots.obj) {
        const char *dots = fm || channel ? frequency + 3 : "WEB";
        if (strcmp(v->dots.text, dots) != 0) rk_dot_set_text(&v->dots, dots);
    }
    if(v->dot_wave){
        bool dot_changed=false;
        for(int i=0;i<18;++i){
            uint8_t lv=playing&&levels&&i<level_count?levels[i]:0;
            if(v->dot_levels[i]!=lv){v->dot_levels[i]=lv;dot_changed=true;}
        }
        if(dot_changed)lv_obj_invalidate(v->dot_wave);
    }
    rk_icon_t icon = playing ? RK_ICON_PAUSE : RK_ICON_PLAY;
    if (v->play_icon && (rk_icon_t)(uintptr_t)lv_obj_get_user_data(v->play_icon) != icon) {
        lv_obj_set_user_data(v->play_icon, (void *)(uintptr_t)icon);
        lv_obj_invalidate(v->play_icon);
    }
    bool bars_live = playing && levels && level_count > 0;
    if (bars_live || v->bars_active) {
        /* 待机时只塌陷一次，不每 120ms 重复写 18 根柱子的尺寸/坐标
         * （每次写都会让整块区域重绘）。 */
        for (int i = 0; i < 18 && v->bars[i]; ++i) {
            int h = bars_live && i < level_count
                        ? 2 + levels[i] * (s->layout == RADIO_LAYOUT_WAVE ? 50 : 20) / 255
                        : 2;
            int center = s->layout == RADIO_LAYOUT_WAVE ? 198 : 234;
            lv_obj_set_height(v->bars[i], h);
            lv_obj_set_y(v->bars[i], center - h / 2);
        }
    }
    v->bars_active = bars_live;
}
