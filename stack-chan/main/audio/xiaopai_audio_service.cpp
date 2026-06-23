#include "xiaopai_audio_service.h"

#include <M5Unified.h>

#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "aw88298_dac.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "es7210_adc.h"
#include "esp_audio_dec.h"
#include "esp_audio_types.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "sdkconfig.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
#include "esp_afe_sr_models.h"
#include "model_path.h"
#endif
#endif

#ifndef CONFIG_STACKCHAN_AUDIO_FULL_DUPLEX
#define CONFIG_STACKCHAN_AUDIO_FULL_DUPLEX 1
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
#define CONFIG_STACKCHAN_AUDIO_DEVICE_AEC 0
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_HW_SAMPLE_RATE
#define CONFIG_STACKCHAN_AUDIO_HW_SAMPLE_RATE 24000
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_PROTOCOL_SAMPLE_RATE
#define CONFIG_STACKCHAN_AUDIO_PROTOCOL_SAMPLE_RATE 16000
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_INPUT_REFERENCE
#define CONFIG_STACKCHAN_AUDIO_INPUT_REFERENCE 0
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_INPUT_GAIN
#define CONFIG_STACKCHAN_AUDIO_INPUT_GAIN 60
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_OUTPUT_VOLUME_DEFAULT
#define CONFIG_STACKCHAN_AUDIO_OUTPUT_VOLUME_DEFAULT 70
#endif
#ifndef CONFIG_STACKCHAN_MIC_MAGNIFICATION
#define CONFIG_STACKCHAN_MIC_MAGNIFICATION 1
#endif

namespace {

static constexpr const char* TAG = "XiaopaiAudio";
static constexpr i2s_port_t kAudioI2sPort = I2S_NUM_0;
static constexpr gpio_num_t kAudioMclkPin = GPIO_NUM_0;
static constexpr gpio_num_t kAudioWsPin = GPIO_NUM_33;
static constexpr gpio_num_t kAudioBclkPin = GPIO_NUM_34;
static constexpr gpio_num_t kAudioDinPin = GPIO_NUM_14;
static constexpr gpio_num_t kAudioDoutPin = GPIO_NUM_13;
static constexpr uint8_t kAxp2101Addr = 0x34;
static constexpr uint8_t kAw9523Addr = 0x58;
static constexpr uint8_t kAw88298Addr = AW88298_CODEC_DEFAULT_ADDR;
static constexpr uint8_t kEs7210Addr = ES7210_CODEC_DEFAULT_ADDR;
static constexpr uint32_t kInternalI2cFreq = 400000;
static constexpr int kHwSampleRate = CONFIG_STACKCHAN_AUDIO_HW_SAMPLE_RATE;
static constexpr int kProtocolSampleRate = CONFIG_STACKCHAN_AUDIO_PROTOCOL_SAMPLE_RATE;
static constexpr int kOpusFrameDurationMs = 60;
static constexpr int kProtocolFrameSamples = kProtocolSampleRate * kOpusFrameDurationMs / 1000;
static constexpr bool kDeviceAecEnabled = CONFIG_STACKCHAN_AUDIO_DEVICE_AEC && CONFIG_STACKCHAN_AUDIO_INPUT_REFERENCE;
static constexpr int kInputChannels = 2;
static constexpr uint16_t kRawInputChannelMask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
static constexpr uint16_t kAecInputChannelMask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                                                ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
static constexpr uint16_t kEs7210MicChannelMask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                                                 ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1) |
                                                 ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2);
static constexpr int kHwInputChunkFrames = kHwSampleRate / 100;
static constexpr int kHwInputChunkSamples = kHwInputChunkFrames * kInputChannels;
static constexpr int kDmaDescNum = 6;
static constexpr int kDmaFrameNum = 240;
static constexpr int kPlayQueueDepth = 8;
static constexpr int kCleanQueueDepth = 32;
static constexpr TickType_t kCleanQueueConsumerGraceTicks = pdMS_TO_TICKS(300);
static constexpr int kToneChunkSamples = 512;
static constexpr int kTailDrainMs = 80;
static constexpr float kTwoPi = 6.28318530717958647692f;
static constexpr uint8_t kAw9523SpeakerPowerMask = 0b00000100;
static constexpr uint8_t kAw9523BoostPowerMask = 0b10000000;

struct AudioBlock {
    size_t samples = 0;
    int16_t data[1];
};

struct M5I2cCodecCtrl {
    audio_codec_ctrl_if_t base;
    bool open = false;
    uint8_t addr = 0;
};

static int clamp_volume_percent(int percent)
{
    return std::max(0, std::min(100, percent));
}

static AudioBlock* allocate_block(size_t samples)
{
    if (samples == 0) {
        return nullptr;
    }
    const size_t bytes = sizeof(AudioBlock) + (samples - 1) * sizeof(int16_t);
    void* mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem == nullptr) {
        mem = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    auto* block = static_cast<AudioBlock*>(mem);
    if (block != nullptr) {
        block->samples = samples;
    }
    return block;
}

static void free_block(AudioBlock* block)
{
    if (block != nullptr) {
        heap_caps_free(block);
    }
}

static int peak_abs_sample(const int16_t* data, size_t samples)
{
    int peak = 0;
    if (data == nullptr) {
        return peak;
    }
    for (size_t i = 0; i < samples; ++i) {
        int value = data[i];
        int abs_value = value < 0 ? -value : value;
        if (abs_value > peak) {
            peak = abs_value;
        }
    }
    return peak;
}

static bool write_i2c_reg8(uint8_t addr, uint8_t reg, uint8_t value, const char* label)
{
    bool ok = M5.In_I2C.writeRegister8(addr, reg, value, kInternalI2cFreq);
    ESP_LOGI(TAG, "%s write addr=0x%02x reg=0x%02x value=0x%02x %s",
             label != nullptr ? label : "I2C", addr, reg, value, ok ? "OK" : "FAIL");
    return ok;
}

static uint8_t read_i2c_reg8(uint8_t addr, uint8_t reg, const char* label)
{
    uint8_t value = M5.In_I2C.readRegister8(addr, reg, kInternalI2cFreq);
    ESP_LOGI(TAG, "%s read addr=0x%02x reg=0x%02x value=0x%02x",
             label != nullptr ? label : "I2C", addr, reg, value);
    return value;
}

static bool read_codec_reg_direct(uint8_t addr_8bit, uint8_t reg, uint16_t* value)
{
    uint8_t data[2] = {};
    if (!M5.In_I2C.readRegister(addr_8bit >> 1, reg, data, sizeof(data), kInternalI2cFreq)) {
        ESP_LOGW(TAG, "codec direct read addr=0x%02x reg=0x%02x failed", addr_8bit >> 1, reg);
        return false;
    }
    *value = (static_cast<uint16_t>(data[0]) << 8) | data[1];
    return true;
}

static bool write_aw88298_reg_direct(uint8_t reg, uint16_t value, const char* label)
{
    uint8_t data[2] = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value & 0xff)};
    bool ok = M5.In_I2C.writeRegister(kAw88298Addr >> 1, reg, data, sizeof(data), kInternalI2cFreq);
    ESP_LOGI(TAG, "%s write AW88298 reg=0x%02x value=0x%04x %s",
             label != nullptr ? label : "AW88298", reg, value, ok ? "OK" : "FAIL");
    return ok;
}

