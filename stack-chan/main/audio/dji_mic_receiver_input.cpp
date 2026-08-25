#include "dji_mic_receiver_input.h"

#include "dji_mic_usb_power.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/ringbuf.h"
#include "power_manager.h"
#include "sdkconfig.h"
#include "usb_stream.h"
#include "xiaopai_psram_task.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <inttypes.h>
#include <array>

#ifndef CONFIG_STACKCHAN_DJI_MIC_USB_INPUT
#define CONFIG_STACKCHAN_DJI_MIC_USB_INPUT 0
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_DRIVE_VBUS
#define CONFIG_STACKCHAN_DJI_MIC_DRIVE_VBUS 0
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE
#define CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE 48000
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS
#define CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS 2
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS
#define CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS 24
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_RINGBUF_BYTES
#define CONFIG_STACKCHAN_DJI_MIC_UAC_RINGBUF_BYTES 131072
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_INTERNAL_BUF_BYTES
#define CONFIG_STACKCHAN_DJI_MIC_UAC_INTERNAL_BUF_BYTES 16384
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_STABLE_MS
#define CONFIG_STACKCHAN_DJI_MIC_STABLE_MS 800
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_DIGITAL_GAIN_PERCENT
#define CONFIG_STACKCHAN_DJI_MIC_DIGITAL_GAIN_PERCENT 100
#endif

