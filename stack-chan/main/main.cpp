#include <M5Unified.h>
#include <cstdlib>

#include "audio/dji_mic_receiver_input.h"
#include "audio/dji_mic_uac_recorder.h"
#include "audio/xiaopai_audio_service.h"
#include "codec_audio_output.h"
#include "debug_events.h"
#include "expression_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
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
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#ifndef CONFIG_STACKCHAN_DJI_MIC_ENUM_ONLY
#define CONFIG_STACKCHAN_DJI_MIC_ENUM_ONLY 0
#endif

#ifndef CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD
#define CONFIG_STACKCHAN_DJI_MIC_UAC_RECORD 0
#endif

void run_xiaozhi_ota_probe();
bool check_and_apply_firmware_ota_once();
void run_stream_tts_demo();
void run_wifi_connect_app();
void run_camera_upload_app();
void run_tracking_user_demo();
static bool wifi_is_connected();
static bool http_get_string(const std::string& url, std::string* response, int timeout_ms);
static int json_int_value(const cJSON* root, const char* key, int default_value);
static bool json_bool_value(const cJSON* root, const char* key, bool default_value);
static void set_light_strip_listening();
static void set_listening_outputs();
static void set_light_strip_speaking();
static void set_light_strip_sleeping();
static void set_waiting_outputs();
static void set_light_strip_listening_bar(uint8_t level);
static void update_listening_light_level(const int16_t* samples, size_t sample_count);
static void update_speaking_light_level(const int16_t* samples, size_t sample_count);
static void start_speaking_light_animation();
static void stop_speaking_light_animation();
static void apply_speaker_volume();
static bool execute_speak_command(const char* text);
static bool execute_speak_command_internal(const char* text, bool pause_voice_listener, const char* cache_name = nullptr,
                                           const char* voice = nullptr, int sample_rate = 0, int volume = 0,
                                           int speech_rate = 0, int pitch_rate = 0);
static bool enqueue_speak_command(const char* cmd_id, const char* text, const char* cache_name, bool pause_voice_listener,
                                  const char* voice = nullptr, int sample_rate = 0, int volume = 0, int speech_rate = 0,
                                  int pitch_rate = 0);
static void request_speak_preempt(const char* reason);
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
#include "main_wifi_debug.inc"
#include "main_command_services.inc"

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

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs_once());
    install_wifi_debug_log_sink();
    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGW(TAG, "Boot reset reason: %s (%d)", reset_reason_name(reset_reason), static_cast<int>(reset_reason));
    force_core_s3_display_board();
    m5_mutex = xSemaphoreCreateMutex();
    audio_mutex = xSemaphoreCreateMutex();
    xiaopai_state_init({
        set_light_strip_sleeping,
        set_listening_outputs,
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
    M5.begin(cfg);

    M5.Display.setBrightness(180);
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

    run_light_strip_boot_probe();

    start_background_services();

    while (true) {
        {
            M5Lock lock;
            if (!camera_owns_internal_i2c) {
                M5.update();
            }
        }
        if (auto_sleep_dark_due()) {
            show_sleep_dark_listening("idle timeout");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
