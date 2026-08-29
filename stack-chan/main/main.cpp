#include <M5Unified.h>
#include <cstdlib>

#include "audio/dji_mic_receiver_input.h"
#include "audio/dji_mic_uac_recorder.h"
#include "audio/xiaopai_audio_service.h"
#include "codec_audio_output.h"
#include "power_manager.h"

#include "expression_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "xiaopai_psram_task.h"
#include "xiaopai_state.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_private/cache_utils.h"
#include "esp_system.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ws.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_audio_enc.h"
#include "esp_audio_dec.h"
#include "esp_camera.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_audio_types.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#ifndef CONFIG_STACKCHAN_DJI_MIC_USB_INPUT
#define CONFIG_STACKCHAN_DJI_MIC_USB_INPUT 0
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_ENUM_ONLY
#define CONFIG_STACKCHAN_DJI_MIC_ENUM_ONLY 0
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD
#define CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD 0
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_AUTO_START
#define CONFIG_STACKCHAN_DJI_MIC_AUTO_START 0
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_START_DELAY_MS
#define CONFIG_STACKCHAN_DJI_MIC_START_DELAY_MS 5000
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_MAX_START_RETRIES
#define CONFIG_STACKCHAN_DJI_MIC_MAX_START_RETRIES 3
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_RETRY_BACKOFF_MS
#define CONFIG_STACKCHAN_DJI_MIC_RETRY_BACKOFF_MS 2000
#endif
#ifndef CONFIG_STACKCHAN_DJI_VOICE_START_THRESHOLD
#define CONFIG_STACKCHAN_DJI_VOICE_START_THRESHOLD 900
#endif
#ifndef CONFIG_STACKCHAN_DJI_VOICE_STOP_THRESHOLD
#define CONFIG_STACKCHAN_DJI_VOICE_STOP_THRESHOLD 400
#endif
#ifndef CONFIG_STACKCHAN_NETWORK_DEBUG
#define CONFIG_STACKCHAN_NETWORK_DEBUG 1
#endif
#ifndef CONFIG_STACKCHAN_NETWORK_DEBUG_QUEUE_DEPTH
#define CONFIG_STACKCHAN_NETWORK_DEBUG_QUEUE_DEPTH 64
#endif
#ifndef CONFIG_STACKCHAN_NETWORK_DEBUG_BATCH_LINES
#define CONFIG_STACKCHAN_NETWORK_DEBUG_BATCH_LINES 8
#endif
#ifndef CONFIG_STACKCHAN_SERIAL_DEBUG_COMMAND
#define CONFIG_STACKCHAN_SERIAL_DEBUG_COMMAND 0
#endif

#if CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD && CONFIG_STACKCHAN_DJI_MIC_AUTO_START
#error "CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD is a standalone test firmware mode; disable CONFIG_STACKCHAN_DJI_MIC_AUTO_START"
#endif

void run_realtime_speech();
bool create_realtime_task_early();
bool create_command_task_early();
bool check_and_apply_firmware_ota_once();
void run_stream_tts_demo();
void run_wifi_connect_app();
void run_camera_upload_app();
void run_tracking_user_demo();
static bool wifi_is_connected();
static bool http_get_string(const std::string& url, std::string* response, int timeout_ms, bool log_result = true);
static int json_int_value(const cJSON* root, const char* key, int default_value);
static bool json_bool_value(const cJSON* root, const char* key, bool default_value);
static void set_light_strip_listening();
static void set_listening_outputs();
static void set_dialog_sleeping_outputs();
static void set_light_strip_speaking();
static void set_light_strip_sleeping();
static void set_waiting_outputs();
static void set_light_strip_demo_lock();
static void set_light_strip_listening_bar(uint8_t level);
static void update_listening_light_level(const int16_t* samples, size_t sample_count);
static void update_speaking_light_level(const int16_t* samples, size_t sample_count);
static void start_speaking_light_animation();
static void stop_speaking_light_animation();
static void apply_speaker_volume();
static bool execute_speak_command_internal(const char* text, bool pause_voice_listener, const char* cache_name = nullptr,
                                           const char* voice = nullptr, int sample_rate = 0, int volume = 0,
                                           int speech_rate = 0, int pitch_rate = 0, const char* cmd_id = "",
                                           bool manage_voice_state = true);