namespace {

static constexpr const char* TAG = "DjiMicInput";
static constexpr uint16_t kDjiVid = 0x2ca3;
static constexpr uint16_t kDjiPid = 0x4011;
static constexpr size_t kCallbackOutChunkSamples = 512;
static constexpr TickType_t kStableGapToleranceTicks = pdMS_TO_TICKS(250);
static constexpr std::array<int16_t, 63> kDecimatorFirQ15 = {
    0, 0, 0, 3, 6, 2, -11, -23, -13, 24, 60, 46, -35, -126, -123, 26,
    227, 274, 36, -357, -538, -209, 501, 989, 616, -635, -1853, -1668,
    730, 4768, 8590, 10154, 8590, 4768, 730, -1668, -1853, -635, 616,
    989, 501, -209, -538, -357, 36, 274, 227, 26, -123, -126, -35, 46,
    60, 24, -13, -23, -11, 2, 6, 3, 0, 0, 0,
};

struct RawUacFrameHeader {
    uint32_t data_bytes = 0;
    uint32_t sample_rate = 0;
    uint16_t bit_resolution = 0;
    uint16_t channels = 0;
};

static int16_t clamp_i16(int32_t value)
{
    if (value > 32767) {
        return 32767;
    }
    if (value < -32768) {
        return -32768;
    }
    return static_cast<int16_t>(value);
}

static int32_t read_le_pcm_sample(const uint8_t* data, uint16_t bits)
{
    if (bits == 24) {
        int32_t value = static_cast<int32_t>(data[0]) |
                        (static_cast<int32_t>(data[1]) << 8) |
                        (static_cast<int32_t>(data[2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<int32_t>(0xff000000);
        }
        return value >> 8;
    }
    if (bits == 16) {
        return static_cast<int16_t>(static_cast<uint16_t>(data[0]) |
                                    (static_cast<uint16_t>(data[1]) << 8));
    }
    if (bits == 8) {
        return (static_cast<int32_t>(data[0]) - 128) << 8;
    }
    return 0;
}

class DjiMicReceiverInput {
public:
    bool start()
    {
#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT
        if (started_.load()) {
            if (capture_ready_.load()) {
                return true;
            }
            ESP_LOGW(TAG, "DJI Mic already started without capture; restarting");
            stop("capture not ready, restart");
        }
        if (started_.exchange(true)) {
            return capture_ready_.load();
        }

        ESP_LOGI(TAG,
                 "DJI Mic start heap: internal_free=%u internal_largest=%u dma_largest=%u spiram_free=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)));

        pcm_ringbuf_ = xRingbufferCreateWithCaps(CONFIG_STACKCHAN_DJI_MIC_UAC_RINGBUF_BYTES,
                                                 RINGBUF_TYPE_BYTEBUF,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pcm_ringbuf_ == nullptr) {
            pcm_ringbuf_ = xRingbufferCreateWithCaps(CONFIG_STACKCHAN_DJI_MIC_UAC_RINGBUF_BYTES,
                                                     RINGBUF_TYPE_BYTEBUF,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (pcm_ringbuf_ == nullptr) {
            set_detail("DJI Mic PCM ringbuffer alloc failed");
            started_ = false;
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }

        raw_ringbuf_ = xRingbufferCreateWithCaps(CONFIG_STACKCHAN_DJI_MIC_UAC_RINGBUF_BYTES,
                                                 RINGBUF_TYPE_NOSPLIT,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (raw_ringbuf_ == nullptr) {
            raw_ringbuf_ = xRingbufferCreateWithCaps(CONFIG_STACKCHAN_DJI_MIC_UAC_RINGBUF_BYTES,
                                                     RINGBUF_TYPE_NOSPLIT,
                                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (raw_ringbuf_ == nullptr) {
            set_detail("DJI Mic raw ringbuffer alloc failed");
            started_ = false;
            delete_ringbufs();
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }

        sample_rate_ = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE;
        bit_resolution_ = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS;
        channels_ = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS;
        connected_ = false;
        capture_ready_ = false;
        stream_stable_ = false;
        callback_count_ = 0;
        callback_bytes_ = 0;
        output_samples_ = 0;
        raw_drops_ = 0;
        pcm_drops_ = 0;
        format_errors_ = 0;
        selected_channel_ = 0;
        selected_channel_locked_ = false;
        channel_select_log_counter_ = 0;
        stable_started_ticks_ = 0;
        last_valid_frame_ticks_ = 0;
        channel_energy_[0] = 0;
        channel_energy_[1] = 0;
        fir_history_.fill(0);
        fir_history_pos_ = 0;
        fir_decimation_phase_ = 0;

        uac_config_t uac_config = {};
        uac_config.mic_ch_num = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS;
        uac_config.mic_bit_resolution = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS;
        uac_config.mic_samples_frequence = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE;
        uac_config.mic_buf_size = 0;
        uac_config.mic_cb = mic_frame_cb;
        uac_config.mic_cb_arg = this;

        esp_err_t err = usb_streaming_state_register(usb_state_cb, this);
        if (err != ESP_OK) {
            set_detail("usb_stream state cb failed: %s", esp_err_to_name(err));
            started_ = false;
            delete_ringbufs();
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }

        err = uac_streaming_config(&uac_config);
        if (err != ESP_OK) {
            set_detail("UAC config failed: %s", esp_err_to_name(err));
            started_ = false;
            delete_ringbufs();
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }

        decode_task_ = nullptr;
        BaseType_t task_ok = xiaopai_task_create_psram(
            [](void* arg) {
                static_cast<DjiMicReceiverInput*>(arg)->decode_task();
                vTaskDeleteWithCaps(nullptr);
            },
            "dji_uac_decode",
            8192,
            this,
            4,
            &decode_task_,
            0);
        if (task_ok != pdPASS) {
            decode_task_ = nullptr;
            set_detail("DJI Mic decode task create failed");
            started_ = false;
            delete_ringbufs();
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }

        set_detail("DJI Mic USB输入已启动，等待首帧UAC PCM");
        ESP_LOGI(TAG, "DJI Mic USB输入已启动: source=dji_mic_receiver; 收到UAC首帧后切换主输入");

        set_detail("关闭CoreS3 USB VBUS，准备重新枚举DJI Mic");
        err = dji_mic_usb_power_prepare_host();
        if (err != ESP_OK) {
            set_detail("DJI USB power policy denied host start: %s", esp_err_to_name(err));
            stop_decode_task();
            delete_ringbufs();
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }

        set_detail("启动usb_stream，等待DJI Mic USB枚举");
        err = usb_streaming_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            set_detail("usb_stream start failed: %s", esp_err_to_name(err));
            dji_mic_usb_power_abort("usb_stream start failed");
            (void)usb_streaming_stop();
            stop_decode_task();
            delete_ringbufs();
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }

        set_detail("打开CoreS3 USB VBUS输出，等待DJI Mic重新上电");
        err = dji_mic_usb_power_enable_after_host();
        if (err != ESP_OK) {
            set_detail("DJI USB VBUS enable failed: %s", esp_err_to_name(err));
            usb_streaming_stop();
            stop_decode_task();
            delete_ringbufs();
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }
#if CONFIG_STACKCHAN_DJI_MIC_DRIVE_VBUS
        bool usb_output = stackchan_power_manager_status().usb_output;
        ESP_LOGI(TAG, "DJI Mic USB VBUS status after enable: %d", static_cast<int>(usb_output));
#endif

        set_detail("等待DJI Mic USB接收器接入；首帧UAC PCM后切换输入");
        ESP_LOGI(TAG, "DJI Mic UAC输入已启动: requested=%uHz %u-bit %u ch, output=16000Hz mono",
                 static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE),
                 static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS),
                 static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS));
        for (int waited_ms = 0; waited_ms < 4000; waited_ms += 100) {
            if (capture_ready_.load()) {
                set_detail("DJI Mic UAC采集已就绪");
                ESP_LOGI(TAG, "DJI Mic UAC capture ready after %d ms", waited_ms);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ESP_LOGE(TAG, "DJI Mic enumerated without UAC capture; receiver LED does not mean input is ready");
        set_detail("DJI Mic已枚举但UAC采集未就绪");
        stop("UAC capture not ready after VBUS");
        return false;
#else
        set_detail("disabled by Kconfig");
        return false;
#endif
    }

    void stop(const char* reason)
    {
#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT
        if (!started_.load()) {
            dji_mic_usb_power_abort(reason != nullptr ? reason : "DJI input already stopped");
            return;
        }
        ESP_LOGW(TAG, "Stopping DJI Mic input: reason=%s", reason != nullptr ? reason : "-");
        esp_err_t err = usb_streaming_stop();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "usb_streaming_stop failed: %s", esp_err_to_name(err));
        }
        dji_mic_usb_power_abort(reason != nullptr ? reason : "DJI input stopped");
        stop_decode_task();
        connected_ = false;
        capture_ready_ = false;
        stream_stable_ = false;
        delete_ringbufs();
        set_detail("DJI Mic USB输入已停止");
#else
        (void)reason;
#endif
    }

    DjiMicReceiverStatus status() const
    {
        const bool connected = connected_.load();
        const bool ready = capture_ready_.load();
        usb_stream_device_info_t usb_info = {};
        const bool descriptor_valid = usb_streaming_device_info_get(&usb_info) == ESP_OK &&
                                      usb_info.descriptor_valid;
        const bool target_vid_pid = descriptor_valid &&
                                    usb_info.vendor_id == kDjiVid &&
                                    usb_info.product_id == kDjiPid;

        DjiMicReceiverStatus status;
        status.detected = connected || ready || (descriptor_valid && usb_info.connected);
        status.target_vid_pid = target_vid_pid;
        status.full_speed = descriptor_valid && usb_info.full_speed;
        status.audio_control = descriptor_valid && usb_info.audio_control;
        status.audio_streaming = ready || (descriptor_valid && usb_info.audio_streaming);
        status.capture_ready = ready;
        status.stream_stable = stream_stable_.load();
        status.identity_confirmed = target_vid_pid;
        status.vendor_id = descriptor_valid ? usb_info.vendor_id : 0;
        status.product_id = descriptor_valid ? usb_info.product_id : 0;
        status.sample_rate = usb_info.mic_sample_rate != 0 ? static_cast<int>(usb_info.mic_sample_rate)
                                                           : static_cast<int>(sample_rate_.load());
        status.channels = usb_info.mic_channels != 0 ? static_cast<int>(usb_info.mic_channels)
                                                     : static_cast<int>(channels_.load());
        status.bit_resolution = static_cast<int>(bit_resolution_.load());
        status.selected_channel = static_cast<int>(selected_channel_.load());
        status.connection_generation = connection_generation_.load();
        status.callback_count = callback_count_.load();
        status.callback_bytes = callback_bytes_.load();
        status.output_samples = output_samples_.load();
        status.raw_drops = raw_drops_.load();
        status.pcm_drops = pcm_drops_.load();
        status.format_errors = format_errors_.load();
        status.speed = descriptor_valid ? (usb_info.full_speed ? "Full-Speed" : "Low-Speed") : "";
        status.manufacturer = "";
        status.product = "";
        status.detail = detail_;
        return status;
    }

    bool enabled() const
    {
        return started_.load();
    }

    size_t read_16k(int16_t* out, size_t samples, TickType_t timeout)
    {
        if (out == nullptr || samples == 0 || pcm_ringbuf_ == nullptr) {
            return 0;
        }

        size_t copied = 0;
        TickType_t start_ticks = xTaskGetTickCount();
        while (copied < samples) {
            TickType_t wait_ticks = remaining_timeout(start_ticks, timeout);
            if (copied > 0 && wait_ticks == 0) {
                break;
            }

            size_t requested_bytes = (samples - copied) * sizeof(int16_t);
            if (requested_bytes == 0) {
                break;
            }
            size_t item_bytes = 0;
            auto* item = static_cast<uint8_t*>(
                xRingbufferReceiveUpTo(pcm_ringbuf_, &item_bytes, wait_ticks, requested_bytes));
            if (item == nullptr || item_bytes == 0) {
                break;
            }

            size_t item_samples = item_bytes / sizeof(int16_t);
            memcpy(out + copied, item, item_samples * sizeof(int16_t));
            copied += item_samples;
            vRingbufferReturnItem(pcm_ringbuf_, item);
        }
        return copied;
    }

private:
    static TickType_t remaining_timeout(TickType_t start_ticks, TickType_t timeout)
    {
        if (timeout == portMAX_DELAY) {
            return portMAX_DELAY;
        }
        if (timeout == 0) {
            return 0;
        }
        TickType_t elapsed = xTaskGetTickCount() - start_ticks;
        return elapsed >= timeout ? 0 : timeout - elapsed;
    }

    static void usb_state_cb(usb_stream_state_t state, void* arg)
    {
        auto* self = static_cast<DjiMicReceiverInput*>(arg);
        if (self != nullptr) {
            self->on_usb_state(state);
        }
    }

    static void mic_frame_cb(mic_frame_t* frame, void* arg)
    {
        auto* self = static_cast<DjiMicReceiverInput*>(arg);
        if (self != nullptr) {
            self->on_mic_frame(frame);
        }
    }

    void on_usb_state(usb_stream_state_t state)
    {
        if (state == STREAM_CONNECTED) {
            connected_ = true;
            connection_generation_.fetch_add(1);
            sample_rate_ = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE;
            bit_resolution_ = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS;
            channels_ = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS;
            capture_ready_ = false;
            stream_stable_ = false;
            selected_channel_ = 0;
            selected_channel_locked_ = false;
            stable_started_ticks_ = 0;
            last_valid_frame_ticks_ = 0;
            channel_energy_[0] = 0;
            channel_energy_[1] = 0;
            fir_history_.fill(0);
            fir_history_pos_ = 0;
            fir_decimation_phase_ = 0;
            set_detail("DJI Mic USB已接入，等待第一帧UAC音频");
            ESP_LOGI(TAG, "DJI Mic USB接收器已接入: source=dji_mic_receiver requested=%uHz %u-bit %u ch; 等待首帧UAC PCM后切换输入",
                     static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE),
                     static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS),
                     static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS));
            return;
        }

        connected_ = false;
        capture_ready_ = false;
        stream_stable_ = false;
        selected_channel_ = 0;
        selected_channel_locked_ = false;
        stable_started_ticks_ = 0;
        last_valid_frame_ticks_ = 0;
        drop_raw_ring();
        drop_pcm_ring();
        set_detail("DJI Mic USB已断开，等待重新接入");
        ESP_LOGW(TAG, "DJI Mic USB接收器已断开: source=dji_mic_receiver capture=0 raw_drops=%" PRIu32
                      " pcm_drops=%" PRIu32,
                 raw_drops_.load(), pcm_drops_.load());
    }

    void on_mic_frame(mic_frame_t* frame)
    {
        if (frame == nullptr || frame->data == nullptr || frame->data_bytes == 0 || raw_ringbuf_ == nullptr) {
            return;
        }

        RawUacFrameHeader header;
        header.data_bytes = frame->data_bytes;
        header.sample_rate = frame->samples_frequence != 0
                                 ? frame->samples_frequence
                                 : static_cast<uint32_t>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE);
        header.bit_resolution = frame->bit_resolution != 0
                                    ? frame->bit_resolution
                                    : static_cast<uint16_t>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS);
        header.channels = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS;

        const size_t packet_bytes = sizeof(header) + frame->data_bytes;
        void* item = nullptr;
        BaseType_t ok = xRingbufferSendAcquire(raw_ringbuf_, &item, packet_bytes, 0);
        if (ok != pdTRUE || item == nullptr) {
            raw_drops_.fetch_add(1);
            return;
        }

        auto* out = static_cast<uint8_t*>(item);
        memcpy(out, &header, sizeof(header));
        memcpy(out + sizeof(header), frame->data, frame->data_bytes);

        ok = xRingbufferSendComplete(raw_ringbuf_, item);
        if (ok != pdTRUE) {
            raw_drops_.fetch_add(1);
        }
    }

