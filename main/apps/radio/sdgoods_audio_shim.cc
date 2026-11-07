// Bridges the radio player's stream calls onto the board audio codec.
#include "sdgoods_audio.h"

#include <esp_log.h>

#include "board.h"
#include "boards/sdgoods/dimension-screen/sdgoods_audio_codec.h"

static SdgoodsAudioCodec* StreamCodec() {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    return static_cast<SdgoodsAudioCodec*>(codec);
}

extern "C" esp_err_t sdgoods_audio_stream_start(uint32_t sample_rate_hz, uint8_t channels) {
    SdgoodsAudioCodec* codec = StreamCodec();
    if (codec == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec->StartStream((int)sample_rate_hz, (int)channels);
}

extern "C" esp_err_t sdgoods_audio_stream_write(const int16_t* pcm, size_t bytes) {
    SdgoodsAudioCodec* codec = StreamCodec();
    if (codec == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return codec->WriteStream(pcm, bytes);
}

extern "C" void sdgoods_audio_stream_stop(void) {
    SdgoodsAudioCodec* codec = StreamCodec();
    if (codec != nullptr) {
        codec->StopStream();
    }
}