static bool enqueue_speak_command(const char* cmd_id, const char* text, const char* cache_name, bool pause_voice_listener,
                                  const char* voice = nullptr, int sample_rate = 0, int volume = 0, int speech_rate = 0,
                                  int pitch_rate = 0, uint32_t attempt = 0, uint32_t generation = 0,
                                  uint16_t segment_index = 0, uint32_t ttl_ms = 30000,
                                  const char* turn_id = nullptr, const char* expression = "calm",
                                  bool reply_end = true, bool reply_cancelled = false,
                                  int speaker_volume = -1);
static void request_speak_preempt(const char* reason);
static void advance_speech_generation(uint32_t generation = 0);
static bool reply_voice_audio_started(const char* turn_id, uint32_t generation);
static bool reply_voice_is_active();
static bool run_find_owner_command(int rounds, const char* reply, float gain_x, float gain_y, float stop_pixels,
                                   bool preserve_speech_playback, bool wait_for_speech);

static const char* reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "external";
        case ESP_RST_SW:
            return "software";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "interrupt_wdt";
        case ESP_RST_TASK_WDT:
            return "task_wdt";
        case ESP_RST_WDT:
            return "other_wdt";
        case ESP_RST_DEEPSLEEP:
            return "deepsleep";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_SDIO:
            return "sdio";
        case ESP_RST_USB:
            return "usb";
        case ESP_RST_JTAG:
            return "jtag";
        case ESP_RST_EFUSE:
            return "efuse";
        case ESP_RST_PWR_GLITCH:
            return "power_glitch";
        case ESP_RST_CPU_LOCKUP:
            return "cpu_lockup";
        default:
            return "unknown";
    }
}

#include "main_app_state.inc"
#include "main_platform.inc"
#include "main_realtime_transport.inc"
#include "main_wifi_provisioning.inc"
#include "main_firmware_ota.inc"
#include "main_realtime_speech.inc"
#include "main_camera_motion.inc"
#include "main_tts_commands.inc"
#include "main_head_touch.inc"

namespace {
constexpr int kDefaultDisplayBrightnessPercent = 70;
constexpr char kDisplayBrightnessNvsNamespace[] = "xiaopai";
constexpr char kDisplayBrightnessNvsKey[] = "brightness";
std::atomic<int> display_brightness_percent{kDefaultDisplayBrightnessPercent};

static int clamp_display_brightness_percent(int percent)
{
    return std::max(1, std::min(100, percent));
}

static uint8_t display_brightness_raw(int percent)
{
    return static_cast<uint8_t>((clamp_display_brightness_percent(percent) * 255 + 50) / 100);
}

static void load_display_brightness_preference()
{
    uint8_t stored = kDefaultDisplayBrightnessPercent;
    nvs_handle_t handle = 0;
    if (nvs_open(kDisplayBrightnessNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, kDisplayBrightnessNvsKey, &stored) != ESP_OK) {
            stored = kDefaultDisplayBrightnessPercent;
        }
        nvs_close(handle);
    }
    display_brightness_percent.store(
        clamp_display_brightness_percent(static_cast<int>(stored)), std::memory_order_release);
}

static bool set_display_brightness(int percent, bool persist)
{
    percent = clamp_display_brightness_percent(percent);
    if (persist) {
        nvs_handle_t handle = 0;
        esp_err_t err = nvs_open(kDisplayBrightnessNvsNamespace, NVS_READWRITE, &handle);
        if (err != ESP_OK) {
            return false;
        }
        err = nvs_set_u8(handle, kDisplayBrightnessNvsKey, static_cast<uint8_t>(percent));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
        if (err != ESP_OK) {
            return false;
        }
    }
    display_brightness_percent.store(percent, std::memory_order_release);
    M5Lock lock;
    M5.Display.setBrightness(display_brightness_raw(percent));
    ESP_LOGI(TAG, "Display brightness set to %d%%", percent);
    return true;
}
}  // namespace

#include "main_command_services.inc"

static void set_demo_lock_enabled(bool enabled)
{
    const bool previous = demo_lock_enabled.exchange(enabled);
    if (previous == enabled) {
        return;
    }

    if (enabled) {
        ESP_LOGW(TAG, "Demo lock enabled by power button");
        demo_lock_abort_pending = true;
        pause_voice_listener_for_shared_peripherals("demo lock");
        request_speak_preempt("demo lock");
        advance_speech_generation();
        xiaopai_cancel_current("demo lock");
        xiaopai_state_set(LocalVoiceState::Idle, "demo lock");
        set_light_strip_demo_lock();
        return;
    }

    ESP_LOGW(TAG, "Demo lock disabled by power button");
    resume_voice_listener_after_shared_peripherals();
    mark_user_interaction("demo unlock");
    xiaopai_state_set(LocalVoiceState::Listening, "demo unlock");
    expression_state_set(kDefaultExpression);
    xiaopai_state_apply_outputs(LocalVoiceState::Listening);
}

