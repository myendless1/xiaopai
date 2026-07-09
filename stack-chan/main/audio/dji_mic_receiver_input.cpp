#include "dji_mic_receiver_input.h"

#include <M5Unified.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/ringbuf.h"
#include "sdkconfig.h"
#include "usb_stream.h"
#include "xiaopai_state.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <inttypes.h>

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

namespace {

static constexpr const char* TAG = "DjiMicInput";
static constexpr uint16_t kDjiVid = 0x2ca3;
static constexpr uint16_t kDjiPid = 0x4011;
static constexpr uint32_t kOutputSampleRate = 16000;
static constexpr size_t kCallbackOutChunkSamples = 512;
static constexpr int kQuietBeforeVbusMs = 500;
static constexpr int kVbusSettleBeforeStreamMs = 800;

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
        if (started_.exchange(true)) {
            return true;
        }

        pcm_ringbuf_ = xRingbufferCreate(CONFIG_STACKCHAN_DJI_MIC_UAC_RINGBUF_BYTES, RINGBUF_TYPE_BYTEBUF);
        if (pcm_ringbuf_ == nullptr) {
            set_detail("DJI Mic PCM ringbuffer alloc failed");
            started_ = false;
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }

        raw_ringbuf_ = xRingbufferCreate(CONFIG_STACKCHAN_DJI_MIC_UAC_RINGBUF_BYTES, RINGBUF_TYPE_NOSPLIT);
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
        resample_accum_ = 0;
        callback_count_ = 0;
        callback_bytes_ = 0;
        output_samples_ = 0;
        dropped_samples_ = 0;

        uac_config_t uac_config = {};
        uac_config.mic_ch_num = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS;
        uac_config.mic_bit_resolution = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS;
        uac_config.mic_samples_frequence = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE;
        uac_config.mic_buf_size = CONFIG_STACKCHAN_DJI_MIC_UAC_INTERNAL_BUF_BYTES;
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

        BaseType_t task_ok = xTaskCreatePinnedToCore(
            [](void* arg) {
                static_cast<DjiMicReceiverInput*>(arg)->decode_task();
                vTaskDelete(nullptr);
            },
            "dji_uac_decode",
            8192,
            this,
            4,
            &decode_task_,
            0);
        if (task_ok != pdPASS) {
            set_detail("DJI Mic decode task create failed");
            started_ = false;
            delete_ringbufs();
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }

        set_detail("DJI Mic USB输入已启动，等待首帧UAC PCM");
        xiaopai_state_set(LocalVoiceState::Waiting, "dji input selected");
        ESP_LOGI(TAG, "DJI Mic USB输入已启动: source=dji_mic_receiver; 收到UAC首帧后切换主输入");

#if CONFIG_STACKCHAN_DJI_MIC_DRIVE_VBUS
        M5.Power.setUsbOutput(false);
        ESP_LOGI(TAG, "DJI Mic USB bring-up: quiet %dms before VBUS", kQuietBeforeVbusMs);
        vTaskDelay(pdMS_TO_TICKS(kQuietBeforeVbusMs));

        set_detail("打开CoreS3 USB VBUS输出");
        ESP_LOGI(TAG, "打开CoreS3 USB VBUS输出，准备给DJI Mic接收器供电");
        M5.Power.setUsbOutput(true);
        ESP_LOGI(TAG, "DJI Mic USB bring-up: VBUS settle %dms before usb_streaming_start",
                 kVbusSettleBeforeStreamMs);
        vTaskDelay(pdMS_TO_TICKS(kVbusSettleBeforeStreamMs));
#else
        ESP_LOGW(TAG, "Skip CoreS3 USB VBUS drive; expect external host-power-safe wiring");
#endif

        set_detail("启动usb_stream，等待DJI Mic USB枚举");
        err = usb_streaming_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            set_detail("usb_stream start failed: %s", esp_err_to_name(err));
            stop_decode_task();
            delete_ringbufs();
            ESP_LOGE(TAG, "%s", detail_);
            return false;
        }

        set_detail("等待DJI Mic USB接收器接入；首帧UAC PCM后切换输入");
        ESP_LOGI(TAG, "DJI Mic UAC输入已启动: requested=%uHz %u-bit %u ch, output=16000Hz mono",
                 static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE),
                 static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS),
                 static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS));
        return true;
#else
        set_detail("disabled by Kconfig");
        return false;
