#include "dji_mic_uac_recorder.h"

#include <M5Unified.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "usb_stream.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <inttypes.h>

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD
#define CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD 0
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

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_SECONDS
#define CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_SECONDS 5
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_RINGBUF_BYTES
#define CONFIG_STACKCHAN_DJI_MIC_UAC_RINGBUF_BYTES 65536
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_INTERNAL_BUF_BYTES
#define CONFIG_STACKCHAN_DJI_MIC_UAC_INTERNAL_BUF_BYTES 16384
#endif

#ifndef CONFIG_STACKCHAN_RECORD_UPLOAD_URL
#define CONFIG_STACKCHAN_RECORD_UPLOAD_URL "http://192.168.21.15:8091/upload-audio?save_only=1"
#endif

#ifndef CONFIG_STACKCHAN_WIFI_SSID
#define CONFIG_STACKCHAN_WIFI_SSID ""
#endif

#ifndef CONFIG_STACKCHAN_WIFI_PASSWORD
#define CONFIG_STACKCHAN_WIFI_PASSWORD ""
#endif

namespace {

static constexpr const char* TAG = "DjiMicUacRec";
static constexpr uint32_t kWavHeaderBytes = 44;
static constexpr int kHttpBufferSize = 4096;
static constexpr int kUploadTimeoutMs = 30000;
static constexpr int kWifiTimeoutMs = 20000;
static constexpr int kWifiRetryLimit = 6;
static constexpr int kUsbConnectTimeoutMs = 15000;
static constexpr int kFirstFrameTimeoutMs = 8000;
static constexpr int kCaptureTimeoutSlackMs = 8000;
static constexpr uint32_t kMaxUacBitsPerSample = 24;
static constexpr uint32_t kTraceFrameCount = 50;
static constexpr uint32_t kTraceHeadBytes = 16;
static constexpr EventBits_t kWifiConnectedBit = BIT0;
static constexpr EventBits_t kWifiFailedBit = BIT1;

static_assert(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS % 8 == 0,
              "WAV writer expects whole-byte PCM samples");

static std::atomic<bool> s_started{false};
static std::atomic<bool> s_wifi_connected{false};
static std::atomic<bool> s_usb_connected{false};
static std::atomic<bool> s_uploading{false};
static std::atomic<bool> s_done{false};
static std::atomic<bool> s_failed{false};
static std::atomic<uint32_t> s_target_bytes{0};
static std::atomic<uint32_t> s_captured_bytes{0};
static std::atomic<uint32_t> s_bytes_sent{0};
static std::atomic<uint32_t> s_callback_bytes{0};
static std::atomic<uint32_t> s_dropped_bytes{0};
static std::atomic<uint32_t> s_callback_count{0};
static std::atomic<uint32_t> s_effective_sample_rate{CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE};
static std::atomic<uint32_t> s_effective_bit_resolution{CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS};
static std::atomic<uint32_t> s_effective_channels{CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS};
static std::atomic<int> s_http_status{0};
static portMUX_TYPE s_text_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_url[128] = CONFIG_STACKCHAN_RECORD_UPLOAD_URL;
static char s_detail[128] = "not started";
static uint8_t* s_capture_buf = nullptr;
static uint32_t s_capture_capacity = 0;
static std::atomic<bool> s_capture_done{false};

struct MicFrameTrace {
    uint16_t bit_resolution = 0;
    uint32_t sample_rate = 0;
    uint32_t bytes = 0;
    uint8_t mod2 = 0;
    uint8_t mod3 = 0;
    uint8_t mod4 = 0;
    uint8_t mod6 = 0;
    uint8_t head_len = 0;
    uint8_t head[kTraceHeadBytes] = {};
};

static MicFrameTrace s_trace[kTraceFrameCount];
static std::atomic<uint32_t> s_trace_count{0};

static EventGroupHandle_t s_wifi_event_group = nullptr;
static bool s_wifi_netif_created = false;
static bool s_wifi_started = false;
static bool s_wifi_handlers_registered = false;
static int s_wifi_retry_count = 0;

static bool http_write_all(esp_http_client_handle_t client, const uint8_t* data, size_t len);

static void set_detail(const char* format, ...)
{
    char tmp[sizeof(s_detail)] = {};
    if (format != nullptr) {
        va_list args;
        va_start(args, format);
        vsnprintf(tmp, sizeof(tmp), format, args);
        va_end(args);
    }

    portENTER_CRITICAL(&s_text_mux);
    strlcpy(s_detail, tmp, sizeof(s_detail));
    portEXIT_CRITICAL(&s_text_mux);
}

static void set_url(const char* url)
{
    portENTER_CRITICAL(&s_text_mux);
    strlcpy(s_url, url != nullptr ? url : "", sizeof(s_url));
    portEXIT_CRITICAL(&s_text_mux);
}

static void put_le16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xff);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
}