static void log_aw88298_reg_direct(uint8_t reg)
{
    uint16_t value = 0;
    if (read_codec_reg_direct(kAw88298Addr, reg, &value)) {
        ESP_LOGI(TAG, "AW88298 direct reg 0x%02x = 0x%04x", reg, value);
    }
}

static uint16_t aw88298_i2sctrl_for_sample_rate(int sample_rate)
{
    static constexpr uint8_t rate_table[] = {4, 5, 6, 8, 10, 11, 15, 20, 22, 44};
    size_t rate_index = 0;
    size_t rate = static_cast<size_t>(std::max(8000, sample_rate) + 1102) / 2205;
    while (rate_index + 1 < sizeof(rate_table) && rate > rate_table[rate_index]) {
        ++rate_index;
    }
    return static_cast<uint16_t>(0x14c0 | rate_index);
}

static int16_t apply_mic_magnification(int16_t sample)
{
    if (CONFIG_STACKCHAN_MIC_MAGNIFICATION <= 1) {
        return sample;
    }
    int32_t amplified = static_cast<int32_t>(sample) * CONFIG_STACKCHAN_MIC_MAGNIFICATION;
    return static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, amplified)));
}

static void restore_aw88298_playback_registers(int sample_rate)
{
    write_aw88298_reg_direct(0x61, 0x0673, "AW88298 restore boost");
    write_aw88298_reg_direct(0x04, 0x4040, "AW88298 restore sysctrl");
    write_aw88298_reg_direct(0x05, 0x0008, "AW88298 restore unmute");
    write_aw88298_reg_direct(0x06, aw88298_i2sctrl_for_sample_rate(sample_rate), "AW88298 restore i2sctrl");
}

static void configure_core_s3_audio_power()
{
    bool aw9523_present = M5.In_I2C.scanID(kAw9523Addr, kInternalI2cFreq);
    bool aw88298_present = M5.In_I2C.scanID(kAw88298Addr >> 1, kInternalI2cFreq);
    bool es7210_present = M5.In_I2C.scanID(kEs7210Addr >> 1, kInternalI2cFreq);
    bool axp2101_present = M5.In_I2C.scanID(kAxp2101Addr, kInternalI2cFreq);
    ESP_LOGI(TAG, "Audio I2C scan: AW9523=%d AW88298=%d ES7210=%d AXP2101=%d",
             aw9523_present, aw88298_present, es7210_present, axp2101_present);

    if (axp2101_present) {
        write_i2c_reg8(kAxp2101Addr, 0x69, 0b00110101, "AXP2101 charge/power");
        write_i2c_reg8(kAxp2101Addr, 0x30, 0b00111111, "AXP2101 power path");
        write_i2c_reg8(kAxp2101Addr, 0x90, 0xBF, "AXP2101 LDOS");
        write_i2c_reg8(kAxp2101Addr, 0x92, 18 - 5, "AXP2101 ALDO1");
        write_i2c_reg8(kAxp2101Addr, 0x93, 33 - 5, "AXP2101 ALDO2");
        write_i2c_reg8(kAxp2101Addr, 0x94, 33 - 5, "AXP2101 ALDO3");
        write_i2c_reg8(kAxp2101Addr, 0x95, 33 - 5, "AXP2101 ALDO4");
        write_i2c_reg8(kAxp2101Addr, 0x97, 0b11110 - 2, "AXP2101 BLDO2");
        write_i2c_reg8(kAxp2101Addr, 0x27, 0x00, "AXP2101 IRQ");
        read_i2c_reg8(kAxp2101Addr, 0x90, "AXP2101 LDOS");
        read_i2c_reg8(kAxp2101Addr, 0x92, "AXP2101 ALDO1");
        read_i2c_reg8(kAxp2101Addr, 0x93, "AXP2101 ALDO2");
        read_i2c_reg8(kAxp2101Addr, 0x94, "AXP2101 ALDO3");
        read_i2c_reg8(kAxp2101Addr, 0x95, "AXP2101 ALDO4");
        read_i2c_reg8(kAxp2101Addr, 0x97, "AXP2101 BLDO2");
    }

    if (aw9523_present) {
        write_i2c_reg8(kAw9523Addr, 0x04, 0b00011000, "AW9523 P0 config");
        write_i2c_reg8(kAw9523Addr, 0x05, 0b00001100, "AW9523 P1 config");
        write_i2c_reg8(kAw9523Addr, 0x11, 0b00010000, "AW9523 GCR");
        write_i2c_reg8(kAw9523Addr, 0x12, 0xff, "AW9523 P0 LED mode");
        write_i2c_reg8(kAw9523Addr, 0x13, 0xff, "AW9523 P1 LED mode");
        bool boost_ok = M5.In_I2C.bitOn(kAw9523Addr, 0x03, kAw9523BoostPowerMask, kInternalI2cFreq);
        bool spk_ok = M5.In_I2C.bitOn(kAw9523Addr, 0x02, kAw9523SpeakerPowerMask, kInternalI2cFreq);
        ESP_LOGI(TAG, "AW9523 audio power: boost=%s speaker=%s",
                 boost_ok ? "OK" : "FAIL", spk_ok ? "OK" : "FAIL");
    }
}

static void reset_core_s3_aw88298()
{
    if (!M5.In_I2C.scanID(kAw9523Addr, kInternalI2cFreq)) {
        ESP_LOGW(TAG, "AW9523 not found; skip AW88298 reset");
        return;
    }
    M5.In_I2C.bitOff(kAw9523Addr, 0x02, kAw9523SpeakerPowerMask, kInternalI2cFreq);
    vTaskDelay(pdMS_TO_TICKS(10));
    M5.In_I2C.bitOn(kAw9523Addr, 0x02, kAw9523SpeakerPowerMask, kInternalI2cFreq);
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "AW88298 reset via AW9523 speaker gate");
}

static int m5_i2c_ctrl_open(const audio_codec_ctrl_if_t* ctrl, void* cfg, int cfg_size)
{
    if (ctrl == nullptr || cfg == nullptr || cfg_size != sizeof(audio_codec_i2c_cfg_t)) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    auto* self = reinterpret_cast<M5I2cCodecCtrl*>(const_cast<audio_codec_ctrl_if_t*>(ctrl));
    auto* i2c_cfg = reinterpret_cast<audio_codec_i2c_cfg_t*>(cfg);
    self->addr = i2c_cfg->addr;
    self->open = true;
    return ESP_CODEC_DEV_OK;
}

static bool m5_i2c_ctrl_is_open(const audio_codec_ctrl_if_t* ctrl)
{
    if (ctrl == nullptr) {
        return false;
    }
    auto* self = reinterpret_cast<M5I2cCodecCtrl*>(const_cast<audio_codec_ctrl_if_t*>(ctrl));
    return self->open;
}