#if CONFIG_STACKCHAN_DJI_MIC_ENUM_ONLY || CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD
static const char* yes_no(bool value)
{
    return value ? "yes" : "no";
}
#endif

#if CONFIG_STACKCHAN_DJI_MIC_ENUM_ONLY
static void draw_dji_mic_enum_screen()
{
    DjiMicReceiverStatus status = dji_mic_receiver_input_status();
    M5Lock lock;
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_left);
    display.setFont(&fonts::Font4);
    display.setTextSize(1);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("DJI USB Enum", 12, 10);
    display.drawFastHLine(0, 42, display.width(), TFT_DARKGREY);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    char line[96];
    snprintf(line, sizeof(line), "VID/PID: %04x:%04x target=%s",
             static_cast<unsigned>(status.vendor_id),
             static_cast<unsigned>(status.product_id),
             yes_no(status.target_vid_pid));
    display.drawString(line, 12, 58);

    snprintf(line, sizeof(line), "Speed: %s full=%s",
             status.speed != nullptr && status.speed[0] != '\0' ? status.speed : "-",
             yes_no(status.full_speed));
    display.drawString(line, 12, 78);

    snprintf(line, sizeof(line), "AudioControl: %s", yes_no(status.audio_control));
    display.drawString(line, 12, 98);

    snprintf(line, sizeof(line), "AudioStreaming: %s", yes_no(status.audio_streaming));
    display.drawString(line, 12, 118);

    snprintf(line, sizeof(line), "Manufacturer: %.32s",
             status.manufacturer != nullptr && status.manufacturer[0] != '\0' ? status.manufacturer : "-");
    display.drawString(line, 12, 146);

    snprintf(line, sizeof(line), "Product: %.40s",
             status.product != nullptr && status.product[0] != '\0' ? status.product : "-");
    display.drawString(line, 12, 166);

    display.setTextColor(status.target_vid_pid && status.full_speed && status.audio_control && status.audio_streaming
                             ? TFT_GREEN
                             : TFT_ORANGE,
                         TFT_BLACK);
    display.drawString(status.detail != nullptr && status.detail[0] != '\0' ? status.detail : "-", 12, 196);
}
#endif

#if CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD
static void draw_dji_mic_uac_record_screen()
{
    DjiMicUacRecordStatus status = dji_mic_uac_recorder_status();
    M5Lock lock;
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_left);
    display.setFont(&fonts::Font4);
    display.setTextSize(1);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("DJI UAC WAV", 12, 10);
    display.drawFastHLine(0, 42, display.width(), TFT_DARKGREY);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    char line[112];
    snprintf(line, sizeof(line), "Format: %lu Hz %u-bit %u ch",
             static_cast<unsigned long>(status.sample_rate),
             static_cast<unsigned>(status.bit_resolution),
             static_cast<unsigned>(status.channels));
    display.drawString(line, 12, 56);

    snprintf(line, sizeof(line), "WiFi: %s USB: %s Up: %s",
             yes_no(status.wifi_connected), yes_no(status.usb_connected), yes_no(status.uploading));
    display.drawString(line, 12, 76);

    uint32_t capture_pct = status.target_bytes > 0 ? (status.captured_bytes * 100U) / status.target_bytes : 0;
    snprintf(line, sizeof(line), "Capt: %lu/%lu %lu%%",
             static_cast<unsigned long>(status.captured_bytes),
             static_cast<unsigned long>(status.target_bytes),
             static_cast<unsigned long>(capture_pct));
    display.drawString(line, 12, 96);

    uint32_t sent_pct = status.captured_bytes > 0 ? (status.bytes_sent * 100U) / status.captured_bytes : 0;
    snprintf(line, sizeof(line), "Sent: %lu/%lu %lu%%",
             static_cast<unsigned long>(status.bytes_sent),
             static_cast<unsigned long>(status.captured_bytes),
             static_cast<unsigned long>(sent_pct));
    display.drawString(line, 12, 116);

    snprintf(line, sizeof(line), "CB: %lu frames %lu bytes",
             static_cast<unsigned long>(status.callback_count),
             static_cast<unsigned long>(status.callback_bytes));
    display.drawString(line, 12, 136);

    snprintf(line, sizeof(line), "F0: %u-bit %luHz %luB m4:%u m6:%u",
             static_cast<unsigned>(status.first_frame_bit_resolution),
             static_cast<unsigned long>(status.first_frame_sample_rate),
             static_cast<unsigned long>(status.first_frame_bytes),
             static_cast<unsigned>(status.first_frame_mod4),
             static_cast<unsigned>(status.first_frame_mod6));
    display.setTextColor(status.trace_count > 0 ? TFT_WHITE : TFT_ORANGE, TFT_BLACK);
    display.drawString(line, 12, 156);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(line, sizeof(line), "HTTP: %d %.46s", status.http_status,
             status.url[0] != '\0' ? status.url : "-");
    display.drawString(line, 12, 180);

    display.setTextColor(status.done ? TFT_GREEN : (status.failed ? TFT_RED : TFT_ORANGE), TFT_BLACK);
    display.drawString(status.detail[0] != '\0' ? status.detail : "-", 12, 208);
}
#endif