static void put_le32(uint8_t* out, uint32_t value)
{
    out[0] = static_cast<uint8_t>(value & 0xff);
    out[1] = static_cast<uint8_t>((value >> 8) & 0xff);
    out[2] = static_cast<uint8_t>((value >> 16) & 0xff);
    out[3] = static_cast<uint8_t>((value >> 24) & 0xff);
}

static uint32_t pcm_bytes_for(uint32_t sample_rate, uint32_t bit_resolution, uint32_t channels, uint32_t seconds)
{
    if (sample_rate == 0 || bit_resolution == 0 || channels == 0 || seconds == 0 ||
        (bit_resolution % 8) != 0) {
        return 0;
    }
    return sample_rate * channels * (bit_resolution / 8) * seconds;
}

static void reset_effective_format()
{
    s_effective_sample_rate = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE;
    s_effective_bit_resolution = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS;
    s_effective_channels = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS;
}

static void lock_effective_format_from_frame(const mic_frame_t* frame)
{
    if (frame == nullptr) {
        return;
    }

    uint32_t sample_rate = frame->samples_frequence != 0
                               ? frame->samples_frequence
                               : static_cast<uint32_t>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE);
    uint32_t bit_resolution = frame->bit_resolution != 0
                                  ? frame->bit_resolution
                                  : static_cast<uint32_t>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS);
    if ((bit_resolution % 8) != 0) {
        bit_resolution = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS;
    }
    uint32_t channels = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS;

    s_effective_sample_rate = sample_rate;
    s_effective_bit_resolution = bit_resolution;
    s_effective_channels = channels;

    uint32_t target = pcm_bytes_for(sample_rate, bit_resolution, channels,
                                    CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_SECONDS);
    if (target > s_capture_capacity) {
        target = s_capture_capacity;
    }
    if (target > 0) {
        s_target_bytes = target;
    }
}

static void build_wav_header(uint8_t* header, uint32_t pcm_bytes)
{
    const uint32_t sample_rate = s_effective_sample_rate.load();
    const uint16_t channels = static_cast<uint16_t>(s_effective_channels.load());
    const uint16_t bits = static_cast<uint16_t>(s_effective_bit_resolution.load());
    const uint16_t block_align = static_cast<uint16_t>(channels * bits / 8);
    const uint32_t byte_rate = sample_rate * block_align;

    memset(header, 0, kWavHeaderBytes);
    memcpy(header + 0, "RIFF", 4);
    put_le32(header + 4, 36 + pcm_bytes);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    put_le32(header + 16, 16);
    put_le16(header + 20, 1);
    put_le16(header + 22, channels);
    put_le32(header + 24, sample_rate);
    put_le32(header + 28, byte_rate);
    put_le16(header + 32, block_align);
    put_le16(header + 34, bits);
    memcpy(header + 36, "data", 4);
    put_le32(header + 40, pcm_bytes);
}

static void mac_address(char* out, size_t out_size)
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_size, "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void bytes_to_hex(const uint8_t* data, size_t len, char* out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    size_t offset = 0;
    for (size_t i = 0; i < len && offset + 3 < out_size; ++i) {
        offset += snprintf(out + offset, out_size - offset, "%02x", data[i]);
    }
    out[offset < out_size ? offset : out_size - 1] = '\0';
}