static int m5_i2c_ctrl_read_reg(const audio_codec_ctrl_if_t* ctrl, int reg, int reg_len, void* data, int data_len)
{
    if (ctrl == nullptr || data == nullptr || reg_len != 1 || data_len <= 0) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    auto* self = reinterpret_cast<M5I2cCodecCtrl*>(const_cast<audio_codec_ctrl_if_t*>(ctrl));
    if (!self->open) {
        return ESP_CODEC_DEV_WRONG_STATE;
    }
    bool ok = M5.In_I2C.readRegister(self->addr >> 1, static_cast<uint8_t>(reg), static_cast<uint8_t*>(data),
                                     data_len, kInternalI2cFreq);
    return ok ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_READ_FAIL;
}

static int m5_i2c_ctrl_write_reg(const audio_codec_ctrl_if_t* ctrl, int reg, int reg_len, void* data, int data_len)
{
    if (ctrl == nullptr || data == nullptr || reg_len != 1 || data_len < 0) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    auto* self = reinterpret_cast<M5I2cCodecCtrl*>(const_cast<audio_codec_ctrl_if_t*>(ctrl));
    if (!self->open) {
        return ESP_CODEC_DEV_WRONG_STATE;
    }
    bool ok = M5.In_I2C.writeRegister(self->addr >> 1, static_cast<uint8_t>(reg), static_cast<uint8_t*>(data),
                                      data_len, kInternalI2cFreq);
    return ok ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_WRITE_FAIL;
}

static int m5_i2c_ctrl_close(const audio_codec_ctrl_if_t* ctrl)
{
    if (ctrl == nullptr) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    auto* self = reinterpret_cast<M5I2cCodecCtrl*>(const_cast<audio_codec_ctrl_if_t*>(ctrl));
    self->open = false;
    return ESP_CODEC_DEV_OK;
}

static void init_m5_i2c_ctrl(M5I2cCodecCtrl* ctrl, uint8_t addr)
{
    ctrl->base.open = m5_i2c_ctrl_open;
    ctrl->base.is_open = m5_i2c_ctrl_is_open;
    ctrl->base.read_reg = m5_i2c_ctrl_read_reg;
    ctrl->base.write_reg = m5_i2c_ctrl_write_reg;
    ctrl->base.close = m5_i2c_ctrl_close;
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 1,
        .addr = addr,
        .bus_handle = nullptr,
    };
    ctrl->base.open(&ctrl->base, &i2c_cfg, sizeof(i2c_cfg));
}

static void resample_linear_mono(const int16_t* in, size_t in_samples, int src_rate, std::vector<int16_t>& out,
                                 int dst_rate)
{
    if (in == nullptr || in_samples == 0 || src_rate <= 0 || dst_rate <= 0) {
        out.clear();
        return;
    }
    const size_t out_samples = static_cast<size_t>((static_cast<uint64_t>(in_samples) * dst_rate + src_rate - 1) / src_rate);
    out.resize(out_samples);
    for (size_t i = 0; i < out_samples; ++i) {
        const uint64_t pos_num = static_cast<uint64_t>(i) * src_rate;
        size_t idx = static_cast<size_t>(pos_num / dst_rate);
        const uint32_t frac = static_cast<uint32_t>(pos_num % dst_rate);
        if (idx + 1 >= in_samples) {
            out[i] = in[in_samples - 1];
            continue;
        }
        int32_t a = in[idx];
        int32_t b = in[idx + 1];
        int32_t sample = a + static_cast<int32_t>((static_cast<int64_t>(b - a) * frac) / dst_rate);
        out[i] = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, sample)));
    }
}

static void resample_linear_interleaved(const int16_t* in, size_t in_frames, int channels, int src_rate,
                                        std::vector<int16_t>& out, int dst_rate)
{
    if (in == nullptr || in_frames == 0 || channels <= 0 || src_rate <= 0 || dst_rate <= 0) {
        out.clear();
        return;
    }
    const size_t out_frames = static_cast<size_t>((static_cast<uint64_t>(in_frames) * dst_rate + src_rate - 1) / src_rate);
    out.resize(out_frames * channels);
    for (size_t i = 0; i < out_frames; ++i) {
        const uint64_t pos_num = static_cast<uint64_t>(i) * src_rate;
        size_t idx = static_cast<size_t>(pos_num / dst_rate);
        const uint32_t frac = static_cast<uint32_t>(pos_num % dst_rate);
        if (idx + 1 >= in_frames) {
            idx = in_frames - 1;
            for (int ch = 0; ch < channels; ++ch) {
                out[i * channels + ch] = in[idx * channels + ch];
            }
            continue;
        }
        for (int ch = 0; ch < channels; ++ch) {
            int32_t a = in[idx * channels + ch];
            int32_t b = in[(idx + 1) * channels + ch];
            int32_t sample = a + static_cast<int32_t>((static_cast<int64_t>(b - a) * frac) / dst_rate);
            out[i * channels + ch] = static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, sample)));
        }
    }
}

class XiaopaiAudioCodec {
public:
    bool init()
    {
        if (initialized_) {
            return true;
        }

        configure_core_s3_audio_power();
        reset_core_s3_aw88298();
        init_m5_i2c_ctrl(&out_ctrl_, kAw88298Addr);
        init_m5_i2c_ctrl(&in_ctrl_, kEs7210Addr);

        if (!create_duplex_channels()) {
            return false;
        }

        audio_codec_i2s_cfg_t i2s_cfg = {
            .port = static_cast<uint8_t>(kAudioI2sPort),
            .rx_handle = rx_handle_,
            .tx_handle = tx_handle_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
        };
        data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
        if (data_if_ == nullptr) {
            ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
            return false;
        }

        gpio_if_ = audio_codec_new_gpio();
        if (gpio_if_ == nullptr) {
            ESP_LOGE(TAG, "audio_codec_new_gpio failed");
            return false;
        }

        aw88298_codec_cfg_t aw88298_cfg = {};
        aw88298_cfg.ctrl_if = &out_ctrl_.base;
        aw88298_cfg.gpio_if = gpio_if_;
        aw88298_cfg.reset_pin = GPIO_NUM_NC;
        aw88298_cfg.hw_gain.pa_voltage = 5.0;
        aw88298_cfg.hw_gain.codec_dac_voltage = 3.3;
        aw88298_cfg.hw_gain.pa_gain = 1;
        out_codec_if_ = aw88298_codec_new(&aw88298_cfg);
        if (out_codec_if_ == nullptr) {
            ESP_LOGE(TAG, "aw88298_codec_new failed");
            return false;
        }
        esp_codec_dev_cfg_t out_dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_OUT,
            .codec_if = out_codec_if_,
            .data_if = data_if_,
        };
        output_dev_ = esp_codec_dev_new(&out_dev_cfg);
        if (output_dev_ == nullptr) {
            ESP_LOGE(TAG, "esp_codec_dev_new output failed");
            return false;
        }