    void decode_task()
    {
        while (started_.load()) {
            size_t item_bytes = 0;
            auto* item = static_cast<uint8_t*>(
                xRingbufferReceive(raw_ringbuf_, &item_bytes, pdMS_TO_TICKS(100)));

            if (item == nullptr) {
                continue;
            }
            if (item_bytes <= sizeof(RawUacFrameHeader)) {
                vRingbufferReturnItem(raw_ringbuf_, item);
                continue;
            }

            RawUacFrameHeader header;
            memcpy(&header, item, sizeof(header));
            const uint8_t* data = item + sizeof(header);
            const size_t data_bytes = item_bytes - sizeof(header);

            process_raw_uac_frame(data, data_bytes, header.sample_rate,
                                  header.bit_resolution, header.channels);
            vRingbufferReturnItem(raw_ringbuf_, item);
        }
        decode_task_ = nullptr;
    }

    void process_raw_uac_frame(const uint8_t* data,
                               size_t data_bytes,
                               uint32_t input_rate,
                               uint16_t bits,
                               uint32_t channels)
    {
        uint32_t bytes_per_sample = bits / 8;
        if (data == nullptr || data_bytes == 0 || input_rate == 0 ||
            bytes_per_sample == 0 || channels == 0) {
            return;
        }

        uint32_t bytes_per_frame = bytes_per_sample * channels;
        if (bytes_per_frame == 0) {
            return;
        }
        uint32_t frames = data_bytes / bytes_per_frame;
        if (frames == 0) {
            return;
        }

        if (input_rate != CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE ||
            bits != CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS ||
            channels != CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS) {
            format_errors_.fetch_add(1);
            capture_ready_ = false;
            stream_stable_ = false;
            stable_started_ticks_ = 0;
            set_detail("DJI UAC格式不匹配: %uHz %u-bit %u ch",
                       static_cast<unsigned>(input_rate), static_cast<unsigned>(bits),
                       static_cast<unsigned>(channels));
            return;
        }

        sample_rate_ = input_rate;
        bit_resolution_ = bits;
        channels_ = channels;
        callback_count_.fetch_add(1);
        callback_bytes_.fetch_add(static_cast<uint32_t>(data_bytes));

        const TickType_t now = xTaskGetTickCount();
        if (stable_started_ticks_ == 0 ||
            (last_valid_frame_ticks_ != 0 && now - last_valid_frame_ticks_ > kStableGapToleranceTicks)) {
            stable_started_ticks_ = now;
            stream_stable_ = false;
            capture_ready_ = false;
            selected_channel_locked_ = false;
            channel_energy_[0] = 0;
            channel_energy_[1] = 0;
        }
        last_valid_frame_ticks_ = now;

        uint32_t selected_ch = 0;
        if (channels > 1) {
            int64_t ch_energy[2] = {0, 0};
            const uint32_t inspect_channels = std::min<uint32_t>(channels, 2);

            for (uint32_t i = 0; i < frames; ++i) {
                const uint8_t* frame_ptr = data + i * bytes_per_frame;
                for (uint32_t ch = 0; ch < inspect_channels; ++ch) {
                    const int32_t s = read_le_pcm_sample(frame_ptr + ch * bytes_per_sample, bits);
                    ch_energy[ch] += s < 0 ? -static_cast<int64_t>(s) : static_cast<int64_t>(s);
                }
            }

            channel_energy_[0] += ch_energy[0];
            channel_energy_[1] += ch_energy[1];

            uint32_t select_log_counter = ++channel_select_log_counter_;
            if ((select_log_counter % 100) == 1) {
                ESP_LOGI(TAG, "DJI Mic channel calibration: energy0=%lld energy1=%lld",
                         static_cast<long long>(ch_energy[0]),
                         static_cast<long long>(ch_energy[1]));
            }
        }

        const bool stable_now =
            now - stable_started_ticks_ >= pdMS_TO_TICKS(CONFIG_STACKCHAN_DJI_MIC_STABLE_MS);
        if (stable_now && !stream_stable_.exchange(true)) {
#if CONFIG_STACKCHAN_DJI_MIC_CHANNEL_RIGHT
            selected_ch = channels > 1 ? 1 : 0;
#elif CONFIG_STACKCHAN_DJI_MIC_CHANNEL_MIX
            selected_ch = 0;
#elif CONFIG_STACKCHAN_DJI_MIC_CHANNEL_AUTO_ONCE
            selected_ch = channels > 1 && channel_energy_[1] > channel_energy_[0] ? 1 : 0;
#else
            selected_ch = 0;
#endif
            selected_channel_ = selected_ch;
            selected_channel_locked_ = true;
            drop_pcm_ring();
            capture_ready_ = true;
            set_detail("DJI Mic UAC稳定采集中");
            ESP_LOGI(TAG,
                     "DJI Mic UAC稳定: %uHz %u-bit %u ch stable_ms=%d selected_ch=%u -> 16000Hz FIR mono",
                     static_cast<unsigned>(input_rate), static_cast<unsigned>(bits),
                     static_cast<unsigned>(channels), CONFIG_STACKCHAN_DJI_MIC_STABLE_MS,
                     static_cast<unsigned>(selected_ch));
        }
        if (!stream_stable_.load()) {
            return;
        }
        selected_ch = selected_channel_.load();

        int16_t out[kCallbackOutChunkSamples];
        size_t out_count = 0;
        for (uint32_t i = 0; i < frames; ++i) {
            const uint8_t* frame_ptr = data + i * bytes_per_frame;
            int32_t mono = read_le_pcm_sample(frame_ptr + selected_ch * bytes_per_sample, bits);
#if CONFIG_STACKCHAN_DJI_MIC_CHANNEL_MIX
            if (channels > 1) {
                const int32_t right = read_le_pcm_sample(frame_ptr + bytes_per_sample, bits);
                mono = (mono + right) / 2;
            }
#endif

            fir_history_[fir_history_pos_] = clamp_i16(mono);
            fir_history_pos_ = (fir_history_pos_ + 1) % fir_history_.size();
            fir_decimation_phase_ = (fir_decimation_phase_ + 1) % 3;
            if (fir_decimation_phase_ != 0) {
                continue;
            }

            int64_t filtered = 0;
            for (size_t tap = 0; tap < kDecimatorFirQ15.size(); ++tap) {
                const size_t history_index = (fir_history_pos_ + tap) % fir_history_.size();
                filtered += static_cast<int32_t>(fir_history_[history_index]) * kDecimatorFirQ15[tap];
            }
            int32_t sample = static_cast<int32_t>((filtered + (1 << 14)) >> 15);
            sample = sample * CONFIG_STACKCHAN_DJI_MIC_DIGITAL_GAIN_PERCENT / 100;
            out[out_count++] = clamp_i16(sample);
            if (out_count >= kCallbackOutChunkSamples) {
                push_output(out, out_count);
                out_count = 0;
            }
        }
        if (out_count > 0) {
            push_output(out, out_count);
        }
    }