static void capture_trace_frame(const mic_frame_t* frame)
{
    uint32_t index = s_trace_count.fetch_add(1);
    if (index == 0) {
        lock_effective_format_from_frame(frame);
    }
    if (index >= kTraceFrameCount) {
        return;
    }

    MicFrameTrace& item = s_trace[index];
    item.bit_resolution = frame->bit_resolution;
    item.sample_rate = frame->samples_frequence;
    item.bytes = frame->data_bytes;
    item.mod2 = static_cast<uint8_t>(frame->data_bytes % 2);
    item.mod3 = static_cast<uint8_t>(frame->data_bytes % 3);
    item.mod4 = static_cast<uint8_t>(frame->data_bytes % 4);
    item.mod6 = static_cast<uint8_t>(frame->data_bytes % 6);
    item.head_len = static_cast<uint8_t>(frame->data_bytes < kTraceHeadBytes ? frame->data_bytes : kTraceHeadBytes);
    if (item.head_len > 0) {
        memcpy(item.head, frame->data, item.head_len);
    }
}

static bool appendf(char* out, size_t out_size, size_t& offset, const char* format, ...)
{
    if (offset >= out_size) {
        return false;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(out + offset, out_size - offset, format, args);
    va_end(args);
    if (written < 0 || static_cast<size_t>(written) >= out_size - offset) {
        offset = out_size;
        return false;
    }
    offset += static_cast<size_t>(written);
    return true;
}

static void make_audio_test_name(char* out, size_t out_size)
{
    snprintf(out, out_size, "dji_%luk_s%u_%s",
             static_cast<unsigned long>(s_effective_sample_rate.load() / 1000),
             static_cast<unsigned>(s_effective_bit_resolution.load()),
             s_effective_channels.load() == 1 ? "mono" : "stereo");
}

static bool make_device_logs_url(char* out, size_t out_size)
{
    const char* url = CONFIG_STACKCHAN_RECORD_UPLOAD_URL;
    const char* scheme = strstr(url, "://");
    if (scheme == nullptr) {
        return false;
    }
    const char* host_start = scheme + 3;
    const char* path_start = strchr(host_start, '/');
    size_t prefix_len = path_start != nullptr ? static_cast<size_t>(path_start - url) : strlen(url);
    if (prefix_len + strlen("/device/logs") + 1 > out_size) {
        return false;
    }
    memcpy(out, url, prefix_len);
    strcpy(out + prefix_len, "/device/logs");
    return true;
}

static bool http_post_json(const char* url, const char* body, size_t body_len)
{
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kUploadTimeoutMs;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;
    config.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }
    bool ok = http_write_all(client, reinterpret_cast<const uint8_t*>(body), body_len);
    if (ok) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ok = status >= 200 && status < 300;
        ESP_LOGI(TAG, "Trace log POST status=%d", status);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static void post_trace_logs()
{
    char url[128] = {};
    if (!make_device_logs_url(url, sizeof(url))) {
        ESP_LOGW(TAG, "Could not build device logs URL");
        return;
    }

    char device_id[16] = {};
    mac_address(device_id, sizeof(device_id));
    char test_name[40] = {};
    make_audio_test_name(test_name, sizeof(test_name));

    constexpr size_t kTraceJsonBytes = 14000;
    char* json = static_cast<char*>(heap_caps_malloc(kTraceJsonBytes, MALLOC_CAP_8BIT));
    if (json == nullptr) {
        ESP_LOGW(TAG, "Trace JSON alloc failed");
        return;
    }

    uint32_t trace_total = s_trace_count.load();
    uint32_t trace_emit = trace_total < kTraceFrameCount ? trace_total : kTraceFrameCount;
    size_t offset = 0;
    bool ok = appendf(json, kTraceJsonBytes, offset,
                      "{\"device_id\":\"%s\",\"events\":["
                      "{\"type\":\"dji_uac_summary\",\"test\":\"%s\",\"cfg_rate\":%u,\"cfg_bit\":%u,\"cfg_ch\":%u,"
                      "\"wav_rate\":%" PRIu32 ",\"wav_bit\":%" PRIu32 ",\"wav_ch\":%" PRIu32 ","
                      "\"target_bytes\":%" PRIu32 ",\"captured_bytes\":%" PRIu32 ",\"callback_count\":%" PRIu32 ","
                      "\"callback_bytes\":%" PRIu32 ",\"trace_total\":%" PRIu32 "}",
                      device_id, test_name,
                      static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE),
                      static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS),
                      static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS),
                      s_effective_sample_rate.load(),
                      s_effective_bit_resolution.load(),
                      s_effective_channels.load(),
                      s_target_bytes.load(), s_captured_bytes.load(), s_callback_count.load(),
                      s_callback_bytes.load(), trace_total);

    for (uint32_t i = 0; ok && i < trace_emit; ++i) {
        char head_hex[kTraceHeadBytes * 2 + 1] = {};
        bytes_to_hex(s_trace[i].head, s_trace[i].head_len, head_hex, sizeof(head_hex));
        ok = appendf(json, kTraceJsonBytes, offset,
                     ",{\"type\":\"dji_uac_frame\",\"test\":\"%s\",\"index\":%" PRIu32 ","
                     "\"frame_bit\":%u,\"frame_rate\":%" PRIu32 ",\"bytes\":%" PRIu32 ","
                     "\"mod2\":%u,\"mod3\":%u,\"mod4\":%u,\"mod6\":%u,\"head\":\"%s\"}",
                     test_name, i,
                     static_cast<unsigned>(s_trace[i].bit_resolution),
                     s_trace[i].sample_rate,
                     s_trace[i].bytes,
                     static_cast<unsigned>(s_trace[i].mod2),
                     static_cast<unsigned>(s_trace[i].mod3),
                     static_cast<unsigned>(s_trace[i].mod4),
                     static_cast<unsigned>(s_trace[i].mod6),
                     head_hex);
    }

    ok = ok && appendf(json, kTraceJsonBytes, offset, "]}");
    if (ok) {
        if (http_post_json(url, json, offset)) {
            ESP_LOGI(TAG, "Posted %" PRIu32 " DJI UAC trace frames", trace_emit);
        } else {
            ESP_LOGW(TAG, "DJI UAC trace POST failed");
        }
    }
    heap_caps_free(json);
}

