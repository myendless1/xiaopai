#include "codec_audio_output.h"

#include <M5Unified.h>

#include "audio_codec_ctrl_if.h"
#include "audio_codec_data_if.h"
#include "audio_codec_gpio_if.h"
#include "audio_codec_if.h"
#include "aw88298_dac.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

static constexpr const char* TAG = "CodecAudioOutput";
static constexpr i2s_port_t kAudioI2sPort = I2S_NUM_0;
static constexpr gpio_num_t kAudioMclkPin = GPIO_NUM_0;
static constexpr gpio_num_t kAudioWsPin = GPIO_NUM_33;
static constexpr gpio_num_t kAudioBclkPin = GPIO_NUM_34;
static constexpr gpio_num_t kAudioDoutPin = GPIO_NUM_13;
static constexpr uint8_t kAxp2101Addr = 0x34;
static constexpr uint8_t kAw9523Addr = 0x58;
static constexpr uint8_t kAw88298Addr = AW88298_CODEC_DEFAULT_ADDR;
static constexpr uint32_t kInternalI2cFreq = 400000;
static constexpr int kDmaDescNum = 6;
static constexpr int kDmaFrameNum = 240;
static constexpr int kTailDrainMs = 80;
static constexpr int kToneChunkSamples = 512;
static constexpr float kTwoPi = 6.28318530717958647692f;
static constexpr uint8_t kAw9523SpeakerPowerMask = 0b00000100;
static constexpr uint8_t kAw9523BoostPowerMask = 0b10000000;

struct M5I2cCodecCtrl {
    audio_codec_ctrl_if_t base;
    bool open = false;
    uint8_t addr = kAw88298Addr;
};

struct CodecAudioOutputState {
    M5I2cCodecCtrl ctrl = {};
    const audio_codec_data_if_t* data_if = nullptr;
    const audio_codec_if_t* codec_if = nullptr;
    const audio_codec_gpio_if_t* gpio_if = nullptr;
    esp_codec_dev_handle_t dev = nullptr;
    i2s_chan_handle_t tx_handle = nullptr;
    int sample_rate = 0;
    int volume_percent = 70;
    bool initialized = false;
    bool active = false;
};

CodecAudioOutputState g_audio;

static int clamp_volume_percent(int percent)
{
    return std::max(0, std::min(100, percent));
}

