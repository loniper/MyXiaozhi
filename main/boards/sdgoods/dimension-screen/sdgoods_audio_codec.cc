#include "sdgoods_audio_codec.h"

#include <esp_log.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "SdgoodsAudioCodec"

// 麦克风 24bit 数据左对齐于 32bit 槽，右移量与 SDGOODS 板级驱动一致（MIC_SHIFT 14）
static constexpr int kMicShift = 14;
// 功放使能后的建立时间（SDGOODS 板级驱动 PA_SETTLE_MS）
static constexpr int kPaSettleMs = 50;

SdgoodsAudioCodec::SdgoodsAudioCodec(int input_sample_rate, int output_sample_rate,
    gpio_num_t mic_bclk, gpio_num_t mic_ws, gpio_num_t mic_din,
    gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t spk_mclk,
    gpio_num_t pa_gpio) {
    duplex_ = false;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    pa_gpio_ = pa_gpio;

    // PA 默认关闭
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << pa_gpio_,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&pa_cfg));
    gpio_set_level(pa_gpio_, 0);

    // Create a new channel for speaker (I2S 控制器 0，16bit 立体声 + MCLK)
    // TX 的 DMA 垫底由 6×5ms≈30ms 加深到 6×10.7ms≈64ms（48kHz）：
    // 电台持续解码 + LVGL 刷屏同时跑时，任务调度抖动常到 30~50ms，
    // 原来刚好卡在 30ms 上就会周期性掉一小段；加深后这段抖动被 DMA 吃掉。
    i2s_chan_config_t chan_cfg = {
        .id = XIAOZHI_I2S_PORT(0),
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 480,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_384,
            #ifdef I2S_HW_VERSION_2
                .ext_clk_freq_hz = 0,
            #endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false,
            #endif
        },
        .gpio_cfg = {
            .mclk = spk_mclk,
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));

    // Create a new channel for MIC (I2S 控制器 1，32bit 单声道左槽)
    // MIC 不需要那么深的垫底，恢复默认，省内部 RAM。
    chan_cfg.id = XIAOZHI_I2S_PORT(1);
    chan_cfg.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, nullptr, &rx_handle_));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate_;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = mic_bclk;
    std_cfg.gpio_cfg.ws = mic_ws;
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Simplex channels created");
}

SdgoodsAudioCodec::~SdgoodsAudioCodec() {
    if (rx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr) {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
}

int SdgoodsAudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    // output_volume_: 0-100 -> volume_factor_: 0-65536（平方曲线，与 NoAudioCodec 一致）
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;

    // 单声道输入复制到左右两个 16bit 槽
    std::vector<int16_t> buffer(samples * 2);
    for (int i = 0; i < samples; i++) {
        int32_t value = (int32_t)((int64_t)data[i] * volume_factor >> 16);
        if (value > INT16_MAX) {
            value = INT16_MAX;
        } else if (value < INT16_MIN) {
            value = INT16_MIN;
        }
        buffer[2 * i] = (int16_t)value;
        buffer[2 * i + 1] = (int16_t)value;
    }

    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(), samples * 2 * sizeof(int16_t),
                                      &bytes_written, portMAX_DELAY));
    return bytes_written / (2 * sizeof(int16_t));
}

int SdgoodsAudioCodec::Read(int16_t* dest, int samples) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    size_t bytes_read;
    constexpr uint32_t kReadTimeoutMs = 200;

    std::vector<int32_t> bit32_buffer(samples);
    if (i2s_channel_read(rx_handle_, bit32_buffer.data(), samples * sizeof(int32_t),
                         &bytes_read, kReadTimeoutMs) != ESP_OK) {
        return 0;
    }

    samples = bytes_read / sizeof(int32_t);
    for (int i = 0; i < samples; i++) {
        int32_t value = bit32_buffer[i] >> kMicShift;
        dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : (int16_t)value;
    }
    return samples;
}

void SdgoodsAudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(rx_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    AudioCodec::EnableInput(enable);
}

void SdgoodsAudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (enable) {
        // 先开功放（含建立时间），再放 I2S 时钟，避免爆音
        gpio_set_level(pa_gpio_, 1);
        vTaskDelay(pdMS_TO_TICKS(kPaSettleMs));
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
        gpio_set_level(pa_gpio_, 0);
    }
    AudioCodec::EnableOutput(enable);
}

esp_err_t SdgoodsAudioCodec::StartStream(int sample_rate, int channels) {
    if (sample_rate <= 0 || channels <= 0 || channels > 2) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (streaming_ && stream_rate_ == sample_rate && stream_channels_ == channels) {
        return ESP_OK;
    }
    if (streaming_) {
        i2s_channel_disable(tx_handle_);
        streaming_ = false;
    } else {
        i2s_channel_disable(tx_handle_);
    }

    i2s_std_clk_config_t clk = {
        .sample_rate_hz = (uint32_t)sample_rate,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
        #ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
        #endif
    };
    esp_err_t err = i2s_channel_reconfig_std_clock(tx_handle_, &clk);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stream clock %d Hz failed: %s", sample_rate, esp_err_to_name(err));
        return err;
    }

    gpio_set_level(pa_gpio_, 1);
    vTaskDelay(pdMS_TO_TICKS(kPaSettleMs));
    err = i2s_channel_enable(tx_handle_);
    if (err != ESP_OK) {
        gpio_set_level(pa_gpio_, 0);
        ESP_LOGE(TAG, "stream enable failed: %s", esp_err_to_name(err));
        return err;
    }
    stream_rate_ = sample_rate;
    stream_channels_ = channels;
    streaming_ = true;
    output_enabled_ = true;
    ESP_LOGI(TAG, "stream started: %d Hz ch=%d", sample_rate, channels);
    return ESP_OK;
}

esp_err_t SdgoodsAudioCodec::WriteStream(const int16_t* pcm, size_t bytes) {
    if (pcm == nullptr || bytes < sizeof(int16_t)) {
        return ESP_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!streaming_) {
        return ESP_ERR_INVALID_STATE;
    }
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;
    size_t samples = bytes / sizeof(int16_t);
    size_t frames = stream_channels_ == 2 ? samples / 2 : samples;
    if (frames == 0) {
        return ESP_OK;
    }
    stream_buf_.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        int32_t left;
        int32_t right;
        if (stream_channels_ == 2) {
            left = pcm[2 * i];
            right = pcm[2 * i + 1];
        } else {
            left = right = pcm[i];
        }
        left = (int32_t)((int64_t)left * volume_factor >> 16);
        right = (int32_t)((int64_t)right * volume_factor >> 16);
        stream_buf_[2 * i] = (int16_t)(left > INT16_MAX ? INT16_MAX : (left < INT16_MIN ? INT16_MIN : left));
        stream_buf_[2 * i + 1] = (int16_t)(right > INT16_MAX ? INT16_MAX : (right < INT16_MIN ? INT16_MIN : right));
    }
    size_t written = 0;
    esp_err_t err = i2s_channel_write(tx_handle_, stream_buf_.data(),
                                      frames * 2 * sizeof(int16_t), &written,
                                      pdMS_TO_TICKS(500));
    if (err != ESP_OK) {
        return err;
    }
    return written == frames * 2 * sizeof(int16_t) ? ESP_OK : ESP_FAIL;
}

void SdgoodsAudioCodec::StopStream(void) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!streaming_) {
        return;
    }
    i2s_channel_disable(tx_handle_);
    gpio_set_level(pa_gpio_, 0);

    /* Restore the assistant playback clock so normal TTS works after exit. */
    i2s_std_clk_config_t clk = {
        .sample_rate_hz = (uint32_t)output_sample_rate_,
        .clk_src = I2S_CLK_SRC_DEFAULT,
        .mclk_multiple = I2S_MCLK_MULTIPLE_384,
        #ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
        #endif
    };
    i2s_channel_reconfig_std_clock(tx_handle_, &clk);

    streaming_ = false;
    stream_rate_ = 0;
    stream_channels_ = 1;
    output_enabled_ = false;
    ESP_LOGI(TAG, "stream stopped");
}
