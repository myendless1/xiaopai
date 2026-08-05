#include "xiaopai_audio_service.h"

#include <M5Unified.h>

#include "dji_mic_receiver_input.h"
#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "aw88298_dac.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "es7210_adc.h"
#include "esp_audio_dec.h"
#include "esp_audio_types.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_opus_dec.h"
#include "sdkconfig.h"
#include "xiaopai_state.h"

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
#ifndef CONFIG_STACKCHAN_AUDIO_TX_ENABLED
#define CONFIG_STACKCHAN_AUDIO_TX_ENABLED 1
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
#define CONFIG_STACKCHAN_AUDIO_DEVICE_AEC 0
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_HW_SAMPLE_RATE
#define CONFIG_STACKCHAN_AUDIO_HW_SAMPLE_RATE 24000
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_UPSTREAM_SAMPLE_RATE
#define CONFIG_STACKCHAN_AUDIO_UPSTREAM_SAMPLE_RATE 16000
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_DOWNSTREAM_SAMPLE_RATE
#define CONFIG_STACKCHAN_AUDIO_DOWNSTREAM_SAMPLE_RATE 24000
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_INPUT_REFERENCE
#define CONFIG_STACKCHAN_AUDIO_INPUT_REFERENCE 0
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_INPUT_GAIN
#define CONFIG_STACKCHAN_AUDIO_INPUT_GAIN 60
#endif
#ifndef CONFIG_STACKCHAN_AUDIO_OUTPUT_VOLUME_DEFAULT
#define CONFIG_STACKCHAN_AUDIO_OUTPUT_VOLUME_DEFAULT 10
#endif
#ifndef CONFIG_STACKCHAN_MIC_MAGNIFICATION
#define CONFIG_STACKCHAN_MIC_MAGNIFICATION 1
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_USB_INPUT
#define CONFIG_STACKCHAN_DJI_MIC_USB_INPUT 0
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_AUTO_START
#define CONFIG_STACKCHAN_DJI_MIC_AUTO_START 0
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_START_DELAY_MS
#define CONFIG_STACKCHAN_DJI_MIC_START_DELAY_MS 5000
#endif

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

namespace {

class XiaopaiAudioService;
static XiaopaiAudioService* g_audio_service_ptr = nullptr;

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
static constexpr i2c_port_t kCoreS3InternalI2cPort = I2C_NUM_1;
static constexpr int kCoreS3InternalI2cSda = 12;
static constexpr int kCoreS3InternalI2cScl = 11;
static constexpr int kHwSampleRate = CONFIG_STACKCHAN_AUDIO_HW_SAMPLE_RATE;
static constexpr int kUpstreamSampleRate = CONFIG_STACKCHAN_AUDIO_UPSTREAM_SAMPLE_RATE;
static constexpr int kDownstreamSampleRate = CONFIG_STACKCHAN_AUDIO_DOWNSTREAM_SAMPLE_RATE;
static_assert(kHwSampleRate == kDownstreamSampleRate,
              "Direct PCM playback requires matching hardware and downstream sample rates");
static_assert(kUpstreamSampleRate == 16000, "The realtime Opus encoder and Aliyun ASR require 16 kHz upstream PCM");
static_assert(kDownstreamSampleRate == 24000, "The realtime TTS Opus decoder requires 24 kHz downstream PCM");
static constexpr int kOpusFrameDurationMs = 60;
static constexpr int kDownstreamFrameSamples = kDownstreamSampleRate * kOpusFrameDurationMs / 1000;
static constexpr bool kDeviceAecEnabled = CONFIG_STACKCHAN_AUDIO_DEVICE_AEC && CONFIG_STACKCHAN_AUDIO_INPUT_REFERENCE;
static constexpr int kInputChannels = 2;
static constexpr uint16_t kRawInputChannelMask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                                                 ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1);
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
static constexpr int kDjiInputStartupTimeoutMs = CONFIG_STACKCHAN_DJI_MIC_START_DELAY_MS;
static constexpr int kDjiPriorityWaitLogIntervalMs = 3000;
static constexpr int kInputUnavailableLogIntervalMs = 3000;
static constexpr int kToneChunkSamples = 512;
static constexpr int kTailDrainMs = 80;
static constexpr float kTwoPi = 6.28318530717958647692f;
static constexpr uint8_t kAw9523SpeakerPowerMask = 0b00000100;
static constexpr uint8_t kAw9523BoostPowerMask = 0b10000000;

struct AudioBlock {
    size_t samples = 0;
    bool in_use = false;
    int16_t data[1];
};

struct M5I2cCodecCtrl {
    audio_codec_ctrl_if_t base;
    bool open = false;
    uint8_t addr = 0;
};

struct AudioI2cPresence {
    bool aw9523 = false;
    bool aw88298 = false;
    bool es7210 = false;
    bool axp2101 = false;

    bool any() const
    {
        return aw9523 || aw88298 || es7210 || axp2101;
    }
};