static void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        s_wifi_connected = false;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d", event != nullptr ? event->reason : -1);
        if (s_wifi_retry_count++ < kWifiRetryLimit) {
            esp_wifi_connect();
        } else if (s_wifi_event_group != nullptr) {
            xEventGroupSetBits(s_wifi_event_group, kWifiFailedBit);
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry_count = 0;
        s_wifi_connected = true;
        if (s_wifi_event_group != nullptr) {
            xEventGroupSetBits(s_wifi_event_group, kWifiConnectedBit);
        }
    }
}

static bool ensure_wifi_connected()
{
    if (s_wifi_connected.load()) {
        return true;
    }
    if (CONFIG_STACKCHAN_WIFI_SSID[0] == '\0') {
        set_detail("WiFi SSID not configured");
        return false;
    }

    if (s_wifi_event_group == nullptr) {
        s_wifi_event_group = xEventGroupCreate();
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        set_detail("esp_netif_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        set_detail("event loop failed: %s", esp_err_to_name(err));
        return false;
    }

    if (!s_wifi_netif_created) {
        esp_netif_create_default_wifi_sta();
        s_wifi_netif_created = true;
    }

    if (!s_wifi_started) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            set_detail("esp_wifi_init failed: %s", esp_err_to_name(err));
            return false;
        }
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    }

    if (!s_wifi_handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler, nullptr, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &wifi_event_handler, nullptr, nullptr));
        s_wifi_handlers_registered = true;
    }

    wifi_config_t wifi_config = {};
    strlcpy(reinterpret_cast<char*>(wifi_config.sta.ssid), CONFIG_STACKCHAN_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy(reinterpret_cast<char*>(wifi_config.sta.password), CONFIG_STACKCHAN_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (CONFIG_STACKCHAN_WIFI_PASSWORD[0] == '\0') {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    xEventGroupClearBits(s_wifi_event_group, kWifiConnectedBit | kWifiFailedBit);
    s_wifi_retry_count = 0;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            set_detail("esp_wifi_start failed: %s", esp_err_to_name(err));
            return false;
        }
        s_wifi_started = true;
    } else {
        esp_wifi_connect();
    }

    set_detail("connecting WiFi: %s", CONFIG_STACKCHAN_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, kWifiConnectedBit | kWifiFailedBit,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(kWifiTimeoutMs));
    if ((bits & kWifiConnectedBit) == 0) {
        set_detail("WiFi connect failed");
        return false;
    }
    return true;
}

static void usb_state_cb(usb_stream_state_t state, void*)
{
    if (state == STREAM_CONNECTED) {
        s_usb_connected = true;
        set_detail("USB UAC connected");
        ESP_LOGI(TAG, "usb_stream connected");
    } else {
        s_usb_connected = false;
        set_detail("USB disconnected");
        ESP_LOGW(TAG, "usb_stream disconnected");
    }
}

