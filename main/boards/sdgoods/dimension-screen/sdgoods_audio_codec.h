#ifndef _SDGOODS_AUDIO_CODEC_H
#define _SDGOODS_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <mutex>

// 谷仓次元屏板载音频（无 codec 芯片）：
//   TX: I2S 控制器 0，16bit 立体声 + MCLK（功放需要 MCLK 时基），PA 由 GPIO 控制
//   RX: I2S 控制器 1，32bit 单声道左槽（MEMS 数字麦，24bit 数据左对齐于 32bit 槽）
// 参数取自 SDGOODS-ESP32S3 板级驱动 sdgoods_audio.c 的实机验证配置。
class SdgoodsAudioCodec : public AudioCodec {
public:
    SdgoodsAudioCodec(int input_sample_rate, int output_sample_rate,
        gpio_num_t mic_bclk, gpio_num_t mic_ws, gpio_num_t mic_din,
        gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t spk_mclk,
        gpio_num_t pa_gpio);
    virtual ~SdgoodsAudioCodec();

    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;

    // Raw PCM streaming for the radio app: reconfigures the speaker I2S clock
    // to the decoded rate and writes interleaved int16 PCM (mono or stereo).
    esp_err_t StartStream(int sample_rate, int channels);
    esp_err_t WriteStream(const int16_t* pcm, size_t bytes);
    void StopStream();

protected:
    virtual int Write(const int16_t* data, int samples) override;
    virtual int Read(int16_t* dest, int samples) override;

private:
    /* 麦克风与喇叭是两个独立的 I2S 通道（i2s_new_channel 各建一个），
     * 必须用各自的锁：Read() 会阻塞最多 200ms 等麦克风数据，若与写喇叭共用
     * 一把锁，电台每次写 I2S 都可能被挡住 几十~200ms，远超 DMA 垫底时长，
     * 听感就是周期性“卡一下/掉一小段”。 */
    std::mutex data_if_mutex_;
    std::mutex rx_mutex_;
    gpio_num_t pa_gpio_;
    int stream_channels_ = 1;
    int stream_rate_ = 0;
    bool streaming_ = false;
    std::vector<int16_t> stream_buf_;
};

#endif // _SDGOODS_AUDIO_CODEC_H
