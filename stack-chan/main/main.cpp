#include <M5Unified.h>
#include <cstdlib>

#include "audio/xiaopai_audio_service.h"
#include "codec_audio_output.h"
#include "expression_controller.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "voice_state.h"

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

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <new>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

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

#include "main_app_state.inc"
#include "main_platform.inc"
#include "main_realtime_transport.inc"
#include "main_wifi_provisioning.inc"
#include "main_firmware_ota.inc"
#include "main_realtime_speech.inc"
#include "main_camera_motion.inc"
#include "main_tts_commands.inc"
#include "main_head_touch.inc"
#include "main_command_services.inc"

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(init_nvs_once());
    force_core_s3_display_board();
    m5_mutex = xSemaphoreCreateMutex();
    audio_mutex = xSemaphoreCreateMutex();
    local_voice_state_init({
        set_light_strip_sleeping,
        set_listening_outputs,
        set_waiting_outputs,
        set_light_strip_speaking,
        mark_wake_find_owner_pending,
    });
    expression_controller_init(m5_mutex, {
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
    audio_service_init();
    audio_service_start();

    M5.Display.setBrightness(180);
    M5.Display.setRotation(1);
    M5.Touch.setHoldThresh(500);
    M5.Touch.setFlickThresh(12);
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
