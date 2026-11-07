/*
 * sdgoods-radio · 电台目录服务（应用层）
 *
 * 职责（组织思路参考 leo-radio，实现完全基于谷仓平台重做，不引用 leo-radio 代码）：
 *   · 定位（ip-api.com，失败退 ipwho.is）拿国家码 + 城市名
 *   · 按城市名查 Radio Browser（用户选的城市优先，否则用定位城市），
 *     再按国家码补足，不足 8 台时补全球热门
 *   · 过滤不可播流（OGG/OPUS/FLAC/HLS 播放器解不了）、按 URL 去重
 *   · 网络全失败时回退内置精选台（保证开机可用）
 *   · 频率解析：从台名解析 87.0~108.0 MHz 的 FM 刻度（供圆屏表盘指针定位）
 *
 * radio_catalog_load 是阻塞网络调用，须在独立任务里调（UI 起个短任务即可）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RADIO_CATALOG_MAX  64
#define RADIO_NAME_MAX     128
#define RADIO_URL_MAX      200
#define RADIO_CODEC_MAX    8
#define RADIO_COUNTRY_MAX  4
#define RADIO_TAG_MAX      40

typedef struct {
    char     name[RADIO_NAME_MAX];
    char     url[RADIO_URL_MAX];      /* 拉流地址（优先 url_resolved） */
    char     codec[RADIO_CODEC_MAX];  /* "MP3" / "AAC" ...，供播放器判编码 */
    char     country[RADIO_COUNTRY_MAX];
    char     tag[RADIO_TAG_MAX];
    uint32_t bitrate;
    int      votes;
    float    freq_mhz;                /* 解析出的 FM 频率；0 = 无 */
} radio_station_t;

/* 拉取电台目录。city_zh 为 NULL/空串时用自动定位到的城市；非空则按该城市（中文名）
 * 查询。返回加载的电台数（>0 即有数据，网络全失败会回退内置台）。可重复调用以刷新。 */
int radio_catalog_load(const char *city_zh);

/* 同步装入内置国内电台（央广 21 路 HLS 直播）。零网络、零等待，
 * 进入界面即可显示/播放；为空时才填充，不覆盖已加载的列表。 */
void radio_catalog_seed_domestic(void);

/* 打开界面时先读上次成功拉取的目录（同步、不联网）。false = 无可用缓存。 */
bool radio_catalog_load_cache(void);
/* 缓存距今秒数；-1 = 无缓存或系统时间未同步（应刷新）。 */
int radio_catalog_cache_age(void);

int                    radio_catalog_count(void);
const radio_station_t *radio_catalog_at(int idx);
const radio_station_t *radio_catalog_next(int idx);   /* 循环取下一台（失败跳台用） */

/* 当前定位国家码（load 后可读）；未取到返回空串。 */
const char *radio_catalog_country(void);

/* 本次目录实际使用的城市名（用户选择或定位得到）；离线精选时返回「离线精选」。 */
const char *radio_catalog_city(void);

/* 从台名解析 FM 频率（87.0~108.0 MHz），失败返回 0。也可独立用于测试。 */
float radio_parse_freq(const char *name);