#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT && CONFIG_STACKCHAN_DJI_MIC_AUTO_START
static void start_dji_mic_after_boot_task(void*)
{
    const int delay_ms = CONFIG_STACKCHAN_DJI_MIC_START_DELAY_MS;
    ESP_LOGI(TAG, "DJI Mic delayed start scheduled: %d ms", delay_ms);

    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    bool logged_ota_wait = false;
    while (s_firmware_ota_in_progress.load()) {
        if (!logged_ota_wait) {
            ESP_LOGI(TAG, "DJI Mic delayed start waiting for firmware OTA to finish");
            logged_ota_wait = true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (usb_serial_jtag_is_connected()) {
        ESP_LOGW(TAG,
                 "DJI Mic start skipped: PC USB Serial/JTAG host detected; "
                 "keeping USB peripheral mode and VBUS output disabled");
        stackchan_power_manager_set_usb_output(false, "PC USB host detected");
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    bool ok = false;
    for (int attempt = 1; attempt <= CONFIG_STACKCHAN_DJI_MIC_MAX_START_RETRIES; ++attempt) {
        if (!stackchan_power_manager_dji_allowed()) {
            ESP_LOGW(TAG, "DJI Mic start cancelled by battery power policy");
            break;
        }
        ESP_LOGI(TAG, "Starting DJI Mic USB input: attempt=%d/%d",
                 attempt, CONFIG_STACKCHAN_DJI_MIC_MAX_START_RETRIES);
        ok = dji_mic_receiver_input_start();
        if (ok) {
            ESP_LOGI(TAG, "Delayed DJI Mic start OK");
            break;
        }
        ESP_LOGE(TAG, "Delayed DJI Mic start failed: attempt=%d detail=%s",
                 attempt, dji_mic_receiver_input_status().detail);
        if (attempt < CONFIG_STACKCHAN_DJI_MIC_MAX_START_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_STACKCHAN_DJI_MIC_RETRY_BACKOFF_MS * attempt));
        }
    }
    if (!ok) {
        ESP_LOGW(TAG, "DJI Mic unavailable after limited retries; internal mic remains active");
    }

    vTaskDeleteWithCaps(nullptr);
}

static void schedule_dji_mic_after_boot()
{
    BaseType_t created = xiaopai_task_create_psram(
        start_dji_mic_after_boot_task,
        "dji_mic_delay",
        4096,
        nullptr,
        3,
        nullptr,
        0);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create delayed DJI Mic start task");
    }
}
#endif

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs_once());
    load_network_debug_preference();
    load_display_brightness_preference();
    create_realtime_task_early();

    // Keep USB Serial/JTAG console until DJI Host actually starts. Switching the
    // console to UART0 was not required for DJI and made boot harder to diagnose.
    esp_reset_reason_t reset_reason = esp_reset_reason();
    const esp_app_desc_t* app = esp_app_get_description();
    ESP_LOGW(TAG, "BOOT reset_reason=%s(%d) project=%s version=%s idf=%s",
             reset_reason_name(reset_reason), static_cast<int>(reset_reason),
             app != nullptr ? app->project_name : "unknown", app != nullptr ? app->version : "unknown",
             app != nullptr ? app->idf_ver : "unknown");
    ESP_LOGW(TAG, "BOOT console=usb_serial_jtag log_level=%s device=%s",
             network_debug_enabled.load(std::memory_order_acquire) ? "info" : "warn",
             mac_address().c_str());
    force_core_s3_display_board();
    m5_mutex = xSemaphoreCreateMutex();
    audio_mutex = xSemaphoreCreateMutex();
    provisioning_mutex = xSemaphoreCreateMutex();
    if (m5_mutex == nullptr || audio_mutex == nullptr || provisioning_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create required service mutexes");
        return;
    }
    xiaopai_state_init({
        set_light_strip_sleeping,
        set_listening_outputs,
        set_dialog_sleeping_outputs,
        set_waiting_outputs,
        set_light_strip_speaking,
    });
    expression_state_init(m5_mutex, {
        start_speaking_light_animation,
        stop_speaking_light_animation,
        set_light_strip_listening,
        set_light_strip_sleeping,
        should_restore_listening_light_after_speech,
    });
    auto cfg = M5.config();
    cfg.internal_mic = false;
    cfg.internal_spk = false;
    // Keep CoreS3 BUS 5V/BOOST like v0.4.6. output_power=false also drops SY7088
    // and Stack-chan servo power, which brownouts a few seconds into boot.
    M5.begin(cfg);
    stackchan_power_manager_init(m5_mutex, reset_reason);
    if (!stackchan_power_manager_start()) {
        ESP_LOGE(TAG, "Failed to start battery power manager");
    }
    stackchan_power_manager_set_usb_output(false, "boot keep USB-C as input");

    set_display_brightness(display_brightness_percent.load(std::memory_order_acquire), false);
    M5.Display.setRotation(1);
    M5.Touch.setHoldThresh(500);
    M5.Touch.setFlickThresh(12);