static bool ensure_core_s3_internal_i2c_ready(bool force_rebuild)
{
    if (!force_rebuild) {
        i2c_master_bus_handle_t bus_handle = nullptr;
        esp_err_t err = i2c_master_get_bus_handle(kCoreS3InternalI2cPort, &bus_handle);
        if (err == ESP_OK && bus_handle != nullptr) {
            return true;
        }
        ESP_LOGW(TAG, "CoreS3 internal I2C bus missing before audio init: %s; rebuilding",
                 esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "CoreS3 internal I2C scan returned no devices; rebuilding bus");
    }

    if (!M5.In_I2C.begin(kCoreS3InternalI2cPort, kCoreS3InternalI2cSda, kCoreS3InternalI2cScl)) {
        ESP_LOGE(TAG, "M5.In_I2C.begin failed before audio init");
        return false;
    }

    i2c_master_bus_handle_t bus_handle = nullptr;
    esp_err_t err = i2c_master_get_bus_handle(kCoreS3InternalI2cPort, &bus_handle);
    if (err != ESP_OK || bus_handle == nullptr) {
        ESP_LOGE(TAG, "CoreS3 internal I2C unavailable after audio rebuild: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static int clamp_volume_percent(int percent)
{
    return std::max(0, std::min(100, percent));
}

struct AudioBlockPool {
    AudioBlock** blocks = nullptr;
    QueueHandle_t free_queue = nullptr;
    size_t block_capacity = 0;
    size_t num_blocks = 0;
    std::atomic<uint32_t> allocation_count{0};
    std::atomic<uint32_t> exhaustion_count{0};
};

static AudioBlock* allocate_block(size_t samples, bool is_play = false);
static void free_block(AudioBlock* block);

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

static int16_t apply_mic_magnification(int16_t sample)
{
    if (CONFIG_STACKCHAN_MIC_MAGNIFICATION <= 1) {
        return sample;
    }
    int32_t amplified = static_cast<int32_t>(sample) * CONFIG_STACKCHAN_MIC_MAGNIFICATION;
    return static_cast<int16_t>(std::max<int32_t>(-32768, std::min<int32_t>(32767, amplified)));
}

static const char* audio_input_source_name_impl(AudioInputSource source)
{
    switch (source) {
        case AudioInputSource::kDjiMicReceiver:
            return "dji_mic_receiver";
        case AudioInputSource::kInternalMic:
        default:
            return "internal_mic";
    }
}

static const char* audio_input_source_label_impl(AudioInputSource source)
{
    switch (source) {
        case AudioInputSource::kDjiMicReceiver:
            return "DJI Mic接收器";
        case AudioInputSource::kInternalMic:
        default:
            return "内置麦克风";
    }
}

static bool dji_receiver_should_own_input(const DjiMicReceiverStatus& dji)
{
#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT
    return dji.capture_ready && dji.identity_confirmed;
#else
    (void)dji;
    return false;
#endif
}

static void restore_aw88298_playback_registers(int sample_rate)
{
    (void)sample_rate;
    write_aw88298_reg_direct(0x61, 0x0673, "AW88298 restore boost");
    write_aw88298_reg_direct(0x04, 0x4040, "AW88298 restore sysctrl");
    write_aw88298_reg_direct(0x05, 0x0008, "AW88298 restore unmute");
}

static AudioI2cPresence scan_core_s3_audio_i2c()
{
    AudioI2cPresence presence;
    presence.aw9523 = M5.In_I2C.scanID(kAw9523Addr, kInternalI2cFreq);
    presence.aw88298 = M5.In_I2C.scanID(kAw88298Addr >> 1, kInternalI2cFreq);
    presence.es7210 = M5.In_I2C.scanID(kEs7210Addr >> 1, kInternalI2cFreq);
    presence.axp2101 = M5.In_I2C.scanID(kAxp2101Addr, kInternalI2cFreq);
    ESP_LOGI(TAG, "Audio I2C scan: AW9523=%d AW88298=%d ES7210=%d AXP2101=%d",
             presence.aw9523, presence.aw88298, presence.es7210, presence.axp2101);
    return presence;
}

static AudioI2cPresence scan_core_s3_audio_i2c_with_recovery()
{
    ensure_core_s3_internal_i2c_ready(false);
    AudioI2cPresence presence = scan_core_s3_audio_i2c();
    if (presence.any()) {
        return presence;
    }

    if (ensure_core_s3_internal_i2c_ready(true)) {
        vTaskDelay(pdMS_TO_TICKS(20));
        presence = scan_core_s3_audio_i2c();
    }
    return presence;
}

static void configure_core_s3_audio_power()
{
    AudioI2cPresence presence = scan_core_s3_audio_i2c_with_recovery();

    if (presence.axp2101) {
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

    if (presence.aw9523) {
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
        ensure_core_s3_internal_i2c_ready(true);
        vTaskDelay(pdMS_TO_TICKS(20));
        if (!M5.In_I2C.scanID(kAw9523Addr, kInternalI2cFreq)) {
            ESP_LOGW(TAG, "AW9523 not found; skip AW88298 reset");
            return;
        }
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

static void resample_linear_interleaved(const int16_t* in, size_t in_frames, int channels, int src_rate,
                                        std::vector<int16_t>& out, int dst_rate)
{
    if (in == nullptr || in_frames == 0 || channels <= 0 || src_rate <= 0 || dst_rate <= 0) {
        out.clear();
        return;
    }
    if (src_rate == dst_rate) {
        out.assign(in, in + in_frames * channels);
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
        return init_output() && init_input();
    }

    bool start()
    {
        return start_output() && start_input();
    }

    bool start_output()
    {
        return init_output() && open_output();
    }

    bool start_input()
    {
        return init_input() && open_input();
    }

    bool init_output()
    {
        if (output_initialized_) {
            return true;
        }

        configure_core_s3_audio_power();
        reset_core_s3_aw88298();
        init_m5_i2c_ctrl(&out_ctrl_, kAw88298Addr);

        if (!create_tx_channel()) {
            return false;
        }

        audio_codec_i2s_cfg_t i2s_cfg = {
            .port = static_cast<uint8_t>(kAudioI2sPort),
            .rx_handle = nullptr,
            .tx_handle = tx_handle_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
        };
        output_data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
        if (output_data_if_ == nullptr) {
            ESP_LOGE(TAG, "audio_codec_new_i2s_data output failed");
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
            .data_if = output_data_if_,
        };
        output_dev_ = esp_codec_dev_new(&out_dev_cfg);
        if (output_dev_ == nullptr) {
            ESP_LOGE(TAG, "esp_codec_dev_new output failed");
            return false;
        }

        output_initialized_ = true;
        ESP_LOGI(TAG, "CoreS3 speaker output initialized: hw_rate=%d", kHwSampleRate);
        return true;
    }

    bool init_input()
    {
        if (input_initialized_) {
            return true;
        }

        configure_core_s3_audio_power();
        init_m5_i2c_ctrl(&in_ctrl_, kEs7210Addr);

        if (!create_rx_channel()) {
            return false;
        }

        audio_codec_i2s_cfg_t i2s_cfg = {
            .port = static_cast<uint8_t>(kAudioI2sPort),
            .rx_handle = rx_handle_,
            .tx_handle = nullptr,
            .clk_src = I2S_CLK_SRC_DEFAULT,
        };
        input_data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
        if (input_data_if_ == nullptr) {
            ESP_LOGE(TAG, "audio_codec_new_i2s_data input failed");
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
            .data_if = input_data_if_,
        };
        input_dev_ = esp_codec_dev_new(&in_dev_cfg);
        if (input_dev_ == nullptr) {
            ESP_LOGE(TAG, "esp_codec_dev_new input failed");
            return false;
        }

        input_initialized_ = true;
        ESP_LOGI(TAG, "CoreS3 ES7210 input initialized late: hw_rate=%d channels=%d reference=%d device_aec=%d mic_mag=%d",
                 kHwSampleRate, kInputChannels, CONFIG_STACKCHAN_AUDIO_INPUT_REFERENCE,
                 CONFIG_STACKCHAN_AUDIO_DEVICE_AEC, CONFIG_STACKCHAN_MIC_MAGNIFICATION);
        return true;
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
                 static_cast<int>(output_initialized_ && input_initialized_),
                 input_open_, output_open_, volume_percent_);
    }

    bool output_initialized() const { return output_initialized_; }
    bool input_initialized() const { return input_initialized_; }

private:
    bool create_tx_channel()
    {
        if (tx_handle_ != nullptr) {
            return true;
        }

        ESP_LOGI(TAG, "Audio TX IOs: mclk=%d bclk=%d ws=%d dout=%d",
                 static_cast<int>(kAudioMclkPin), static_cast<int>(kAudioBclkPin),
                 static_cast<int>(kAudioWsPin), static_cast<int>(kAudioDoutPin));

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
            ESP_LOGE(TAG, "i2s_new_channel duplex failed: %s", esp_err_to_name(err));
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

        err = i2s_channel_init_std_mode(tx_handle_, &std_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_init_std_mode tx failed: %s", esp_err_to_name(err));
            return false;
        }
        // CoreS3 shares MCLK/BCLK/WS between its standard TX and TDM RX.
        // Initialize the paired RX before enabling TX, matching the upstream board driver.
        if (!create_rx_channel()) {
            return false;
        }
        err = i2s_channel_enable(tx_handle_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_enable tx failed: %s", esp_err_to_name(err));
            return false;
        }
        ESP_LOGI(TAG, "I2S0 duplex channels created with shared speaker/microphone clock");
        return true;
    }

    bool create_rx_channel()
    {
        if (rx_channel_initialized_) {
            return true;
        }

        ESP_LOGI(TAG, "Audio RX IOs: mclk=%d bclk=%d ws=%d din=%d",
                 static_cast<int>(kAudioMclkPin), static_cast<int>(kAudioBclkPin),
                 static_cast<int>(kAudioWsPin), static_cast<int>(kAudioDinPin));

        esp_err_t err = ESP_OK;
        if (rx_handle_ == nullptr) {
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
            err = i2s_new_channel(&chan_cfg, nullptr, &rx_handle_);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "i2s_new_channel rx failed: %s", esp_err_to_name(err));
                rx_handle_ = nullptr;
                return false;
            }
        }

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

        err = i2s_channel_init_tdm_mode(rx_handle_, &tdm_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_init_tdm_mode rx failed: %s", esp_err_to_name(err));
            return false;
        }
        err = i2s_channel_enable(rx_handle_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s_channel_enable rx failed: %s", esp_err_to_name(err));
            return false;
        }
        rx_channel_initialized_ = true;
        ESP_LOGI(TAG, "I2S0 RX channel initialized for ES7210 input");
        return true;
    }

    M5I2cCodecCtrl out_ctrl_ = {};
    M5I2cCodecCtrl in_ctrl_ = {};
    const audio_codec_data_if_t* output_data_if_ = nullptr;
    const audio_codec_data_if_t* input_data_if_ = nullptr;
    const audio_codec_gpio_if_t* gpio_if_ = nullptr;
    const audio_codec_if_t* out_codec_if_ = nullptr;
    const audio_codec_if_t* in_codec_if_ = nullptr;
    esp_codec_dev_handle_t output_dev_ = nullptr;
    esp_codec_dev_handle_t input_dev_ = nullptr;
    i2s_chan_handle_t tx_handle_ = nullptr;
    i2s_chan_handle_t rx_handle_ = nullptr;
    bool rx_channel_initialized_ = false;
    int volume_percent_ = CONFIG_STACKCHAN_AUDIO_OUTPUT_VOLUME_DEFAULT;
    bool output_initialized_ = false;
    bool input_initialized_ = false;
    bool output_open_ = false;
    bool input_open_ = false;
};

class XiaopaiAudioService {
public:
    XiaopaiAudioService()
    {
        g_audio_service_ptr = this;
    }

    bool init_block_pools()
    {
        if (play_pool_.free_queue != nullptr) {
            return true;
        }

        // One block must hold a complete 60 ms downstream Opus frame (1440 samples at 24 kHz).
        play_pool_.block_capacity = kDownstreamFrameSamples;
        play_pool_.num_blocks = 12;
        play_pool_.free_queue = xQueueCreate(play_pool_.num_blocks, sizeof(AudioBlock*));
        if (play_pool_.free_queue == nullptr) {
            return false;
        }

        const size_t play_block_bytes = sizeof(AudioBlock) + (play_pool_.block_capacity - 1) * sizeof(int16_t);
        play_pool_.blocks = static_cast<AudioBlock**>(heap_caps_malloc(play_pool_.num_blocks * sizeof(AudioBlock*), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (play_pool_.blocks == nullptr) {
            return false;
        }

        for (size_t i = 0; i < play_pool_.num_blocks; ++i) {
            void* mem = heap_caps_malloc(play_block_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (mem == nullptr) {
                return false;
            }
            auto* block = static_cast<AudioBlock*>(mem);
            block->samples = 0;
            block->in_use = false;
            play_pool_.blocks[i] = block;
            xQueueSend(play_pool_.free_queue, &block, 0);
        }

        // 2. Recording Pool: 36 blocks, 640 samples each
        rec_pool_.block_capacity = 640;
        rec_pool_.num_blocks = 36;
        rec_pool_.free_queue = xQueueCreate(rec_pool_.num_blocks, sizeof(AudioBlock*));
        if (rec_pool_.free_queue == nullptr) {
            return false;
        }

        const size_t rec_block_bytes = sizeof(AudioBlock) + (rec_pool_.block_capacity - 1) * sizeof(int16_t);
        rec_pool_.blocks = static_cast<AudioBlock**>(heap_caps_malloc(rec_pool_.num_blocks * sizeof(AudioBlock*), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (rec_pool_.blocks == nullptr) {
            return false;
        }

        for (size_t i = 0; i < rec_pool_.num_blocks; ++i) {
            void* mem = heap_caps_malloc(rec_block_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (mem == nullptr) {
                return false;
            }
            auto* block = static_cast<AudioBlock*>(mem);
            block->samples = 0;
            block->in_use = false;
            rec_pool_.blocks[i] = block;
            xQueueSend(rec_pool_.free_queue, &block, 0);
        }

        return true;
    }

    AudioBlock* lease_block(size_t samples, bool is_play)
    {
        AudioBlockPool& pool = is_play ? play_pool_ : rec_pool_;
        if (samples > pool.block_capacity) {
            ESP_LOGE(TAG, "Requested block size %u exceeds pool capacity %u", samples, pool.block_capacity);
            return nullptr;
        }

        AudioBlock* block = nullptr;
        if (xQueueReceive(pool.free_queue, &block, 0) == pdTRUE) {
            block->samples = samples;
            block->in_use = true;
            pool.allocation_count++;
            return block;
        }

        pool.exhaustion_count++;
        return nullptr;
    }

    void release_block(AudioBlock* block)
    {
        if (block == nullptr) {
            return;
        }

        bool returned = false;
        for (size_t i = 0; i < play_pool_.num_blocks; ++i) {
            if (play_pool_.blocks[i] == block) {
                if (!block->in_use) {
                    ESP_LOGE(TAG, "Double-return detected on play block %p!", block);
                    return;
                }
                block->in_use = false;
                xQueueSend(play_pool_.free_queue, &block, 0);
                returned = true;
                break;
            }
        }

        if (!returned) {
            for (size_t i = 0; i < rec_pool_.num_blocks; ++i) {
                if (rec_pool_.blocks[i] == block) {
                    if (!block->in_use) {
                        ESP_LOGE(TAG, "Double-return detected on rec block %p!", block);
                        return;
                    }
                    block->in_use = false;
                    xQueueSend(rec_pool_.free_queue, &block, 0);
                    returned = true;
                    break;
                }
            }
        }

        if (!returned) {
            ESP_LOGE(TAG, "Attempted to release block %p that does not belong to any pool!", block);
        }
    }

    bool init()
    {
        std::lock_guard<std::mutex> lock(init_mutex_);
        if (initialized_) {
            return true;
        }
        if (!init_block_pools()) {
            ESP_LOGE(TAG, "failed to initialize audio block pools");
            return false;
        }
        play_queue_ = xQueueCreate(kPlayQueueDepth, sizeof(AudioBlock*));
        clean_queue_ = xQueueCreate(kCleanQueueDepth, sizeof(AudioBlock*));
        read_mutex_ = xSemaphoreCreateMutex();
        if (play_queue_ == nullptr || clean_queue_ == nullptr || read_mutex_ == nullptr) {
            ESP_LOGE(TAG, "failed to create audio queues");
            return false;
        }
#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT
        active_input_source_ = static_cast<int>(AudioInputSource::kInternalMic);
        vad_state_ = static_cast<int>(AudioVadState::kUnknown);

#if CONFIG_STACKCHAN_DJI_MIC_AUTO_START
        ESP_LOGI(TAG, "DJI Mic auto-start enabled");
        dji_start_timeout_logged_ = false;
        dji_start_wait_ticks_ = xTaskGetTickCount();
        if (!dji_mic_receiver_input_start()) {
            ESP_LOGE(TAG, "DJI Mic receiver start failed: %s",
                     dji_mic_receiver_input_status().detail);
            dji_start_wait_ticks_ = 0;
        } else {
            ESP_LOGI(TAG, "DJI Mic接收器输入门控已启用: 收到首帧UAC PCM后切换输入源");
        }
#else
        ESP_LOGW(TAG, "DJI Mic USB input compiled but not auto-started");
#endif

        if (!ensure_output_started("audio service init")) {
            ESP_LOGW(TAG, "speaker output init failed; playback unavailable until retry");
        }
#if !CONFIG_STACKCHAN_DJI_MIC_AUTO_START
        if (!ensure_internal_input_started("audio service init without DJI auto-start")) {
            ESP_LOGW(TAG, "internal input init failed; microphone unavailable until retry");
        }
#endif
#else
        if (!ensure_output_started("audio service init")) {
            ESP_LOGE(TAG, "speaker output init failed");
        }
        if (!ensure_internal_input_started("audio service init")) {
            ESP_LOGE(TAG, "internal input init failed");
        }
#endif
        if (!init_opus_decoder()) {
            ESP_LOGW(TAG, "Opus decoder unavailable; binary TTS playback will fail");
        }
        initialized_ = true;
        return true;
    }

    bool start()
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!init()) {
            return false;
        }
        DjiMicReceiverStatus dji = dji_mic_receiver_input_status();
        ensure_output_started("audio service start");
        if (!dji_receiver_should_own_input(dji) && !dji_receiver_startup_waiting(dji)) {
            ensure_internal_input_started("audio service start fallback");
        } else if (dji_receiver_startup_waiting(dji) && last_input_unavailable_log_ticks_ == 0) {
            ESP_LOGI(TAG, "ES7210输入初始化已延后: 等待DJI Mic首帧UAC PCM，避免USB枚举期间并行采集");
            last_input_unavailable_log_ticks_ = xTaskGetTickCount();
        }
        if (running_) {
            return true;
        }
        running_ = true;
        abort_generation_++;

        BaseType_t input_ok = pdPASS;
        TaskHandle_t temp_input = nullptr;
        if (input_task_.load() == nullptr) {
            input_ok = xTaskCreatePinnedToCore(
                [](void* arg) {
                    static_cast<XiaopaiAudioService*>(arg)->input_task();
                    vTaskDelete(nullptr);
                },
                "audio_input_task", 6144, this, 6, &temp_input, 1);
            if (input_ok == pdPASS) {
                input_task_.store(temp_input);
            }
        }

        BaseType_t output_ok = pdPASS;
        TaskHandle_t temp_output = nullptr;
        if (output_task_.load() == nullptr) {
            output_ok = xTaskCreate(
                [](void* arg) {
                    static_cast<XiaopaiAudioService*>(arg)->output_task();
                    vTaskDelete(nullptr);
                },
                "audio_output_hw", 4096, this, 5, &temp_output);
            if (output_ok == pdPASS) {
                output_task_.store(temp_output);
            }
        }

        if (input_ok != pdPASS || output_ok != pdPASS) {
            ESP_LOGE(TAG, "failed to create audio tasks: input_ok=%d output_ok=%d, rolling back...",
                     static_cast<int>(input_ok), static_cast<int>(output_ok));
            running_ = false;
            abort_generation_++;
            if (output_task_.load() != nullptr && play_queue_ != nullptr) {
                AudioBlock* sentinel = nullptr;
                xQueueSend(play_queue_, &sentinel, 0);
            }
            TickType_t start_ticks = xTaskGetTickCount();
            while ((input_task_.load() != nullptr || output_task_.load() != nullptr) &&
                   (xTaskGetTickCount() - start_ticks) < pdMS_TO_TICKS(500)) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (input_task_.load() != nullptr || output_task_.load() != nullptr) {
                ESP_LOGE(TAG, "Audio task cleanup timeout during start failure rollback!");
            }
            return false;
        }

#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
        ensure_afe_started("audio service start");
        ensure_afe_fetch_task_started();
#endif
        ESP_LOGI(TAG, "audio service started: hw=%d upstream=%d downstream=%d afe=%d",
                 kHwSampleRate, kUpstreamSampleRate, kDownstreamSampleRate,
                 static_cast<int>(afe_ready_));
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!running_) {
            return;
        }
        running_ = false;
        abort_playback();
        AudioBlock* sentinel = nullptr;
        if (play_queue_ != nullptr) {
            xQueueSend(play_queue_, &sentinel, 0);
        }
        TickType_t start_ticks = xTaskGetTickCount();
        while ((input_task_.load() != nullptr || output_task_.load() != nullptr) &&
               (xTaskGetTickCount() - start_ticks) < pdMS_TO_TICKS(500)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (input_task_.load() != nullptr || output_task_.load() != nullptr) {
            ESP_LOGE(TAG, "Audio task cleanup timeout during stop!");
        }
    }

    void log_diagnostics();

    void register_heartbeat_task(TaskHandle_t handle)
    {
        heartbeat_task_handle_.store(handle);
    }

    void set_volume(int percent)
    {
        codec_.set_volume(percent);
    }

    bool play_pcm_24k(const int16_t* samples, size_t count, AudioPlayOptions options)
    {
        if (samples == nullptr || count == 0) {
            return true;
        }
        if (!start() || play_queue_ == nullptr) {
            return false;
        }
        if (!ensure_output_started("playback request")) {
            ESP_LOGW(TAG, "播放请求已拒绝: 扬声器输出尚不可用");
            return false;
        }
        if (!codec_output_started_.load()) {
            ESP_LOGW(TAG, "播放请求已拒绝: 内部扬声器输出不可用");
            return false;
        }
        AudioBlock* block = allocate_block(count, true);
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
                play_dropped_count_++;
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

    bool play_opus_frame_24k(const uint8_t* data, size_t len)
    {
        if (data == nullptr || len == 0) {
            return true;
        }
        if (!start() || opus_decoder_ == nullptr) {
            return false;
        }

        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = const_cast<uint8_t*>(data);
        raw.len = static_cast<uint32_t>(len);
        raw.consumed = 0;
        raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;

        esp_audio_dec_out_frame_t out_frame = {};
        out_frame.buffer = opus_decode_buffer_;
        out_frame.len = sizeof(opus_decode_buffer_);
        out_frame.needed_size = 0;
        out_frame.decoded_size = 0;
        esp_audio_dec_info_t info = {};

        esp_audio_err_t ret;
        {
            std::lock_guard<std::mutex> lock(opus_decoder_mutex_);
            ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &info);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                ESP_LOGE(TAG, "unexpected opus decode size: needed=%u capacity=%u",
                         out_frame.needed_size, static_cast<unsigned>(sizeof(opus_decode_buffer_)));
                return false;
            }
        }
        if (ret != ESP_AUDIO_ERR_OK || out_frame.decoded_size == 0) {
            opus_decode_failed_count_++;
            ESP_LOGW(TAG, "Opus decode failed: ret=%d decoded=%u consumed=%u len=%u", ret,
                     static_cast<unsigned>(out_frame.decoded_size), static_cast<unsigned>(raw.consumed),
                     static_cast<unsigned>(len));
            return false;
        }
        return play_pcm_24k(reinterpret_cast<int16_t*>(out_frame.buffer), out_frame.decoded_size / sizeof(int16_t),
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

        if (dji_exclusive_waiting_for_audio()) {
            drop_queued_clean_frames();
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

    AudioInputStatus input_status() const
    {
        DjiMicReceiverStatus dji = dji_mic_receiver_input_status();
        AudioInputStatus status;
        AudioInputSource active_source = static_cast<AudioInputSource>(active_input_source_.load());
#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT && CONFIG_STACKCHAN_DJI_MIC_AUTO_START
        TickType_t dji_start_ticks = dji_start_wait_ticks_.load();
        if (dji_mic_receiver_input_is_enabled() && !dji.capture_ready && dji.identity_confirmed &&
            dji_start_ticks != 0 && kDjiInputStartupTimeoutMs > 0 &&
            (xTaskGetTickCount() - dji_start_ticks) < pdMS_TO_TICKS(kDjiInputStartupTimeoutMs)) {
            active_source = AudioInputSource::kDjiMicReceiver;
        }
#endif
        status.active_source = active_source;
        status.dji_receiver_detected = dji.detected;
        status.dji_receiver_streaming = dji.audio_streaming;
        status.dji_receiver_capture_ready = dji.capture_ready;
        status.dji_receiver_identity_confirmed = dji.identity_confirmed;
        status.dji_receiver_manufacturer = dji.manufacturer;
        status.dji_receiver_product = dji.product;
        status.detail = dji.detail;
        return status;
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
        DjiMicReceiverStatus dji = dji_mic_receiver_input_status();
        ESP_LOGI(TAG, "service: initialized=%d output_started=%d input_started=%d running=%d playing=%d play_q=%u clean_q=%u clean_readers=%u vad=%d afe=%d selected_ch=%d input=%d dji_detected=%d dji_streaming=%d dji_capture=%d identity=%d %04x:%04x rate=%d ch=%d manufacturer=%s product=%s detail=%s",
                 static_cast<int>(initialized_.load()),
                 static_cast<int>(codec_output_started_.load()), static_cast<int>(codec_input_started_.load()),
                 static_cast<int>(running_.load()),
                 static_cast<int>(playing_.load()),
                 play_queue_ ? static_cast<unsigned>(uxQueueMessagesWaiting(play_queue_)) : 0,
                 clean_queue_ ? static_cast<unsigned>(uxQueueMessagesWaiting(clean_queue_)) : 0,
                 static_cast<unsigned>(clean_reader_count_.load()),
                 vad_state_.load(), static_cast<int>(afe_ready_), selected_input_channel_.load(),
                 active_input_source_.load(), static_cast<int>(dji.detected),
                 static_cast<int>(dji.audio_streaming), static_cast<int>(dji.capture_ready),
                 static_cast<int>(dji.identity_confirmed),
                 static_cast<unsigned>(dji.vendor_id), static_cast<unsigned>(dji.product_id),
                 dji.sample_rate, dji.channels,
                 dji.manufacturer != nullptr && dji.manufacturer[0] != '\0' ? dji.manufacturer : "-",
                 dji.product != nullptr && dji.product[0] != '\0' ? dji.product : "-",
                 dji.detail != nullptr ? dji.detail : "");
    }

    bool test_tone(int sample_rate, int tone_hz, int duration_ms, int volume_percent)
    {
        sample_rate = std::max(8000, sample_rate);
        tone_hz = std::max(50, std::min(tone_hz, sample_rate / 2 - 100));
        duration_ms = std::max(100, std::min(duration_ms, 10000));
        set_volume(volume_percent);

        const int total_samples = kDownstreamSampleRate * duration_ms / 1000;
        const int amplitude = 9000;
        std::vector<int16_t> buffer(kToneChunkSamples);
        float phase = 0.0f;
        const float phase_step = kTwoPi * static_cast<float>(tone_hz) / static_cast<float>(kDownstreamSampleRate);
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
            if (!play_pcm_24k(buffer.data(), chunk, AudioPlayOptions{.wait = true, .drop_oldest = false})) {
                return false;
            }
            generated += chunk;
        }
        wait_playback_idle(pdMS_TO_TICKS(duration_ms + 1000));
        ESP_LOGI(TAG, "Audio test tone done: requested_rate=%d playback_rate=%d samples=%d",
                 sample_rate, kDownstreamSampleRate, generated);
        return true;
    }

    bool available() const
    {
        return initialized_;
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

    bool ensure_output_started(const char* reason)
    {
#if !CONFIG_STACKCHAN_AUDIO_TX_ENABLED
        static std::atomic<bool> disabled_logged{false};
        if (!disabled_logged.exchange(true)) {
            ESP_LOGW(TAG, "扬声器I2S TX已通过CONFIG_STACKCHAN_AUDIO_TX_ENABLED关闭: reason=%s",
                     reason != nullptr ? reason : "-");
        }
        return true;
#else
        if (codec_output_started_.load()) {
            return true;
        }
        if (!codec_.start_output()) {
            ESP_LOGE(TAG, "speaker output start failed: reason=%s", reason != nullptr ? reason : "-");
            return false;
        }
        codec_output_started_ = true;
        ESP_LOGI(TAG, "扬声器输出已启动: reason=%s", reason != nullptr ? reason : "-");
        return true;
#endif
    }

    bool ensure_internal_input_started(const char* reason)
    {
        if (codec_input_started_.load()) {
            return true;
        }
        DjiMicReceiverStatus dji = dji_mic_receiver_input_status();
        if (dji_receiver_startup_waiting(dji)) {
            ESP_LOGI(TAG, "ES7210输入继续延后: reason=%s dji_capture=%d detail=%s",
                     reason != nullptr ? reason : "-",
                     static_cast<int>(dji.capture_ready),
                     dji.detail != nullptr ? dji.detail : "-");
            return false;
        }
        if (!codec_.start_input()) {
            ESP_LOGE(TAG, "ES7210 input start failed: reason=%s", reason != nullptr ? reason : "-");
            return false;
        }
        codec_input_started_ = true;
        ESP_LOGI(TAG, "ES7210输入已启动: reason=%s dji_capture=%d",
                 reason != nullptr ? reason : "-", static_cast<int>(dji.capture_ready));
#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
        ensure_afe_started(reason);
        ensure_afe_fetch_task_started();
#endif
        return true;
    }

    bool dji_receiver_startup_waiting(const DjiMicReceiverStatus& dji)
    {
#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT && CONFIG_STACKCHAN_DJI_MIC_AUTO_START
        if (!dji_mic_receiver_input_is_enabled() || dji.capture_ready || !dji.identity_confirmed ||
            kDjiInputStartupTimeoutMs <= 0) {
            return false;
        }
        TickType_t start_ticks = dji_start_wait_ticks_.load();
        if (start_ticks == 0) {
            return false;
        }
        TickType_t elapsed = xTaskGetTickCount() - start_ticks;
        if (elapsed < pdMS_TO_TICKS(kDjiInputStartupTimeoutMs)) {
            return true;
        }
        if (!dji_start_timeout_logged_.exchange(true)) {
            ESP_LOGW(TAG, "DJI Mic启动等待超时，允许ES7210输入fallback: timeout_ms=%d detected=%d streaming=%d capture=%d detail=%s",
                     kDjiInputStartupTimeoutMs,
                     static_cast<int>(dji.detected),
                     static_cast<int>(dji.audio_streaming),
                     static_cast<int>(dji.capture_ready),
                     dji.detail != nullptr ? dji.detail : "-");
        }
        return false;
#else
        (void)dji;
        return false;
#endif
    }

    bool init_opus_decoder()
    {
        esp_opus_dec_cfg_t opus_cfg = {};
        opus_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_24K;
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
    bool ensure_afe_started(const char* reason)
    {
        if (afe_ready_) {
            return true;
        }
        if (afe_init_attempted_ || !codec_input_started_.load()) {
            return false;
        }
        afe_init_attempted_ = true;
        afe_ready_ = init_afe();
        if (!afe_ready_) {
            ESP_LOGW(TAG, "AFE init failed: reason=%s", reason != nullptr ? reason : "-");
        }
        return afe_ready_;
    }

    void ensure_afe_fetch_task_started()
    {
        if (!running_ || !afe_ready_ || afe_task_ != nullptr) {
            return;
        }
        xTaskCreate(
            [](void* arg) {
                static_cast<XiaopaiAudioService*>(arg)->afe_fetch_task();
                vTaskDelete(nullptr);
            },
            "xiaopai_afe_fetch", 6144, this, 4, &afe_task_);
    }

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
        std::vector<int16_t> usb16(kUpstreamSampleRate / 25);
        in16.reserve((kUpstreamSampleRate / 100) * kInputChannels + kInputChannels);

        while (running_) {
            DjiMicReceiverStatus dji = dji_mic_receiver_input_status();
            log_dji_status_if_changed(dji);
            if (dji_receiver_should_own_input(dji)) {
                set_active_input_source(AudioInputSource::kDjiMicReceiver, dji,
                                        "DJI Mic接收器正在采集，内置麦克风停用");
                size_t usb_read = try_read_dji_receiver(usb16, dji);
                if (usb_read > 0) {
                    push_clean_samples(usb16.data(), usb_read);
                } else {
                    log_dji_priority_wait_if_needed(dji);
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
                continue;
            }
            if (dji_receiver_startup_waiting(dji)) {
                set_active_input_source(AudioInputSource::kDjiMicReceiver, dji,
                                        dji.detected ? "DJI Mic已接入，等待UAC音频，ES7210输入延后"
                                                     : "等待DJI Mic USB接收器，ES7210输入延后");
                drop_queued_clean_frames();
                log_dji_priority_wait_if_needed(dji);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            set_active_input_source(AudioInputSource::kInternalMic, dji,
                                    dji.detected ? "DJI Mic接收器暂不可采集" : "未检测到DJI Mic接收器");
            if (!codec_input_started_.load() && !ensure_internal_input_started("audio input fallback")) {
                log_input_unavailable_if_needed(dji);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (!codec_.read(hw.data(), hw.size())) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            resample_linear_interleaved(hw.data(), kHwInputChunkFrames, kInputChannels, kHwSampleRate, in16,
                                        kUpstreamSampleRate);
#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
            if (afe_ready_) {
                feed_afe(in16);
            } else
#endif
            {
                push_mono_from_interleaved(in16.data(), in16.size() / kInputChannels, kInputChannels);
            }
        }
        input_task_.store(nullptr);
        ESP_LOGW(TAG, "audio input task stopped");
    }

    size_t try_read_dji_receiver(std::vector<int16_t>& buffer, const DjiMicReceiverStatus& status)
    {
        if (!status.capture_ready || !status.identity_confirmed || buffer.empty()) {
            return 0;
        }
        size_t read = dji_mic_receiver_input_read_16k(buffer.data(), buffer.size(), pdMS_TO_TICKS(2));
        if (read == 0) {
            return 0;
        }
        uint32_t counter = ++dji_input_log_counter_;
        if ((counter % 100) == 1) {
            ESP_LOGI(TAG, "监听输入源: source=%s source_label=%s samples=%u rate=%d ch=%d dev=%04x:%04x",
                     audio_input_source_name_impl(AudioInputSource::kDjiMicReceiver),
                     audio_input_source_label_impl(AudioInputSource::kDjiMicReceiver),
                     static_cast<unsigned>(read), status.sample_rate, status.channels,
                     static_cast<unsigned>(status.vendor_id), static_cast<unsigned>(status.product_id));
        }
        return read;
    }

    void set_active_input_source(AudioInputSource source, const DjiMicReceiverStatus& dji, const char* reason)
    {
        int source_value = static_cast<int>(source);
        int previous_value = active_input_source_.exchange(source_value);
        if (previous_value == source_value) {
            return;
        }
        xiaopai_audio_source_commit(source == AudioInputSource::kDjiMicReceiver ? MicSource::DjiMic
                                                                                 : MicSource::InternalMic,
                                    reason);
        AudioInputSource previous = static_cast<AudioInputSource>(previous_value);
        ESP_LOGI(TAG, "监听输入源切换: from=%s from_label=%s to=%s to_label=%s reason=%s dji_detected=%d dji_streaming=%d dji_capture=%d dji_identity=%d dev=%04x:%04x manufacturer=%s product=%s detail=%s",
                 audio_input_source_name_impl(previous), audio_input_source_label_impl(previous),
                 audio_input_source_name_impl(source), audio_input_source_label_impl(source),
                 reason != nullptr ? reason : "-",
                 static_cast<int>(dji.detected), static_cast<int>(dji.audio_streaming),
                 static_cast<int>(dji.capture_ready), static_cast<int>(dji.identity_confirmed),
                 static_cast<unsigned>(dji.vendor_id), static_cast<unsigned>(dji.product_id),
                 dji.manufacturer != nullptr && dji.manufacturer[0] != '\0' ? dji.manufacturer : "-",
                 dji.product != nullptr && dji.product[0] != '\0' ? dji.product : "-",
                 dji.detail != nullptr ? dji.detail : "-");
    }

    void log_dji_priority_wait_if_needed(const DjiMicReceiverStatus& dji)
    {
        TickType_t now = xTaskGetTickCount();
        if (last_dji_priority_wait_log_ticks_ != 0 &&
            now - last_dji_priority_wait_log_ticks_ < pdMS_TO_TICKS(kDjiPriorityWaitLogIntervalMs)) {
            return;
        }
        last_dji_priority_wait_log_ticks_ = now;
        ESP_LOGI(TAG, "监听输入源保持: source=%s source_label=%s reason=DJI Mic优先级更高，USB音频协议已匹配，等待音频数据 capture=%d dev=%04x:%04x manufacturer=%s product=%s detail=%s",
                 audio_input_source_name_impl(AudioInputSource::kDjiMicReceiver),
                 audio_input_source_label_impl(AudioInputSource::kDjiMicReceiver),
                 static_cast<int>(dji.capture_ready),
                 static_cast<unsigned>(dji.vendor_id), static_cast<unsigned>(dji.product_id),
                 dji.manufacturer != nullptr && dji.manufacturer[0] != '\0' ? dji.manufacturer : "-",
                 dji.product != nullptr && dji.product[0] != '\0' ? dji.product : "-",
                 dji.detail != nullptr ? dji.detail : "-");
    }

    void log_input_unavailable_if_needed(const DjiMicReceiverStatus& dji)
    {
        TickType_t now = xTaskGetTickCount();
        if (last_input_unavailable_log_ticks_ != 0 &&
            now - last_input_unavailable_log_ticks_ < pdMS_TO_TICKS(kInputUnavailableLogIntervalMs)) {
            return;
        }
        last_input_unavailable_log_ticks_ = now;
        ESP_LOGW(TAG, "监听输入不可用: ES7210输入尚未启动 detected=%d streaming=%d capture=%d detail=%s",
                 static_cast<int>(dji.detected), static_cast<int>(dji.audio_streaming),
                 static_cast<int>(dji.capture_ready), dji.detail != nullptr ? dji.detail : "-");
    }

    void log_dji_status_if_changed(const DjiMicReceiverStatus& dji)
    {
        bool detail_changed = last_dji_detail_ != dji.detail;
        bool changed = !last_dji_status_valid_ ||
                       last_dji_detected_ != dji.detected ||
                       last_dji_audio_streaming_ != dji.audio_streaming ||
                       last_dji_capture_ready_ != dji.capture_ready ||
                       last_dji_identity_confirmed_ != dji.identity_confirmed ||
                       last_dji_vendor_id_ != dji.vendor_id ||
                       last_dji_product_id_ != dji.product_id ||
                       last_dji_sample_rate_ != dji.sample_rate ||
                       last_dji_channels_ != dji.channels ||
                       detail_changed;
        if (!changed) {
            return;
        }
        last_dji_status_valid_ = true;
        last_dji_detected_ = dji.detected;
        last_dji_audio_streaming_ = dji.audio_streaming;
        last_dji_capture_ready_ = dji.capture_ready;
        last_dji_identity_confirmed_ = dji.identity_confirmed;
        last_dji_vendor_id_ = dji.vendor_id;
        last_dji_product_id_ = dji.product_id;
        last_dji_sample_rate_ = dji.sample_rate;
        last_dji_channels_ = dji.channels;
        last_dji_detail_ = dji.detail;
        ESP_LOGI(TAG, "DJI Mic状态: detected=%d streaming=%d capture=%d identity=%d dev=%04x:%04x rate=%d ch=%d manufacturer=%s product=%s detail=%s",
                 static_cast<int>(dji.detected),
                 static_cast<int>(dji.audio_streaming),
                 static_cast<int>(dji.capture_ready),
                 static_cast<int>(dji.identity_confirmed),
                 static_cast<unsigned>(dji.vendor_id),
                 static_cast<unsigned>(dji.product_id),
                 dji.sample_rate, dji.channels,
                 dji.manufacturer != nullptr && dji.manufacturer[0] != '\0' ? dji.manufacturer : "-",
                 dji.product != nullptr && dji.product[0] != '\0' ? dji.product : "-",
                 dji.detail != nullptr ? dji.detail : "-");
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
            uint32_t counter = ++playback_log_counter_;
            if ((counter % 12) == 1) {
                ESP_LOGI(TAG, "direct playback block: samples=%u rate=%d peak=%d pending=%u",
                         static_cast<unsigned>(block->samples), kHwSampleRate,
                         peak_abs_sample(block->data, block->samples),
                         static_cast<unsigned>(pending_play_blocks_.load()));
            }
            const size_t chunk = static_cast<size_t>(kHwSampleRate / 50);
            size_t offset = 0;
            while (offset < block->samples && running_ && local_generation == abort_generation_.load()) {
                const size_t to_write = std::min(chunk, block->samples - offset);
                if (!codec_.write(block->data + offset, to_write)) {
                    break;
                }
                offset += to_write;
            }
            free_block(block);
            decrement_pending_play_blocks();
            playing_ = false;
        }
        output_task_.store(nullptr);
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
        AudioBlock* block = allocate_block(frames, false);
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
        if (data == nullptr || samples == 0) {
            return;
        }

        update_energy_vad(data, samples);

        AudioBlock* block = allocate_block(samples, false);
        if (block == nullptr) {
            return;
        }
        for (size_t i = 0; i < samples; ++i) {
            block->data[i] = apply_mic_magnification(data[i]);
        }
        push_clean_block_keep_latest(block);
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

    void push_clean_block_keep_latest(AudioBlock* block)
    {
        if (block == nullptr || clean_queue_ == nullptr) {
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

    bool dji_exclusive_waiting_for_audio() const
    {
#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT && CONFIG_STACKCHAN_DJI_MIC_AUTO_START
        DjiMicReceiverStatus dji = dji_mic_receiver_input_status();
        TickType_t start_ticks = dji_start_wait_ticks_.load();
        return dji_mic_receiver_input_is_enabled() && !dji.capture_ready && dji.identity_confirmed &&
               start_ticks != 0 && kDjiInputStartupTimeoutMs > 0 &&
               (xTaskGetTickCount() - start_ticks) < pdMS_TO_TICKS(kDjiInputStartupTimeoutMs);
#else
        return false;
#endif
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
            AudioInputSource source = static_cast<AudioInputSource>(active_input_source_.load());
            ESP_LOGI(TAG, "输入电平均值: source=%s source_label=%s selected=%d best=%d mag=%d ch0=%u ch1=%u ch2=%u ch3=%u",
                     audio_input_source_name_impl(source), audio_input_source_label_impl(source),
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
    std::mutex lifecycle_mutex_;
    AudioBlockPool play_pool_;
    AudioBlockPool rec_pool_;
    uint8_t opus_decode_buffer_[kDownstreamFrameSamples * sizeof(int16_t)] = {};
    QueueHandle_t play_queue_ = nullptr;
    QueueHandle_t clean_queue_ = nullptr;
    SemaphoreHandle_t read_mutex_ = nullptr;
    AudioBlock* read_block_ = nullptr;
    size_t read_offset_ = 0;
    std::atomic<TaskHandle_t> input_task_{nullptr};
    std::atomic<TaskHandle_t> output_task_{nullptr};
    std::atomic<TaskHandle_t> heartbeat_task_handle_{nullptr};
    void* opus_decoder_ = nullptr;
    std::mutex opus_decoder_mutex_;
    std::atomic<uint32_t> play_dropped_count_{0};
    std::atomic<uint32_t> opus_decode_failed_count_{0};
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
    std::atomic<uint32_t> dji_input_log_counter_{0};
    std::atomic<uint32_t> playback_log_counter_{0};
    TickType_t last_dji_priority_wait_log_ticks_ = 0;
    TickType_t last_input_unavailable_log_ticks_ = 0;
    std::atomic<int> selected_input_channel_{0};
    std::atomic<int> active_input_source_{static_cast<int>(AudioInputSource::kInternalMic)};
    std::atomic<bool> codec_output_started_{false};
    std::atomic<bool> codec_input_started_{false};
    std::atomic<TickType_t> dji_start_wait_ticks_{0};
    std::atomic<bool> dji_start_timeout_logged_{false};
    bool last_dji_status_valid_ = false;
    bool last_dji_detected_ = false;
    bool last_dji_audio_streaming_ = false;
    bool last_dji_capture_ready_ = false;
    bool last_dji_identity_confirmed_ = false;
    uint16_t last_dji_vendor_id_ = 0;
    uint16_t last_dji_product_id_ = 0;
    int last_dji_sample_rate_ = 0;
    int last_dji_channels_ = 0;
    const char* last_dji_detail_ = nullptr;
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

#include "xiaopai_state.h"

void XiaopaiAudioService::log_diagnostics()
{
    auto log_watermark = [](const char* name, TaskHandle_t handle) {
        if (handle != nullptr) {
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(handle);
            ESP_LOGI("XiaopaiDiag", "Task %s watermark: %u words (%u bytes)", name,
                     static_cast<unsigned>(watermark), static_cast<unsigned>(watermark * sizeof(StackType_t)));
        } else {
            ESP_LOGI("XiaopaiDiag", "Task %s is not running", name);
        }
    };

    log_watermark("audio_input_task", input_task_.load());
    log_watermark("audio_output_hw", output_task_.load());
    log_watermark("supervisor_task", xiaopai_supervisor_get_task_handle());
    log_watermark("heartbeat_task", heartbeat_task_handle_.load());
#if CONFIG_STACKCHAN_AUDIO_DEVICE_AEC
    log_watermark("AFE fetch task", afe_task_);
#endif

    UBaseType_t play_free = play_queue_ != nullptr ? uxQueueMessagesWaiting(play_queue_) : 0;
    UBaseType_t clean_free = clean_queue_ != nullptr ? uxQueueMessagesWaiting(clean_queue_) : 0;

    UBaseType_t play_pool_free = play_pool_.free_queue != nullptr ? uxQueueMessagesWaiting(play_pool_.free_queue) : 0;
    UBaseType_t rec_pool_free = rec_pool_.free_queue != nullptr ? uxQueueMessagesWaiting(rec_pool_.free_queue) : 0;

    ESP_LOGI("XiaopaiDiag", "Play pool free: %u/%u (exhausted: %u)",
             static_cast<unsigned>(play_pool_free), static_cast<unsigned>(play_pool_.num_blocks),
             static_cast<unsigned>(play_pool_.exhaustion_count.load()));
    ESP_LOGI("XiaopaiDiag", "Rec pool free: %u/%u (exhausted: %u)",
             static_cast<unsigned>(rec_pool_free), static_cast<unsigned>(rec_pool_.num_blocks),
             static_cast<unsigned>(rec_pool_.exhaustion_count.load()));

    ESP_LOGI("XiaopaiDiag", "Play queue depth: %u, Clean queue depth: %u",
             static_cast<unsigned>(play_free), static_cast<unsigned>(clean_free));

    ESP_LOGI("XiaopaiDiag", "Dropped play blocks: %u", static_cast<unsigned>(play_dropped_count_.load()));
    ESP_LOGI("XiaopaiDiag", "Dropped clean blocks: %u, idle dropped: %u",
             static_cast<unsigned>(clean_drop_count_.load()), static_cast<unsigned>(clean_idle_drop_count_.load()));
    ESP_LOGI("XiaopaiDiag", "Opus decode failed: %u", static_cast<unsigned>(opus_decode_failed_count_.load()));
    ESP_LOGI("XiaopaiDiag", "Total internal free: %u, largest free block: %u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

static AudioBlock* allocate_block(size_t samples, bool is_play)
{
    if (g_audio_service_ptr == nullptr) {
        return nullptr;
    }
    return g_audio_service_ptr->lease_block(samples, is_play);
}

static void free_block(AudioBlock* block)
{
    if (block != nullptr && g_audio_service_ptr != nullptr) {
        g_audio_service_ptr->release_block(block);
    }
}

XiaopaiAudioService g_audio_service;

}  // namespace

const char* audio_input_source_name(AudioInputSource source)
{
    return audio_input_source_name_impl(source);
}

const char* audio_input_source_label(AudioInputSource source)
{
    return audio_input_source_label_impl(source);
}

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

bool audio_service_play_pcm_24k(const int16_t* samples, size_t count, AudioPlayOptions options)
{
    return g_audio_service.play_pcm_24k(samples, count, options);
}

bool audio_service_play_opus_frame_24k(const uint8_t* data, size_t len)
{
    return g_audio_service.play_opus_frame_24k(data, len);
}

size_t audio_service_read_clean_16k(int16_t* out, size_t samples, TickType_t timeout)
{
    return g_audio_service.read_clean_16k(out, samples, timeout);
}

AudioVadState audio_service_get_vad_state()
{
    return g_audio_service.vad_state();
}

AudioInputStatus audio_service_get_input_status()
{
    return g_audio_service.input_status();
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

void audio_service_log_diagnostics()
{
    g_audio_service.log_diagnostics();
}

void audio_service_register_heartbeat_task(TaskHandle_t handle)
{
    g_audio_service.register_heartbeat_task(handle);
}
