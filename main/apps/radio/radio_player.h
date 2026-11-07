/*
 * sdgoods-radio · 网络电台播放器（应用层服务）
 *
 * 组织思路参考 leo-radio（generation 竞态控制、失败重试、断流重连、频谱电平），
 * 但完全基于谷仓平台重实现，不引用 leo-radio 任何代码：
 *   esp_http_client 拉流 → esp_audio_simple_dec 解码（MP3 / AAC）
 *   → sdgoods_audio_stream_start/write/stop 流式输出（平台层 Phase1 新增）。
 *
 * 超越点：同时支持 MP3 与 AAC（含解不出时自动改判兜底），采样率/声道运行时自适应。
 *
 * 播放逻辑跑在独立 FreeRTOS 任务；换台/停止用 generation 计数让旧任务自行收尾，
 * 避免竞态。失败重试用尽后状态置 RP_ERROR，由 UI 决定跳台（播放器只管单 URL）。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    RP_IDLE = 0,      /* 空闲：未播放 */
    RP_CONNECTING,    /* 正在连接 / 建立 HTTP */
    RP_BUFFERING,     /* 已连上，等待首帧解码 */
    RP_PLAYING,       /* 正常播放中 */
    RP_PAUSED,        /* 暂停（保留连接，停止喂数） */
    RP_ERROR,         /* 本次播放失败（重试用尽）；UI 可据此跳下一台 */
} radio_player_state_t;

/* 初始化播放器（准备任务所需资源，幂等）。 */
esp_err_t radio_player_init(void);

/* 播放给定 URL（等价于换台）：中止上一路 → 重连 → 解码 → 流式输出。
 * codec_hint 可传 "mp3"/"aac"（来自电台目录的 codec 字段），NULL 时默认按 MP3、
 * 解不出再自动改 AAC。 */
esp_err_t radio_player_play(const char *url, const char *codec_hint);

/* 停止播放（中止拉流、关功放）。 */
void radio_player_stop(void);

/* 暂停 / 恢复（暂停：保持 HTTP 连接，停止向 I2S 喂数据）。 */
void radio_player_set_paused(bool paused);

radio_player_state_t radio_player_state(void);
bool radio_player_is_busy(void);         /* CONNECTING / BUFFERING / PLAYING 之一 */
bool radio_player_is_active(void);       /* 非 IDLE（含暂停） */

/* 解码得到的实际音频参数（进入 PLAYING 后有效），供 UI 展示。 */
uint32_t radio_player_sample_rate(void);
uint8_t  radio_player_channels(void);

/* 电平表：填充 n 个 0~255 的条高（近似频谱分箱，供圆屏表盘动画）。返回实际填入条数。 */
int radio_player_levels(uint8_t *out, int n);

/* 推荐电平条数（与内部分箱一致）。 */
#define RADIO_PLAYER_LEVELS 18