        es7210_codec_cfg_t es7210_cfg = {};
        es7210_cfg.ctrl_if = &in_ctrl_.base;
        es7210_cfg.master_mode = false;
        es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
        es7210_cfg.mclk_src = ES7210_MCLK_FROM_PAD;
        es7210_cfg.mclk_div = 256;
        in_codec_if_ = es7210_codec_new(&es7210_cfg);
        if (in_codec_if_ == nullptr) {
            ESP_LOGE(TAG, "es7210_codec_new failed");
            return false;
        }
        esp_codec_dev_cfg_t in_dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN,
            .codec_if = in_codec_if_,
            .data_if = data_if_,
        };
        input_dev_ = esp_codec_dev_new(&in_dev_cfg);
        if (input_dev_ == nullptr) {
            ESP_LOGE(TAG, "esp_codec_dev_new input failed");
            return false;
        }

        initialized_ = true;
        ESP_LOGI(TAG, "CoreS3 duplex codec initialized: hw_rate=%d channels=%d reference=%d device_aec=%d mic_mag=%d",
                 kHwSampleRate, kInputChannels, CONFIG_STACKCHAN_AUDIO_INPUT_REFERENCE,
                 CONFIG_STACKCHAN_AUDIO_DEVICE_AEC, CONFIG_STACKCHAN_MIC_MAGNIFICATION);
        return true;
    }

    bool start()
    {
        if (!init()) {
            return false;
        }
        return open_output() && open_input();
    }

    bool open_output()
    {
        if (output_open_) {
            return true;
        }
        configure_core_s3_audio_power();
        reset_core_s3_aw88298();
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = static_cast<uint32_t>(kHwSampleRate),
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        };
        int ret = esp_codec_dev_open(output_dev_, &fs);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_open output failed: %d", ret);
            return false;
        }
        restore_aw88298_playback_registers(kHwSampleRate);
        set_volume(volume_percent_);
        output_open_ = true;
        ESP_LOGI(TAG, "AW88298 output opened at %d Hz", kHwSampleRate);
        return true;
    }

    bool open_input()
    {
        if (input_open_) {
            return true;
        }
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = kInputChannels,
            .channel_mask = kDeviceAecEnabled ? kAecInputChannelMask : kRawInputChannelMask,
            .sample_rate = static_cast<uint32_t>(kHwSampleRate),
            .mclk_multiple = 0,
        };
        int ret = esp_codec_dev_open(input_dev_, &fs);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "esp_codec_dev_open input failed: %d", ret);
            return false;
        }
        ret = esp_codec_dev_set_in_channel_gain(input_dev_, kEs7210MicChannelMask,
                                                CONFIG_STACKCHAN_AUDIO_INPUT_GAIN);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_set_in_channel_gain failed: %d", ret);
        }
        input_open_ = true;
        ESP_LOGI(TAG, "ES7210 input opened at %d Hz, channels=%d mask=0x%04x gain=%d",
                 kHwSampleRate, kInputChannels, fs.channel_mask, CONFIG_STACKCHAN_AUDIO_INPUT_GAIN);
        return true;
    }

    bool read(int16_t* dest, size_t samples)
    {
        if (!input_open_ || input_dev_ == nullptr || dest == nullptr || samples == 0) {
            return false;
        }
        int ret = esp_codec_dev_read(input_dev_, dest, static_cast<int>(samples * sizeof(int16_t)));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_read failed: %d", ret);
            return false;
        }
        return true;
    }

    bool write(const int16_t* data, size_t samples)
    {
        if (!output_open_ || output_dev_ == nullptr || data == nullptr || samples == 0) {
            return false;
        }
        int ret = esp_codec_dev_write(output_dev_, const_cast<int16_t*>(data), static_cast<int>(samples * sizeof(int16_t)));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_write failed: %d", ret);
            return false;
        }
        return true;
    }

    void set_volume(int percent)
    {
        volume_percent_ = clamp_volume_percent(percent);
        if (output_dev_ != nullptr) {
            int ret = esp_codec_dev_set_out_vol(output_dev_, volume_percent_);
            if (ret != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "esp_codec_dev_set_out_vol failed: %d", ret);
            }
        }
    }

    void dump_state()
    {
        configure_core_s3_audio_power();
        log_aw88298_reg_direct(0x00);
        log_aw88298_reg_direct(0x04);
        log_aw88298_reg_direct(0x05);
        log_aw88298_reg_direct(0x06);
        log_aw88298_reg_direct(0x0C);
        log_aw88298_reg_direct(0x61);
        if (output_dev_ != nullptr) {
            ESP_LOGI(TAG, "AW88298 esp_codec_dev register dump follows");
            esp_codec_dev_dump_reg(output_dev_);
        }
        if (input_dev_ != nullptr) {
            ESP_LOGI(TAG, "ES7210 esp_codec_dev register dump follows");
            esp_codec_dev_dump_reg(input_dev_);
        }
        ESP_LOGI(TAG, "state: initialized=%d input_open=%d output_open=%d volume=%d",
                 initialized_, input_open_, output_open_, volume_percent_);
    }

    bool initialized() const { return initialized_; }

private:
    bool create_duplex_channels()
    {
        if (tx_handle_ != nullptr && rx_handle_ != nullptr) {
            return true;
        }

        ESP_LOGI(TAG, "Audio IOs: mclk=%d bclk=%d ws=%d dout=%d din=%d",
                 static_cast<int>(kAudioMclkPin), static_cast<int>(kAudioBclkPin),
                 static_cast<int>(kAudioWsPin), static_cast<int>(kAudioDoutPin),
                 static_cast<int>(kAudioDinPin));

        i2s_chan_config_t chan_cfg = {
            .id = kAudioI2sPort,
            .role = I2S_ROLE_MASTER,
            .dma_desc_num = kDmaDescNum,
            .dma_frame_num = kDmaFrameNum,
            .auto_clear_after_cb = true,
            .auto_clear_before_cb = false,
            .allow_pd = false,
            .intr_priority = 0,
        };
        esp_err_t err = i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
            tx_handle_ = nullptr;
            rx_handle_ = nullptr;
            return false;
        }

        i2s_std_config_t std_cfg = {
            .clk_cfg = {
                .sample_rate_hz = static_cast<uint32_t>(kHwSampleRate),
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .ext_clk_freq_hz = 0,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                .bclk_div = 0,
            },
            .slot_cfg = {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = I2S_STD_SLOT_BOTH,
                .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false,
            },
            .gpio_cfg = {
                .mclk = kAudioMclkPin,
                .bclk = kAudioBclkPin,
                .ws = kAudioWsPin,
                .dout = kAudioDoutPin,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };

        i2s_tdm_config_t tdm_cfg = {
            .clk_cfg = {
                .sample_rate_hz = static_cast<uint32_t>(kHwSampleRate),
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .ext_clk_freq_hz = 0,
                .mclk_multiple = I2S_MCLK_MULTIPLE_256,
                .bclk_div = 8,
            },
            .slot_cfg = {
                .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_STEREO,
                .slot_mask = static_cast<i2s_tdm_slot_mask_t>(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                                               I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
                .ws_width = I2S_TDM_AUTO_WS_WIDTH,
                .ws_pol = false,
                .bit_shift = true,
                .left_align = false,
                .big_endian = false,
                .bit_order_lsb = false,
                .skip_mask = false,
                .total_slot = I2S_TDM_AUTO_SLOT_NUM,
            },
            .gpio_cfg = {
                .mclk = kAudioMclkPin,
                .bclk = kAudioBclkPin,
                .ws = kAudioWsPin,
                .dout = I2S_GPIO_UNUSED,
                .din = kAudioDinPin,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };

        err = i2s_channel_init_std_mode(tx_handle_, &std_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
            return false;
        }
        err = i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_init_tdm_mode failed: %s", esp_err_to_name(err));
            return false;
        }
        err = i2s_channel_enable(tx_handle_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_enable tx failed: %s", esp_err_to_name(err));
            return false;
        }
        err = i2s_channel_enable(rx_handle_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_enable rx failed: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "I2S0 duplex channels created");
        return true;
    }

    M5I2cCodecCtrl out_ctrl_ = {};
    M5I2cCodecCtrl in_ctrl_ = {};
    const audio_codec_data_if_t* data_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;
    int volume_percent_ = CONFIG_STACKCHAN_AUDIO_OUTPUT_VOLUME_DEFAULT;
    bool initialized_ = false;
    bool output_open_ = false;
    bool input_open_ = false;
};

