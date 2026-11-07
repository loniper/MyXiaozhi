/*
 * sdgoods-radio · 电台偏好设置（应用层 · 持久化服务）
 *
 * 方案的 radio_settings 定位：音量/亮度/定时/收藏里，音量·亮度已由平台控制中心
 * （sdgoods_cc）统一管理与持久化，本模块只补「电台应用私有」的那部分偏好，走 NVS：
 *   · 定时休眠分钟数（0=关）——到点由 UI 停播；
 *   · 上次收听的台索引——开机续播；
 *   · 城市（空串 = 自动定位）——决定目录拉哪个城市的台；
 *   · 主题（0 纸张 / 1 夜色）；
 *   · 收藏标记（bitmap，最多 32 台，为长列表快速回跳预留）。
 *
 * 独立命名空间 "radiocfg"，与配网 wifi_prov 的 "radio" 分开，互不影响。
 * 所有读写都做一次 NVS open/get/set/commit，量小、低频，够用；不缓存以免与配网抢内存。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 开机调用一次（幂等）。NVS 已由 main 初始化，这里只确保命名空间可开。 */
void radio_settings_init(void);

/* ---- 定时休眠（分钟，0=关闭）---- */
int  radio_settings_get_sleep_min(void);
void radio_settings_set_sleep_min(int minutes);

/* ---- 上次收听台索引（-1=无历史）---- */
int  radio_settings_get_last_index(void);
void radio_settings_set_last_index(int idx);

/* ---- 城市：UTF-8 中文城市名，空串 = 自动定位。返回值指向内部静态缓冲，
 *      下次调用前有效；写入超长会被截断。---- */
const char *radio_settings_get_city(void);
bool        radio_settings_set_city(const char *city_zh);

/* ---- 主题：true = 夜色 ---- */
bool radio_settings_get_night(void);
void radio_settings_set_night(bool night);

/* Stable skin ID. Legacy night preference is used when skin_id is absent. */
const char *radio_settings_get_skin(void);
bool radio_settings_set_skin(const char *id);

/* ---- 收藏：32 位位图，set/clear/toggle/get，idx 越界(0..31)安全忽略 ---- */
bool radio_settings_is_fav(int idx);
void radio_settings_set_fav(int idx, bool on);
void radio_settings_toggle_fav(int idx);