#endif
    }

    DjiMicReceiverStatus status() const
    {
        const bool connected = connected_.load();
        const bool ready = capture_ready_.load();

        DjiMicReceiverStatus status;
        status.detected = connected || ready;
        status.target_vid_pid = connected || ready;
        status.full_speed = connected || ready;
        status.audio_control = connected || ready;
        status.audio_streaming = connected || ready;
        status.capture_ready = ready;
        status.identity_confirmed = connected || ready;
        status.vendor_id = connected || ready ? kDjiVid : 0;
        status.product_id = connected || ready ? kDjiPid : 0;
        status.sample_rate = sample_rate_.load();
        status.channels = channels_.load();
        status.speed = connected || ready ? "Full-Speed" : "";
        status.manufacturer = connected || ready ? "DJI" : "";
        status.product = connected || ready ? "DJI Mic Receiver" : "";
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
            sample_rate_ = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE;
            bit_resolution_ = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS;
            channels_ = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS;
            resample_accum_ = 0;
            set_detail("DJI Mic USB已接入，等待第一帧UAC音频");
            xiaopai_state_set(LocalVoiceState::Waiting, "dji usb attach");
            ESP_LOGI(TAG, "DJI Mic USB接收器已接入: source=dji_mic_receiver requested=%uHz %u-bit %u ch; 等待首帧UAC PCM后切换输入",
                     static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE),
                     static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS),
                     static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS));
            return;
        }

        connected_ = false;
        capture_ready_ = false;
        resample_accum_ = 0;
        drop_raw_ring();
        drop_pcm_ring();
        set_detail("DJI Mic USB已断开，等待重新接入");
        xiaopai_state_set(LocalVoiceState::Waiting, "dji usb detached");
        ESP_LOGW(TAG, "DJI Mic USB接收器已断开: source=dji_mic_receiver capture=0 dropped=%" PRIu32,
                 dropped_samples_.load());
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
            dropped_samples_.fetch_add(1);
            return;
        }

        auto* out = static_cast<uint8_t*>(item);
        memcpy(out, &header, sizeof(header));
        memcpy(out + sizeof(header), frame->data, frame->data_bytes);

        ok = xRingbufferSendComplete(raw_ringbuf_, item);
        if (ok != pdTRUE) {
            dropped_samples_.fetch_add(1);
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

        sample_rate_ = input_rate;
        bit_resolution_ = bits;
        channels_ = channels;
        callback_count_.fetch_add(1);
        callback_bytes_.fetch_add(static_cast<uint32_t>(data_bytes));

        if (!capture_ready_.exchange(true)) {
            set_detail("DJI Mic UAC采集中：完全使用USB麦克风");
            xiaopai_state_set(LocalVoiceState::Listening, "dji mic ready");
            ESP_LOGI(TAG, "DJI Mic UAC首帧: %uHz %u-bit %u ch bytes=%u -> 16000Hz mono; 内置麦克风输入已停用",
                     static_cast<unsigned>(input_rate),
                     static_cast<unsigned>(bits),
                     static_cast<unsigned>(channels),
                     static_cast<unsigned>(data_bytes));
        }

        int16_t out[kCallbackOutChunkSamples];
        size_t out_count = 0;
        for (uint32_t i = 0; i < frames; ++i) {
            const uint8_t* frame_ptr = data + i * bytes_per_frame;
            int64_t mono = 0;
            for (uint32_t ch = 0; ch < channels; ++ch) {
                mono += read_le_pcm_sample(frame_ptr + ch * bytes_per_sample, bits);
            }
            mono /= static_cast<int64_t>(channels);

            resample_accum_ += kOutputSampleRate;
            if (resample_accum_ < input_rate) {
                continue;
            }
            resample_accum_ -= input_rate;

            out[out_count++] = clamp_i16(static_cast<int32_t>(mono));
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
            dropped_samples_.fetch_add(static_cast<uint32_t>(old_bytes / sizeof(int16_t)));
            vRingbufferReturnItem(pcm_ringbuf_, old);
        }
        if (xRingbufferSend(pcm_ringbuf_, const_cast<int16_t*>(samples), bytes, 0) == pdTRUE) {
            output_samples_.fetch_add(static_cast<uint32_t>(count));
        } else {
            dropped_samples_.fetch_add(static_cast<uint32_t>(count));
        }
    }

    void drop_pcm_ring()
    {
        if (pcm_ringbuf_ == nullptr) {
            return;
        }
        size_t item_bytes = 0;
        while (void* item = xRingbufferReceiveUpTo(pcm_ringbuf_, &item_bytes, 0, 4096)) {
            dropped_samples_.fetch_add(static_cast<uint32_t>(item_bytes / sizeof(int16_t)));
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
        for (int i = 0; decode_task_ != nullptr && i < 20; ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    void delete_ringbufs()
    {
        if (pcm_ringbuf_ != nullptr) {
            vRingbufferDelete(pcm_ringbuf_);
            pcm_ringbuf_ = nullptr;
        }
        if (raw_ringbuf_ != nullptr && decode_task_ == nullptr) {
            vRingbufferDelete(raw_ringbuf_);
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
    std::atomic<uint32_t> sample_rate_{0};
    std::atomic<uint32_t> bit_resolution_{0};
    std::atomic<uint32_t> channels_{0};
    std::atomic<uint32_t> callback_count_{0};
    std::atomic<uint32_t> callback_bytes_{0};
    std::atomic<uint32_t> output_samples_{0};
    std::atomic<uint32_t> dropped_samples_{0};
    RingbufHandle_t pcm_ringbuf_ = nullptr;
    RingbufHandle_t raw_ringbuf_ = nullptr;
    TaskHandle_t decode_task_ = nullptr;
    uint32_t resample_accum_ = 0;
    char detail_[128] = "未启动";
};

DjiMicReceiverInput g_dji_mic_receiver;

} // namespace

bool dji_mic_receiver_input_start()
{
    return g_dji_mic_receiver.start();
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