#if CONFIG_STACKCHAN_DJI_MIC_ENUM_ONLY
    ESP_LOGI(TAG, "DJI Mic enum-only mode enabled; skipping audio capture and app services");
    dji_mic_receiver_input_start();
    draw_dji_mic_enum_screen();
    TickType_t last_draw_ticks = xTaskGetTickCount();
    while (true) {
        {
            M5Lock lock;
            M5.update();
        }
        TickType_t now = xTaskGetTickCount();
        if (now - last_draw_ticks >= pdMS_TO_TICKS(500)) {
            last_draw_ticks = now;
            draw_dji_mic_enum_screen();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#endif

#if CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD
    ESP_LOGI(TAG, "DJI Mic UAC record mode enabled; skipping ASR/dialog services");
    dji_mic_uac_recorder_start();
    draw_dji_mic_uac_record_screen();
    TickType_t last_record_draw_ticks = xTaskGetTickCount();
    while (true) {
        {
            M5Lock lock;
            M5.update();
        }
        TickType_t now = xTaskGetTickCount();
        if (now - last_record_draw_ticks >= pdMS_TO_TICKS(500)) {
            last_record_draw_ticks = now;
            draw_dji_mic_uac_record_screen();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
#endif

    audio_service_init();
    audio_service_start();
    start_speak_command_service();
    create_command_task_early();

    run_light_strip_boot_probe();
    ESP_LOGI(TAG, "Moving head to boot pose yaw=0.0 pitch=%.1f", kTrackingHomePitchDeg);
    if (!move_head_to_tracking_angles(0.0f, kTrackingHomePitchDeg, 800)) {
        ESP_LOGW(TAG, "Failed to move head to boot pose");
    }
    vTaskDelay(pdMS_TO_TICKS(900));

    start_background_services();
    start_network_debug_service();
#if CONFIG_STACKCHAN_SERIAL_DEBUG_COMMAND
    start_serial_debug_command_service();
#endif

#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT && CONFIG_STACKCHAN_DJI_MIC_AUTO_START
    schedule_dji_mic_after_boot();
#endif

    while (true) {
        bool power_button_clicked = false;
        {
            M5Lock lock;
            if (!camera_owns_internal_i2c) {
                M5.update();
                power_button_clicked = M5.BtnPWR.wasClicked();
            }
        }
        if (power_button_clicked) {
            set_demo_lock_enabled(!demo_lock_enabled.load());
        }
        if (auto_sleep_dark_due()) {
            show_sleep_dark_listening("idle timeout");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