static int codec_volume_from_percent(int percent)
{
    return clamp_volume_percent(percent);
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

static bool read_aw88298_reg_direct(uint8_t reg, uint16_t* value)
{
    uint8_t data[2] = {};
    if (!M5.In_I2C.readRegister(kAw88298Addr >> 1, reg, data, sizeof(data), kInternalI2cFreq)) {
        ESP_LOGW(TAG, "AW88298 direct read reg=0x%02x failed", reg);
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
    if (read_aw88298_reg_direct(reg, &value)) {
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

static void configure_core_s3_audio_power()
{
    bool aw9523_present = M5.In_I2C.scanID(kAw9523Addr, kInternalI2cFreq);
    bool aw88298_present = M5.In_I2C.scanID(kAw88298Addr >> 1, kInternalI2cFreq);
    bool axp2101_present = M5.In_I2C.scanID(kAxp2101Addr, kInternalI2cFreq);
    ESP_LOGI(TAG, "Audio I2C scan: AW9523=%d AW88298=%d AXP2101=%d",
             aw9523_present, aw88298_present, axp2101_present);

    if (axp2101_present) {
        write_i2c_reg8(kAxp2101Addr, 0x90, 0xBF, "AXP2101 LDOS");
        write_i2c_reg8(kAxp2101Addr, 0x92, 18 - 5, "AXP2101 ALDO1");
        read_i2c_reg8(kAxp2101Addr, 0x90, "AXP2101 LDOS");
        read_i2c_reg8(kAxp2101Addr, 0x92, "AXP2101 ALDO1");
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
        read_i2c_reg8(kAw9523Addr, 0x02, "AW9523 P0 output");
        read_i2c_reg8(kAw9523Addr, 0x03, "AW9523 P1 output");
        read_i2c_reg8(kAw9523Addr, 0x04, "AW9523 P0 config");
        read_i2c_reg8(kAw9523Addr, 0x05, "AW9523 P1 config");
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

static bool aw9523_set_speaker_power(bool enable)
{
    if (enable) {
        configure_core_s3_audio_power();
    }
    bool ok = enable ? M5.In_I2C.bitOn(kAw9523Addr, 0x02, kAw9523SpeakerPowerMask, kInternalI2cFreq)
                     : M5.In_I2C.bitOff(kAw9523Addr, 0x02, kAw9523SpeakerPowerMask, kInternalI2cFreq);
    if (!ok) {
        ESP_LOGW(TAG, "AW9523 speaker power %s failed", enable ? "enable" : "disable");
    }
    return ok;
}

static void restore_aw88298_playback_registers(int sample_rate)
{
    write_aw88298_reg_direct(0x61, 0x0673, "AW88298 restore boost");
    write_aw88298_reg_direct(0x04, 0x4040, "AW88298 restore sysctrl");
    write_aw88298_reg_direct(0x05, 0x0008, "AW88298 restore unmute");
    write_aw88298_reg_direct(0x06, aw88298_i2sctrl_for_sample_rate(sample_rate), "AW88298 restore i2sctrl");
}

static void disable_aw88298_playback_registers()
{
    write_aw88298_reg_direct(0x04, 0x4000, "AW88298 disable i2s");
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

static void init_m5_i2c_ctrl(M5I2cCodecCtrl* ctrl)
{
    ctrl->base.open = m5_i2c_ctrl_open;
    ctrl->base.is_open = m5_i2c_ctrl_is_open;
    ctrl->base.read_reg = m5_i2c_ctrl_read_reg;
    ctrl->base.write_reg = m5_i2c_ctrl_write_reg;
    ctrl->base.close = m5_i2c_ctrl_close;
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 1,
        .addr = kAw88298Addr,
        .bus_handle = nullptr,
    };
    ctrl->base.open(&ctrl->base, &i2c_cfg, sizeof(i2c_cfg));
}

static bool create_i2s_channel()
{
    if (g_audio.tx_handle != nullptr) {
        return true;
    }

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
    esp_err_t err = i2s_new_channel(&chan_cfg, &g_audio.tx_handle, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        g_audio.tx_handle = nullptr;
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = 16000,
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
    err = i2s_channel_init_std_mode(g_audio.tx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        i2s_del_channel(g_audio.tx_handle);
        g_audio.tx_handle = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "I2S TX initialized: port=%d mclk=%d bclk=%d ws=%d dout=%d dma_desc=%d dma_frame=%d",
             static_cast<int>(kAudioI2sPort), static_cast<int>(kAudioMclkPin), static_cast<int>(kAudioBclkPin),
             static_cast<int>(kAudioWsPin), static_cast<int>(kAudioDoutPin), kDmaDescNum, kDmaFrameNum);
    return true;
}

static bool init_codec_dev()
{
    if (g_audio.initialized) {
        return true;
    }
    aw9523_set_speaker_power(true);

    init_m5_i2c_ctrl(&g_audio.ctrl);
    if (!create_i2s_channel()) {
        return false;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = kAudioI2sPort,
        .rx_handle = nullptr,
        .tx_handle = g_audio.tx_handle,
        .clk_src = I2S_CLK_SRC_DEFAULT,
    };
    g_audio.data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (g_audio.data_if == nullptr) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        return false;
    }

    g_audio.gpio_if = audio_codec_new_gpio();
    if (g_audio.gpio_if == nullptr) {
        ESP_LOGE(TAG, "audio_codec_new_gpio failed");
        return false;
    }

    aw88298_codec_cfg_t aw88298_cfg = {};
    aw88298_cfg.ctrl_if = &g_audio.ctrl.base;
    aw88298_cfg.gpio_if = g_audio.gpio_if;
    aw88298_cfg.reset_pin = GPIO_NUM_NC;
    aw88298_cfg.hw_gain.pa_voltage = 5.0;
    aw88298_cfg.hw_gain.codec_dac_voltage = 3.3;
    aw88298_cfg.hw_gain.pa_gain = 1;
    g_audio.codec_if = aw88298_codec_new(&aw88298_cfg);
    if (g_audio.codec_if == nullptr) {
        ESP_LOGE(TAG, "aw88298_codec_new failed");
        return false;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = g_audio.codec_if,
        .data_if = g_audio.data_if,
    };
    g_audio.dev = esp_codec_dev_new(&dev_cfg);
    if (g_audio.dev == nullptr) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return false;
    }
    g_audio.initialized = true;
    ESP_LOGI(TAG, "CoreS3 codec output initialized");
    return true;
}

}  // namespace

bool codec_audio_output_begin(int sample_rate, int volume_percent)
{
    if (!init_codec_dev()) {
        return false;
    }
    if (g_audio.active && g_audio.sample_rate == sample_rate) {
        codec_audio_output_set_volume_percent(volume_percent);
        return true;
    }
    if (g_audio.active) {
        codec_audio_output_end();
    }

    g_audio.sample_rate = sample_rate;
    g_audio.volume_percent = clamp_volume_percent(volume_percent);
    aw9523_set_speaker_power(true);
    reset_core_s3_aw88298();

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = static_cast<uint32_t>(sample_rate),
        .mclk_multiple = I2S_MCLK_MULTIPLE_256,
    };
    int ret = esp_codec_dev_open(g_audio.dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", ret);
        return false;
    }
    restore_aw88298_playback_registers(sample_rate);
    ret = esp_codec_dev_set_out_vol(g_audio.dev, codec_volume_from_percent(g_audio.volume_percent));
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "esp_codec_dev_set_out_vol failed: %d", ret);
    }
    g_audio.active = true;
    ESP_LOGI(TAG, "Codec output opened: sample_rate=%d volume=%d", sample_rate, g_audio.volume_percent);
    codec_audio_output_dump_state();
    return true;
}

bool codec_audio_output_write(const int16_t* samples, size_t sample_count)
{
    if (samples == nullptr || sample_count == 0) {
        return true;
    }
    if (!g_audio.active || g_audio.dev == nullptr) {
        ESP_LOGE(TAG, "codec audio output is not active");
        return false;
    }
    int ret = esp_codec_dev_write(g_audio.dev, const_cast<int16_t*>(samples),
                                  static_cast<int>(sample_count * sizeof(int16_t)));
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_write failed: %d", ret);
        return false;
    }
    return true;
}

