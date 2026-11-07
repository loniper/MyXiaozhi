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
 */
#pragma once
#include <stddef.h>

/* Published station identity, independent of online directory naming. */
float radio_station_known_frequency(const char *name);
void radio_station_short_name(const char *name, char *out, size_t capacity);
void radio_station_copy_utf8(const char *name, char *out, size_t capacity);