    void push_output(const int16_t* samples, size_t count)
    {
        if (samples == nullptr || count == 0 || pcm_ringbuf_ == nullptr) {
            return;
        }
        const size_t bytes = count * sizeof(int16_t);
        if (xRingbufferSend(pcm_ringbuf_, const_cast<int16_t*>(samples), bytes, 0) == pdTRUE) {
            output_samples_.fetch_add(static_cast<uint32_t>(count));
            return;
        }

        size_t old_bytes = 0;
        void* old = xRingbufferReceiveUpTo(pcm_ringbuf_, &old_bytes, 0, bytes);
        if (old != nullptr) {
            pcm_drops_.fetch_add(static_cast<uint32_t>(old_bytes / sizeof(int16_t)));
            vRingbufferReturnItem(pcm_ringbuf_, old);
        }
        if (xRingbufferSend(pcm_ringbuf_, const_cast<int16_t*>(samples), bytes, 0) == pdTRUE) {
            output_samples_.fetch_add(static_cast<uint32_t>(count));
        } else {
            pcm_drops_.fetch_add(static_cast<uint32_t>(count));
        }
    }

    void drop_pcm_ring()
    {
        if (pcm_ringbuf_ == nullptr) {
            return;
        }
        size_t item_bytes = 0;
        while (void* item = xRingbufferReceiveUpTo(pcm_ringbuf_, &item_bytes, 0, 4096)) {
            pcm_drops_.fetch_add(static_cast<uint32_t>(item_bytes / sizeof(int16_t)));
            vRingbufferReturnItem(pcm_ringbuf_, item);
        }
    }