class XiaopaiAudioService {
public:
    bool init()
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        if (initialized_) {
            return true;
        }
        play_queue_ = xQueueCreate(kPlayQueueDepth, sizeof(AudioBlock*));
        clean_queue_ = xQueueCreate(kCleanQueueDepth, sizeof(AudioBlock*));
        read_mutex_ = xSemaphoreCreateMutex();
        if (play_queue_ == nullptr || clean_queue_ == nullptr || read_mutex_ == nullptr) {
            ESP_LOGE(TAG, "failed to create audio queues");
            return false;
        }
        if (!codec_.init()) {
            ESP_LOGE(TAG, "codec init failed");
            return false;
        }
        if (!init_opus_decoder()) {
            ESP_LOGW(TAG, "Opus decoder unavailable; binary TTS playback will fail");
        }
        initialized_ = true;
        return true;
    }

    bool start()
    {
        if (!init()) {
            return false;
        }
        if (!codec_.start()) {
            ESP_LOGE(TAG, "codec start failed");
            return false;
        }
        if (running_) {
            return true;
        }
        running_ = true;
        abort_generation_++;

#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
        if (!afe_init_attempted_) {
            afe_init_attempted_ = true;
            afe_ready_ = init_afe();
        }
#endif

        if (input_task_ == nullptr) {
            xTaskCreatePinnedToCore(
                [](void* arg) {
                    static_cast<XiaopaiAudioService*>(arg)->input_task();
                    vTaskDelete(nullptr);
                },
                "xiaopai_audio_in", 6144, this, 4, &input_task_, 0);
        }
        if (output_task_ == nullptr) {
            xTaskCreate(
                [](void* arg) {
                    static_cast<XiaopaiAudioService*>(arg)->output_task();
                    vTaskDelete(nullptr);
                },
                "xiaopai_audio_out", 4096, this, 5, &output_task_);
        }
#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
        if (afe_ready_ && afe_task_ == nullptr) {
            xTaskCreate(
                [](void* arg) {
                    static_cast<XiaopaiAudioService*>(arg)->afe_fetch_task();
                    vTaskDelete(nullptr);
                },
                "xiaopai_afe_fetch", 6144, this, 4, &afe_task_);
        }
#endif
        ESP_LOGI(TAG, "audio service started: hw=%d protocol=%d afe=%d", kHwSampleRate, kProtocolSampleRate,
                 static_cast<int>(afe_ready_));
        return true;
    }

    void stop()
    {
        if (!running_) {
            return;
        }
        running_ = false;
        abort_playback();
        AudioBlock* sentinel = nullptr;
        if (play_queue_ != nullptr) {
            xQueueSend(play_queue_, &sentinel, 0);
        }
    }

    void set_volume(int percent)
    {
        codec_.set_volume(percent);
    }

    bool play_pcm_16k(const int16_t* samples, size_t count, AudioPlayOptions options)
    {
        if (samples == nullptr || count == 0) {
            return true;
        }
        if (!start() || play_queue_ == nullptr) {
            return false;
        }
        AudioBlock* block = allocate_block(count);
        if (block == nullptr) {
            ESP_LOGE(TAG, "failed to allocate playback block: samples=%u", static_cast<unsigned>(count));
            return false;
        }
        memcpy(block->data, samples, count * sizeof(int16_t));

        TickType_t wait_ticks = options.wait ? pdMS_TO_TICKS(1000) : 0;
        pending_play_blocks_++;
        while (xQueueSend(play_queue_, &block, wait_ticks) != pdTRUE) {
            if (!options.drop_oldest) {
                decrement_pending_play_blocks();
                free_block(block);
                return false;
            }
            AudioBlock* old = nullptr;
            if (xQueueReceive(play_queue_, &old, 0) == pdTRUE) {
                if (old != nullptr) {
                    decrement_pending_play_blocks();
                }
                free_block(old);
                ESP_LOGW(TAG, "playback queue full; dropped oldest PCM block");
                wait_ticks = 0;
                continue;
            }
            decrement_pending_play_blocks();
            free_block(block);
            return false;
        }
        return true;
    }

    bool play_opus_frame_16k(const uint8_t* data, size_t len)
    {
        if (data == nullptr || len == 0) {
            return true;
        }
        if (!start() || opus_decoder_ == nullptr) {
            return false;
        }

        std::vector<uint8_t> decode_bytes(kProtocolFrameSamples * sizeof(int16_t));
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = const_cast<uint8_t*>(data);
        raw.len = static_cast<uint32_t>(len);
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

        esp_audio_dec_out_frame_t out_frame = {};
        out_frame.buffer = decode_bytes.data();
        out_frame.len = decode_bytes.size();
        out_frame.needed_size = 0;
        out_frame.decoded_size = 0;
        esp_audio_dec_info_t info = {};

        esp_audio_err_t ret;
        {
            std::lock_guard<std::mutex> lock(opus_decoder_mutex_);
            ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &info);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH && out_frame.needed_size > out_frame.len) {
                decode_bytes.resize(out_frame.needed_size);
                out_frame.buffer = decode_bytes.data();
                out_frame.len = decode_bytes.size();
                out_frame.needed_size = 0;
                out_frame.decoded_size = 0;
                raw.buffer = const_cast<uint8_t*>(data);
                raw.len = static_cast<uint32_t>(len);
                raw.consumed = 0;
                ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &info);
            }
        }
        if (ret != ESP_AUDIO_ERR_OK || out_frame.decoded_size == 0) {
            ESP_LOGW(TAG, "Opus decode failed: ret=%d decoded=%u consumed=%u len=%u", ret,
                     static_cast<unsigned>(out_frame.decoded_size), static_cast<unsigned>(raw.consumed),
                     static_cast<unsigned>(len));
            return false;
        }
        return play_pcm_16k(reinterpret_cast<int16_t*>(out_frame.buffer), out_frame.decoded_size / sizeof(int16_t),
                            AudioPlayOptions{.wait = false, .drop_oldest = true});
    }

    size_t read_clean_16k(int16_t* out, size_t samples, TickType_t timeout)
    {
        if (out == nullptr || samples == 0 || clean_queue_ == nullptr) {
            return 0;
        }
        if (!start()) {
            return 0;
        }

        if (!clean_consumer_recent()) {
            drop_queued_clean_frames();
        }
        clean_reader_count_++;
        last_clean_read_ticks_ = xTaskGetTickCount();
        struct CleanReadGuard {
            XiaopaiAudioService* self;
            ~CleanReadGuard()
            {
                self->last_clean_read_ticks_ = xTaskGetTickCount();
                self->clean_reader_count_--;
            }
        } read_guard{this};

        if (xSemaphoreTake(read_mutex_, timeout == 0 ? 0 : portMAX_DELAY) != pdTRUE) {
            return 0;
        }

        size_t copied = 0;
        TickType_t start_ticks = xTaskGetTickCount();
        while (copied < samples) {
            if (read_block_ == nullptr) {
                TickType_t wait_ticks = remaining_timeout(start_ticks, timeout);
                if (xQueueReceive(clean_queue_, &read_block_, wait_ticks) != pdTRUE) {
                    break;
                }
                read_offset_ = 0;
                if (read_block_ == nullptr) {
                    continue;
                }
            }

            const size_t available = read_block_->samples - read_offset_;
            const size_t to_copy = std::min(samples - copied, available);
            memcpy(out + copied, read_block_->data + read_offset_, to_copy * sizeof(int16_t));
            copied += to_copy;
            read_offset_ += to_copy;
            if (read_offset_ >= read_block_->samples) {
                free_block(read_block_);
                read_block_ = nullptr;
                read_offset_ = 0;
            }
        }

        xSemaphoreGive(read_mutex_);
        return copied;
    }

    AudioVadState vad_state() const
    {
        return static_cast<AudioVadState>(vad_state_.load());
    }

    void abort_playback()
    {
        abort_generation_++;
        if (play_queue_ != nullptr) {
            AudioBlock* block = nullptr;
            while (xQueueReceive(play_queue_, &block, 0) == pdTRUE) {
                if (block != nullptr) {
                    decrement_pending_play_blocks();
                }
                free_block(block);
            }
        }
        playing_ = false;
        if (opus_decoder_ != nullptr) {
            std::lock_guard<std::mutex> lock(opus_decoder_mutex_);
            esp_opus_dec_reset(opus_decoder_);
        }
    }

    void dump_state()
    {
        codec_.dump_state();
        ESP_LOGI(TAG, "service: initialized=%d running=%d playing=%d play_q=%u clean_q=%u clean_readers=%u vad=%d afe=%d selected_ch=%d",
                 static_cast<int>(initialized_), static_cast<int>(running_.load()),
                 static_cast<int>(playing_.load()),
                 play_queue_ ? static_cast<unsigned>(uxQueueMessagesWaiting(play_queue_)) : 0,
                 clean_queue_ ? static_cast<unsigned>(uxQueueMessagesWaiting(clean_queue_)) : 0,
                 static_cast<unsigned>(clean_reader_count_.load()),
                 vad_state_.load(), static_cast<int>(afe_ready_), selected_input_channel_.load());
    }

    bool test_tone(int sample_rate, int tone_hz, int duration_ms, int volume_percent)
    {
        sample_rate = std::max(8000, sample_rate);
        tone_hz = std::max(50, std::min(tone_hz, sample_rate / 2 - 100));
        duration_ms = std::max(100, std::min(duration_ms, 10000));
        set_volume(volume_percent);

        const int total_samples = kProtocolSampleRate * duration_ms / 1000;
        const int amplitude = 9000;
        std::vector<int16_t> buffer(kToneChunkSamples);
        float phase = 0.0f;
        const float phase_step = kTwoPi * static_cast<float>(tone_hz) / static_cast<float>(kProtocolSampleRate);
        int generated = 0;
        while (generated < total_samples) {
            const int chunk = std::min<int>(buffer.size(), total_samples - generated);
            for (int i = 0; i < chunk; ++i) {
                buffer[i] = static_cast<int16_t>(std::sin(phase) * amplitude);
                phase += phase_step;
                if (phase >= kTwoPi) {
                    phase -= kTwoPi;
                }
            }
            if (!play_pcm_16k(buffer.data(), chunk, AudioPlayOptions{.wait = true, .drop_oldest = false})) {
                return false;
            }
            generated += chunk;
        }
        wait_playback_idle(pdMS_TO_TICKS(duration_ms + 1000));
        ESP_LOGI(TAG, "Audio test tone done: requested_rate=%d playback_rate=%d samples=%d",
                 sample_rate, kProtocolSampleRate, generated);
        return true;
    }

    bool available() const
    {
        return initialized_ && codec_.initialized();
    }

    bool is_playing() const
    {
        return playing_.load() || pending_play_blocks_.load() > 0 ||
               (play_queue_ != nullptr && uxQueueMessagesWaiting(play_queue_) > 0);
    }

    bool wait_playback_idle(TickType_t timeout)
    {
        TickType_t start_ticks = xTaskGetTickCount();
        while (is_playing()) {
            TickType_t wait_ticks = remaining_timeout(start_ticks, timeout);
            if (wait_ticks == 0) {
                return !is_playing();
            }
            vTaskDelay(std::min<TickType_t>(pdMS_TO_TICKS(10), wait_ticks));
        }
        vTaskDelay(pdMS_TO_TICKS(kTailDrainMs));
        return true;
    }