void codec_audio_output_drain()
{
    if (g_audio.active) {
        vTaskDelay(pdMS_TO_TICKS(kTailDrainMs));
    }
}

void codec_audio_output_stop()
{
    if (!g_audio.initialized || g_audio.dev == nullptr) {
        return;
    }
    if (g_audio.active) {
        esp_codec_dev_close(g_audio.dev);
        disable_aw88298_playback_registers();
        aw9523_set_speaker_power(false);
        g_audio.active = false;
        g_audio.sample_rate = 0;
    }
}

void codec_audio_output_end()
{
    codec_audio_output_stop();
}

bool codec_audio_output_is_active()
{
    return g_audio.active;
}

void codec_audio_output_set_volume_percent(int volume_percent)
{
    g_audio.volume_percent = clamp_volume_percent(volume_percent);
    if (g_audio.active && g_audio.dev != nullptr) {
        int ret = esp_codec_dev_set_out_vol(g_audio.dev, codec_volume_from_percent(g_audio.volume_percent));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "esp_codec_dev_set_out_vol failed: %d", ret);
        }
    }
}

void codec_audio_output_dump_state()
{
    configure_core_s3_audio_power();
    log_aw88298_reg_direct(0x00);
    log_aw88298_reg_direct(0x04);
    log_aw88298_reg_direct(0x05);
    log_aw88298_reg_direct(0x06);
    log_aw88298_reg_direct(0x0C);
    log_aw88298_reg_direct(0x61);
    if (g_audio.dev != nullptr) {
        ESP_LOGI(TAG, "AW88298 esp_codec_dev register dump follows");
        esp_codec_dev_dump_reg(g_audio.dev);
    }
}

bool codec_audio_output_test_tone(int sample_rate, int tone_hz, int duration_ms, int volume_percent)
{
    sample_rate = std::max(8000, sample_rate);
    tone_hz = std::max(50, std::min(tone_hz, sample_rate / 2 - 100));
    duration_ms = std::max(100, std::min(duration_ms, 10000));
    volume_percent = clamp_volume_percent(volume_percent);

    ESP_LOGI(TAG, "Audio test tone: sample_rate=%d tone=%d duration_ms=%d volume=%d",
             sample_rate, tone_hz, duration_ms, volume_percent);
    if (!codec_audio_output_begin(sample_rate, volume_percent)) {
        ESP_LOGE(TAG, "Audio test tone failed to open codec output");
        return false;
    }

    std::vector<int16_t> buffer(kToneChunkSamples);
    const int total_samples = sample_rate * duration_ms / 1000;
    const int amplitude = 9000;
    float phase = 0.0f;
    const float phase_step = kTwoPi * static_cast<float>(tone_hz) / static_cast<float>(sample_rate);
    int written_samples = 0;
    while (written_samples < total_samples) {
        int chunk_samples = std::min<int>(static_cast<int>(buffer.size()), total_samples - written_samples);
        for (int i = 0; i < chunk_samples; ++i) {
            buffer[i] = static_cast<int16_t>(std::sin(phase) * amplitude);
            phase += phase_step;
            if (phase >= kTwoPi) {
                phase -= kTwoPi;
            }
        }
        if (!codec_audio_output_write(buffer.data(), chunk_samples)) {
            codec_audio_output_stop();
            return false;
        }
        written_samples += chunk_samples;
    }
    codec_audio_output_drain();
    codec_audio_output_stop();
    ESP_LOGI(TAG, "Audio test tone done: samples=%d", written_samples);
    return true;
}