    void drop_raw_ring()
    {
        if (raw_ringbuf_ == nullptr) {
            return;
        }
        size_t item_bytes = 0;
        while (void* item = xRingbufferReceive(raw_ringbuf_, &item_bytes, 0)) {
            vRingbufferReturnItem(raw_ringbuf_, item);
        }
    }

    void stop_decode_task()
    {
        started_ = false;
        if (raw_ringbuf_ != nullptr) {
            uint8_t wake = 0;
            xRingbufferSend(raw_ringbuf_, &wake, sizeof(wake), 0);
        }
        for (int i = 0; decode_task_ != nullptr && i < 100; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (decode_task_ != nullptr) {
            ESP_LOGW(TAG, "DJI decode task did not exit; deleting");
            vTaskDeleteWithCaps(decode_task_);
            decode_task_ = nullptr;
        }
    }

    void delete_ringbufs()
    {
        if (pcm_ringbuf_ != nullptr) {
            vRingbufferDeleteWithCaps(pcm_ringbuf_);
            pcm_ringbuf_ = nullptr;
        }
        if (raw_ringbuf_ != nullptr && decode_task_ == nullptr) {
            vRingbufferDeleteWithCaps(raw_ringbuf_);
            raw_ringbuf_ = nullptr;
        }
    }

    void set_detail(const char* format, ...)
    {
        if (format == nullptr) {
            detail_[0] = '\0';
            return;
        }
        va_list args;
        va_start(args, format);
        vsnprintf(detail_, sizeof(detail_), format, args);
        va_end(args);
    }

    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> capture_ready_{false};
    std::atomic<bool> stream_stable_{false};
    std::atomic<uint32_t> sample_rate_{0};
    std::atomic<uint32_t> bit_resolution_{0};
    std::atomic<uint32_t> channels_{0};
    std::atomic<uint32_t> callback_count_{0};
    std::atomic<uint32_t> callback_bytes_{0};
    std::atomic<uint32_t> output_samples_{0};
    std::atomic<uint32_t> raw_drops_{0};
    std::atomic<uint32_t> pcm_drops_{0};
    std::atomic<uint32_t> format_errors_{0};
    std::atomic<uint32_t> connection_generation_{0};
    std::atomic<uint32_t> selected_channel_{0};
    std::atomic<bool> selected_channel_locked_{false};
    std::atomic<uint32_t> channel_select_log_counter_{0};
    RingbufHandle_t pcm_ringbuf_ = nullptr;
    RingbufHandle_t raw_ringbuf_ = nullptr;
    TaskHandle_t decode_task_ = nullptr;
    TickType_t stable_started_ticks_ = 0;
    TickType_t last_valid_frame_ticks_ = 0;
    int64_t channel_energy_[2] = {0, 0};
    std::array<int16_t, kDecimatorFirQ15.size()> fir_history_{};
    size_t fir_history_pos_ = 0;
    uint8_t fir_decimation_phase_ = 0;
    char detail_[128] = "未启动";
};

DjiMicReceiverInput g_dji_mic_receiver;

} // namespace

bool dji_mic_receiver_input_start()
{
    return g_dji_mic_receiver.start();
}

void dji_mic_receiver_input_stop(const char* reason)
{
    g_dji_mic_receiver.stop(reason);
}

bool dji_mic_receiver_input_is_enabled()
{
    return g_dji_mic_receiver.enabled();
}

DjiMicReceiverStatus dji_mic_receiver_input_status()
{
    return g_dji_mic_receiver.status();
}

size_t dji_mic_receiver_input_read_16k(int16_t* out, size_t samples, TickType_t timeout)
{
    return g_dji_mic_receiver.read_16k(out, samples, timeout);
}