private:
    TickType_t remaining_timeout(TickType_t start_ticks, TickType_t timeout) const
    {
        if (timeout == portMAX_DELAY) {
            return portMAX_DELAY;
        }
        if (timeout == 0) {
            return 0;
        }
        TickType_t elapsed = xTaskGetTickCount() - start_ticks;
        if (elapsed >= timeout) {
            return 0;
        }
        return timeout - elapsed;
    }

    bool init_opus_decoder()
    {
        esp_opus_dec_cfg_t opus_cfg = {};
        opus_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
        opus_cfg.channel = ESP_AUDIO_MONO;
        opus_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        opus_cfg.self_delimited = false;
        esp_audio_err_t ret = esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &opus_decoder_);
        if (ret != ESP_AUDIO_ERR_OK || opus_decoder_ == nullptr) {
            ESP_LOGE(TAG, "esp_opus_dec_open failed: %d", ret);
            opus_decoder_ = nullptr;
            return false;
        }
        return true;
    }

#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
    bool init_afe()
    {
        if (!CONFIG_STACKCHAN_AUDIO_INPUT_REFERENCE) {
            ESP_LOGW(TAG, "device AEC requested but input reference is disabled");
            return false;
        }
        afe_config_t* afe_config = afe_config_init("MR", nullptr, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
        if (afe_config == nullptr) {
            ESP_LOGE(TAG, "afe_config_init failed");
            return false;
        }
        afe_config->aec_init = true;
        afe_config->aec_mode = AEC_MODE_VOIP_HIGH_PERF;
        afe_config->vad_init = true;
        afe_config->vad_model_name = nullptr;
        afe_config->vad_mode = VAD_MODE_0;
        afe_config->vad_min_noise_ms = 100;
        afe_config->vad_mute_playback = false;
        afe_config->ns_init = false;
        afe_config->agc_init = false;
        afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
        afe_iface_ = esp_afe_handle_from_config(afe_config);
        if (afe_iface_ == nullptr) {
            ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
            return false;
        }
        afe_data_ = afe_iface_->create_from_config(afe_config);
        if (afe_data_ == nullptr) {
            ESP_LOGE(TAG, "AFE create_from_config failed");
            return false;
        }
        afe_feed_samples_ = afe_iface_->get_feed_chunksize(afe_data_) * kInputChannels;
        ESP_LOGI(TAG, "AFE initialized: input=MR feed_samples=%u fetch_samples=%d",
                 static_cast<unsigned>(afe_feed_samples_), afe_iface_->get_fetch_chunksize(afe_data_));
        return true;
    }
#endif

    void input_task()
    {
        std::vector<int16_t> hw(kHwInputChunkSamples);
        std::vector<int16_t> in16;
        in16.reserve((kProtocolSampleRate / 100) * kInputChannels + kInputChannels);

        while (running_) {
            if (!codec_.read(hw.data(), hw.size())) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            resample_linear_interleaved(hw.data(), kHwInputChunkFrames, kInputChannels, kHwSampleRate, in16,
                                        kProtocolSampleRate);
#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
            if (afe_ready_) {
                feed_afe(in16);
            } else
#endif
            {
                push_mono_from_interleaved(in16.data(), in16.size() / kInputChannels, kInputChannels);
            }
        }
        input_task_ = nullptr;
        ESP_LOGW(TAG, "audio input task stopped");
    }

#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
    void feed_afe(const std::vector<int16_t>& input)
    {
        if (!afe_ready_ || afe_iface_ == nullptr || afe_data_ == nullptr || input.empty()) {
            return;
        }
        afe_feed_buffer_.insert(afe_feed_buffer_.end(), input.begin(), input.end());
        while (afe_feed_samples_ > 0 && afe_feed_buffer_.size() >= afe_feed_samples_) {
            afe_iface_->feed(afe_data_, afe_feed_buffer_.data());
            afe_feed_buffer_.erase(afe_feed_buffer_.begin(), afe_feed_buffer_.begin() + afe_feed_samples_);
        }
    }

    void afe_fetch_task()
    {
        std::vector<int16_t> out_buffer;
        while (running_) {
            auto* res = afe_iface_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
            if (!running_) {
                break;
            }
            if (res == nullptr || res->ret_value == ESP_FAIL) {
                continue;
            }
            const size_t samples = res->data_size / sizeof(int16_t);
            if (samples > 0 && res->data != nullptr) {
                push_clean_samples(res->data, samples);
                if (res->vad_state == VAD_SPEECH) {
                    vad_state_ = static_cast<int>(AudioVadState::kSpeech);
                } else if (res->vad_state == VAD_SILENCE) {
                    vad_state_ = static_cast<int>(AudioVadState::kSilence);
                } else {
                    update_energy_vad(res->data, samples);
                }
            }
        }
        afe_task_ = nullptr;
        ESP_LOGW(TAG, "AFE fetch task stopped");
    }
#endif

    void output_task()
    {
        std::vector<int16_t> out24;
        uint32_t local_generation = abort_generation_.load();
        while (true) {
            AudioBlock* block = nullptr;
            if (xQueueReceive(play_queue_, &block, portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (!running_ || block == nullptr) {
                if (block != nullptr) {
                    decrement_pending_play_blocks();
                }
                free_block(block);
                if (!running_) {
                    break;
                }
                continue;
            }

            playing_ = true;
            local_generation = abort_generation_.load();
            resample_linear_mono(block->data, block->samples, kProtocolSampleRate, out24, kHwSampleRate);
            uint32_t counter = ++playback_log_counter_;
            if ((counter % 12) == 1) {
                ESP_LOGI(TAG, "playback block: in_samples=%u out_samples=%u peak=%d pending=%u",
                         static_cast<unsigned>(block->samples), static_cast<unsigned>(out24.size()),
                         peak_abs_sample(out24.data(), out24.size()),
                         static_cast<unsigned>(pending_play_blocks_.load()));
            }
            const size_t chunk = static_cast<size_t>(kHwSampleRate / 50);
            size_t offset = 0;
            while (offset < out24.size() && running_ && local_generation == abort_generation_.load()) {
                const size_t to_write = std::min(chunk, out24.size() - offset);
                if (!codec_.write(out24.data() + offset, to_write)) {
                    break;
                }
                offset += to_write;
            }
            free_block(block);
            decrement_pending_play_blocks();
            playing_ = false;
        }
        output_task_ = nullptr;
        playing_ = false;
        ESP_LOGW(TAG, "audio output task stopped");
    }

    void push_mono_from_interleaved(const int16_t* input, size_t frames, int channels)
    {
        if (input == nullptr || frames == 0 || channels <= 0) {
            return;
        }
        int selected_channel = select_mono_channel(input, frames, channels);
        if (!clean_consumer_recent()) {
            update_energy_vad_interleaved(input, frames, channels, selected_channel);
            drop_queued_clean_frames();
            ++clean_idle_drop_count_;
            return;
        }
        AudioBlock* block = allocate_block(frames);
        if (block == nullptr) {
            return;
        }
        for (size_t i = 0; i < frames; ++i) {
            block->data[i] = apply_mic_magnification(input[i * channels + selected_channel]);
        }
        update_energy_vad(block->data, block->samples);
        push_clean_block(block);
    }

    void push_clean_samples(const int16_t* data, size_t samples)
    {
        if (!clean_consumer_recent()) {
            drop_queued_clean_frames();
            ++clean_idle_drop_count_;
            return;
        }
        AudioBlock* block = allocate_block(samples);
        if (block == nullptr) {
            return;
        }
        for (size_t i = 0; i < samples; ++i) {
            block->data[i] = apply_mic_magnification(data[i]);
        }
        push_clean_block(block);
    }

    void push_clean_block(AudioBlock* block)
    {
        if (block == nullptr || clean_queue_ == nullptr) {
            free_block(block);
            return;
        }
        if (!clean_consumer_recent()) {
            drop_queued_clean_frames();
            ++clean_idle_drop_count_;
            free_block(block);
            return;
        }
        while (xQueueSend(clean_queue_, &block, 0) != pdTRUE) {
            AudioBlock* old = nullptr;
            if (xQueueReceive(clean_queue_, &old, 0) == pdTRUE) {
                free_block(old);
                uint32_t dropped = ++clean_drop_count_;
                if ((dropped % 50) == 1) {
                    ESP_LOGW(TAG, "clean queue full; dropped oldest clean frame count=%u",
                             static_cast<unsigned>(dropped));
                }
                continue;
            }
            free_block(block);
            return;
        }
    }

    bool clean_consumer_recent() const
    {
        if (clean_reader_count_.load() > 0) {
            return true;
        }
        TickType_t last = last_clean_read_ticks_.load();
        return last != 0 && (xTaskGetTickCount() - last) <= kCleanQueueConsumerGraceTicks;
    }

    void drop_queued_clean_frames()
    {
        if (clean_queue_ == nullptr) {
            return;
        }
        AudioBlock* old = nullptr;
        while (xQueueReceive(clean_queue_, &old, 0) == pdTRUE) {
            free_block(old);
        }
    }

    void update_energy_vad(const int16_t* data, size_t samples)
    {
        if (data == nullptr || samples == 0) {
            return;
        }
        uint64_t sum = 0;
        for (size_t i = 0; i < samples; ++i) {
            sum += std::abs(static_cast<int>(data[i]));
        }
        int avg = static_cast<int>(sum / samples);
        const int start_threshold = 500;
        const int stop_threshold = 220;
        int current = vad_state_.load();
        if (avg >= start_threshold && current != static_cast<int>(AudioVadState::kSpeech)) {
            vad_state_ = static_cast<int>(AudioVadState::kSpeech);
        } else if (avg <= stop_threshold && current != static_cast<int>(AudioVadState::kSilence)) {
            vad_state_ = static_cast<int>(AudioVadState::kSilence);
        }
    }

    int select_mono_channel(const int16_t* data, size_t frames, int channels)
    {
        if (data == nullptr || frames == 0 || channels <= 1) {
            selected_input_channel_ = 0;
            return 0;
        }

        static constexpr int kMaxLoggedChannels = 4;
        const int measured_channels = std::min(channels, kMaxLoggedChannels);
        uint64_t sums[kMaxLoggedChannels] = {};
        for (size_t i = 0; i < frames; ++i) {
            for (int ch = 0; ch < measured_channels; ++ch) {
                sums[ch] += std::abs(static_cast<int>(data[i * channels + ch]));
            }
        }

        int best_channel = 0;
        uint64_t best_sum = sums[0];
        for (int ch = 1; ch < measured_channels; ++ch) {
            if (sums[ch] > best_sum) {
                best_sum = sums[ch];
                best_channel = ch;
            }
        }
        int selected_channel = best_channel;
        int current_channel = selected_input_channel_.load();
        if (current_channel >= 0 && current_channel < measured_channels &&
            sums[current_channel] > 0 && sums[current_channel] * 4 >= best_sum * 3) {
            selected_channel = current_channel;
        }
        selected_input_channel_ = selected_channel;

        uint32_t counter = ++input_level_log_counter_;
        if ((counter % 100) == 1) {
            ESP_LOGI(TAG, "input levels avg: selected=%d best=%d mag=%d ch0=%u ch1=%u ch2=%u ch3=%u",
                     selected_channel, best_channel, CONFIG_STACKCHAN_MIC_MAGNIFICATION,
                     measured_channels > 0 ? static_cast<unsigned>(sums[0] / frames) : 0,
                     measured_channels > 1 ? static_cast<unsigned>(sums[1] / frames) : 0,
                     measured_channels > 2 ? static_cast<unsigned>(sums[2] / frames) : 0,
                     measured_channels > 3 ? static_cast<unsigned>(sums[3] / frames) : 0);
        }
        return selected_channel;
    }

    void update_energy_vad_interleaved(const int16_t* data, size_t frames, int channels, int selected_channel)
    {
        if (data == nullptr || frames == 0 || channels <= 0) {
            return;
        }
        selected_channel = std::max(0, std::min(selected_channel, channels - 1));
        uint64_t sum = 0;
        for (size_t i = 0; i < frames; ++i) {
            sum += std::abs(static_cast<int>(data[i * channels + selected_channel]));
        }
        int avg = static_cast<int>(sum / frames);
        const int start_threshold = 500;
        const int stop_threshold = 220;
        int current = vad_state_.load();
        if (avg >= start_threshold && current != static_cast<int>(AudioVadState::kSpeech)) {
            vad_state_ = static_cast<int>(AudioVadState::kSpeech);
        } else if (avg <= stop_threshold && current != static_cast<int>(AudioVadState::kSilence)) {
            vad_state_ = static_cast<int>(AudioVadState::kSilence);
        }
    }

    void decrement_pending_play_blocks()
    {
        uint32_t current = pending_play_blocks_.load();
        while (current > 0 &&
               !pending_play_blocks_.compare_exchange_weak(current, current - 1)) {
        }
    }

    XiaopaiAudioCodec codec_;
    std::mutex init_mutex_;
    QueueHandle_t play_queue_ = nullptr;
    QueueHandle_t clean_queue_ = nullptr;
    SemaphoreHandle_t read_mutex_ = nullptr;
    AudioBlock* read_block_ = nullptr;
    size_t read_offset_ = 0;
    TaskHandle_t input_task_ = nullptr;
    TaskHandle_t output_task_ = nullptr;
    void* opus_decoder_ = nullptr;
    std::mutex opus_decoder_mutex_;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false};
    std::atomic<uint32_t> pending_play_blocks_{0};
    std::atomic<uint32_t> abort_generation_{0};
    std::atomic<int> vad_state_{static_cast<int>(AudioVadState::kUnknown)};
    std::atomic<uint32_t> clean_reader_count_{0};
    std::atomic<TickType_t> last_clean_read_ticks_{0};
    std::atomic<uint32_t> clean_drop_count_{0};
    std::atomic<uint32_t> clean_idle_drop_count_{0};
    std::atomic<uint32_t> input_level_log_counter_{0};
    std::atomic<uint32_t> playback_log_counter_{0};
    std::atomic<int> selected_input_channel_{0};
    bool afe_init_attempted_ = false;
    bool afe_ready_ = false;

#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
    const esp_afe_sr_iface_t* afe_iface_ = nullptr;
    esp_afe_sr_data_t* afe_data_ = nullptr;
    TaskHandle_t afe_task_ = nullptr;
    std::vector<int16_t> afe_feed_buffer_;
    size_t afe_feed_samples_ = 0;
#endif
};

XiaopaiAudioService g_audio_service;

}  // namespace

