// Platform stream API expected by the ported radio player.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sdgoods_audio_stream_start(uint32_t sample_rate_hz, uint8_t channels);
esp_err_t sdgoods_audio_stream_write(const int16_t *pcm, size_t bytes);
void      sdgoods_audio_stream_stop(void);

#ifdef __cplusplus
}
#endif