static void mic_frame_cb(mic_frame_t* frame, void*)
{
    if (frame == nullptr || frame->data == nullptr || frame->data_bytes == 0 ||
        s_capture_buf == nullptr || s_capture_capacity == 0 || s_capture_done.load()) {
        return;
    }

    s_callback_count.fetch_add(1);
    s_callback_bytes.fetch_add(frame->data_bytes);
    capture_trace_frame(frame);

    uint32_t target_bytes = s_target_bytes.load(std::memory_order_acquire);
    if (target_bytes == 0 || target_bytes > s_capture_capacity) {
        target_bytes = s_capture_capacity;
    }

    uint32_t offset = s_captured_bytes.load(std::memory_order_relaxed);
    if (offset >= target_bytes) {
        s_capture_done = true;
        return;
    }

    size_t to_copy = frame->data_bytes;
    uint32_t remaining = target_bytes - offset;
    if (to_copy > remaining) {
        to_copy = remaining;
    }

    memcpy(s_capture_buf + offset, frame->data, to_copy);
    uint32_t captured = offset + static_cast<uint32_t>(to_copy);
    s_captured_bytes.store(captured, std::memory_order_release);
    if (captured >= target_bytes) {
        s_capture_done = true;
    }
}

static esp_err_t start_usb_stream()
{
    ESP_LOGI(TAG, "Opening CoreS3 USB VBUS output");
    M5.Power.setUsbOutput(true);
    vTaskDelay(pdMS_TO_TICKS(200));

    uac_config_t uac_config = {};
    uac_config.mic_ch_num = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS;
    uac_config.mic_bit_resolution = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS;
    uac_config.mic_samples_frequence = CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE;
    uac_config.mic_buf_size = CONFIG_STACKCHAN_DJI_MIC_UAC_INTERNAL_BUF_BYTES;
    uac_config.mic_cb = mic_frame_cb;
    uac_config.mic_cb_arg = nullptr;

    esp_err_t err = usb_streaming_state_register(usb_state_cb, nullptr);
    if (err != ESP_OK) {
        set_detail("usb_stream state cb failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uac_streaming_config(&uac_config);
    if (err != ESP_OK) {
        set_detail("UAC config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = usb_streaming_start();
    if (err != ESP_OK) {
        set_detail("usb_stream start failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static bool http_write_all(esp_http_client_handle_t client, const uint8_t* data, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > kHttpBufferSize) {
            chunk = kHttpBufferSize;
        }
        int written = esp_http_client_write(client, reinterpret_cast<const char*>(data + offset), chunk);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

static bool upload_wav_buffer(const uint8_t* pcm, uint32_t pcm_bytes)
{
    if (pcm == nullptr || pcm_bytes == 0) {
        set_detail("no captured PCM to upload");
        return false;
    }

    char device_id[16] = {};
    mac_address(device_id, sizeof(device_id));

    set_url(CONFIG_STACKCHAN_RECORD_UPLOAD_URL);
    esp_http_client_config_t config = {};
    config.url = CONFIG_STACKCHAN_RECORD_UPLOAD_URL;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kUploadTimeoutMs;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;
    config.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_detail("esp_http_client_init failed");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Device-Id", device_id);
    esp_http_client_set_header(client, "X-Client-Id", device_id);
    esp_http_client_set_header(client, "X-Upload-Source", "dji-uac-usb-stream");
    esp_http_client_set_header(client, "X-Upload-Mode", "save-only");
    esp_http_client_set_header(client, "X-Save-Only", "1");
    esp_http_client_set_header(client, "X-Save-Raw", "1");
    char test_name[40] = {};
    make_audio_test_name(test_name, sizeof(test_name));
    esp_http_client_set_header(client, "X-Audio-Test-Name", test_name);

    esp_err_t err = esp_http_client_open(client, kWavHeaderBytes + pcm_bytes);
    if (err != ESP_OK) {
        set_detail("HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    uint8_t wav_header[kWavHeaderBytes] = {};
    build_wav_header(wav_header, pcm_bytes);
    if (!http_write_all(client, wav_header, sizeof(wav_header))) {
        set_detail("HTTP header write failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    s_uploading = true;
    set_detail("uploading WAV to server");
    ESP_LOGI(TAG, "Uploading DJI UAC WAV to %s, pcm=%" PRIu32 " bytes", CONFIG_STACKCHAN_RECORD_UPLOAD_URL, pcm_bytes);

    uint32_t offset = 0;
    while (offset < pcm_bytes && !s_failed.load()) {
        uint32_t remaining = pcm_bytes - offset;
        size_t to_send = remaining;
        if (to_send > kHttpBufferSize) {
            to_send = kHttpBufferSize;
        }

        bool ok = http_write_all(client, pcm + offset, to_send);
        if (!ok) {
            s_failed = true;
            set_detail("HTTP audio write failed");
            break;
        }

        offset += static_cast<uint32_t>(to_send);
        s_bytes_sent = offset;
        if ((offset % (128 * 1024)) < to_send) {
            ESP_LOGI(TAG, "HTTP WAV progress: %" PRIu32 "/%" PRIu32 " bytes",
                     offset, pcm_bytes);
        }
    }

    s_uploading = false;

    if (!s_failed.load()) {
        int content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        s_http_status = status;
        ESP_LOGI(TAG, "Upload HTTP status=%d content_length=%d", status, content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        if (status >= 200 && status < 300) {
            s_done = true;
            set_detail("server saved WAV, HTTP %d", status);
            return true;
        }
        set_detail("server returned HTTP %d", status);
        return false;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
}

static bool capture_wav_to_memory()
{
    s_capture_done = false;
    s_usb_connected = false;
    s_captured_bytes = 0;
    s_callback_bytes = 0;
    s_callback_count = 0;
    s_dropped_bytes = 0;
    s_trace_count = 0;
    memset(s_trace, 0, sizeof(s_trace));

    esp_err_t err = start_usb_stream();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s", s_detail);
        return false;
    }

    set_detail("waiting for DJI Mic USB");
    TickType_t wait_start_ticks = xTaskGetTickCount();
    while (!s_usb_connected.load() && !s_failed.load()) {
        if (xTaskGetTickCount() - wait_start_ticks > pdMS_TO_TICKS(kUsbConnectTimeoutMs)) {
            usb_streaming_stop();
            set_detail("USB wait timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    set_detail("waiting for first UAC frame");
    wait_start_ticks = xTaskGetTickCount();
    while (s_captured_bytes.load() == 0 && !s_failed.load()) {
        if (!s_usb_connected.load()) {
            usb_streaming_stop();
            set_detail("USB disconnected before audio");
            return false;
        }
        if (xTaskGetTickCount() - wait_start_ticks > pdMS_TO_TICKS(kFirstFrameTimeoutMs)) {
            usb_streaming_stop();
            set_detail("UAC frame wait timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    set_detail("capturing DJI Mic to PSRAM");
    const TickType_t start_ticks = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(
        CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_SECONDS * 1000 + kCaptureTimeoutSlackMs);

    while (!s_capture_done.load() && !s_failed.load()) {
        if (xTaskGetTickCount() - start_ticks > timeout_ticks) {
            set_detail("capture timeout, saving partial WAV");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    usb_streaming_stop();
    s_usb_connected = false;

    uint32_t captured = s_captured_bytes.load();
    if (captured == 0) {
        set_detail("captured 0 bytes");
        return false;
    }

    if (captured < s_target_bytes.load()) {
        set_detail("captured partial: %" PRIu32 " bytes", captured);
    } else {
        set_detail("capture complete: %" PRIu32 " bytes", captured);
    }
    ESP_LOGI(TAG, "DJI UAC capture done: captured=%" PRIu32 " callback=%" PRIu32 " frames=%" PRIu32,
             captured, s_callback_bytes.load(), s_callback_count.load());
    return true;
}

static void recorder_task(void*)
{
    ESP_LOGI(TAG, "DJI Mic UAC Wi-Fi upload mode: %u Hz %u-bit %u ch, %u seconds",
             static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE),
             static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS),
             static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS),
             static_cast<unsigned>(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_SECONDS));

    reset_effective_format();
    s_target_bytes = pcm_bytes_for(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE,
                                   CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_BITS,
                                   CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS,
                                   CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_SECONDS);
    s_capture_capacity = pcm_bytes_for(CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_RATE,
                                       kMaxUacBitsPerSample,
                                       CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_CHANNELS,
                                       CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD_SECONDS);
    if (s_capture_capacity < s_target_bytes.load()) {
        s_capture_capacity = s_target_bytes.load();
    }
    s_capture_buf = static_cast<uint8_t*>(
        heap_caps_malloc(s_capture_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_capture_buf == nullptr) {
        s_capture_buf = static_cast<uint8_t*>(heap_caps_malloc(s_capture_capacity, MALLOC_CAP_8BIT));
    }
    if (s_capture_buf == nullptr) {
        s_failed = true;
        set_detail("capture buffer alloc failed");
        vTaskDelete(nullptr);
        return;
    }

    if (!capture_wav_to_memory()) {
        s_failed = true;
        heap_caps_free(s_capture_buf);
        s_capture_buf = nullptr;
        s_capture_capacity = 0;
        vTaskDelete(nullptr);
        return;
    }

    if (!ensure_wifi_connected()) {
        s_failed = true;
        heap_caps_free(s_capture_buf);
        s_capture_buf = nullptr;
        s_capture_capacity = 0;
        vTaskDelete(nullptr);
        return;
    }

    bool upload_ok = upload_wav_buffer(s_capture_buf, s_captured_bytes.load());
    post_trace_logs();
    if (!upload_ok) {
        s_failed = true;
        ESP_LOGE(TAG, "%s", s_detail);
    }

    if (s_capture_buf != nullptr) {
        heap_caps_free(s_capture_buf);
        s_capture_buf = nullptr;
        s_capture_capacity = 0;
    }
    vTaskDelete(nullptr);
}

} // namespace

bool dji_mic_uac_recorder_start()
{
#if CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD
    if (s_started.exchange(true)) {
        return true;
    }
    s_failed = false;
    s_done = false;
    s_uploading = false;
    s_usb_connected = false;
    s_capture_done = false;
    s_captured_bytes = 0;
    s_bytes_sent = 0;
    s_callback_bytes = 0;
    s_dropped_bytes = 0;
    s_callback_count = 0;
    s_trace_count = 0;
    reset_effective_format();
    memset(s_trace, 0, sizeof(s_trace));
    s_http_status = 0;
    set_url(CONFIG_STACKCHAN_RECORD_UPLOAD_URL);
    set_detail("starting Wi-Fi UAC uploader");

    BaseType_t ok = xTaskCreatePinnedToCore(recorder_task, "dji_uac_wifi", 8192, nullptr, 4, nullptr, 0);
    if (ok != pdPASS) {
        s_started = false;
        s_failed = true;
        set_detail("recorder task create failed");
        return false;
    }
    return true;
#else
    set_detail("UAC record disabled by Kconfig");
    return false;
#endif
}

DjiMicUacRecordStatus dji_mic_uac_recorder_status()
{
    DjiMicUacRecordStatus status;
    status.started = s_started.load();
    status.wifi_connected = s_wifi_connected.load();
    status.usb_connected = s_usb_connected.load();
    status.uploading = s_uploading.load();
    status.done = s_done.load();
    status.failed = s_failed.load();
    status.sample_rate = s_effective_sample_rate.load();
    status.bit_resolution = static_cast<uint16_t>(s_effective_bit_resolution.load());
    status.channels = static_cast<uint8_t>(s_effective_channels.load());
    status.target_bytes = s_target_bytes.load();
    status.captured_bytes = s_captured_bytes.load();
    status.bytes_sent = s_bytes_sent.load();
    status.callback_bytes = s_callback_bytes.load();
    status.dropped_bytes = s_dropped_bytes.load();
    status.callback_count = s_callback_count.load();
    status.trace_count = s_trace_count.load();
    if (status.trace_count > 0) {
        status.first_frame_bit_resolution = s_trace[0].bit_resolution;
        status.first_frame_sample_rate = s_trace[0].sample_rate;
        status.first_frame_bytes = s_trace[0].bytes;
        status.first_frame_mod4 = s_trace[0].mod4;
        status.first_frame_mod6 = s_trace[0].mod6;
    }
    status.http_status = s_http_status.load();

    portENTER_CRITICAL(&s_text_mux);
    strlcpy(status.url, s_url, sizeof(status.url));
    strlcpy(status.detail, s_detail, sizeof(status.detail));
    portEXIT_CRITICAL(&s_text_mux);
    return status;
}