bool audio_service_init()
{
    return g_audio_service.init();
}

bool audio_service_start()
{
    return g_audio_service.start();
}

void audio_service_stop()
{
    g_audio_service.stop();
}

void audio_service_set_volume_percent(int percent)
{
    g_audio_service.set_volume(percent);
}

bool audio_service_play_pcm_16k(const int16_t* samples, size_t count, AudioPlayOptions options)
{
    return g_audio_service.play_pcm_16k(samples, count, options);
}

bool audio_service_play_opus_frame_16k(const uint8_t* data, size_t len)
{
    return g_audio_service.play_opus_frame_16k(data, len);
}

size_t audio_service_read_clean_16k(int16_t* out, size_t samples, TickType_t timeout)
{
    return g_audio_service.read_clean_16k(out, samples, timeout);
}

AudioVadState audio_service_get_vad_state()
{
    return g_audio_service.vad_state();
}

void audio_service_abort_playback()
{
    g_audio_service.abort_playback();
}

void audio_service_dump_state()
{
    g_audio_service.dump_state();
}

bool audio_service_test_tone(int sample_rate, int tone_hz, int duration_ms, int volume_percent)
{
    return g_audio_service.test_tone(sample_rate, tone_hz, duration_ms, volume_percent);
}

bool audio_service_is_available()
{
    return g_audio_service.available();
}

bool audio_service_is_playing()
{
    return g_audio_service.is_playing();
}

bool audio_service_wait_playback_idle(TickType_t timeout)
{
    return g_audio_service.wait_playback_idle(timeout);
}
