#include "dji_mic_usb_power.h"

#include "power_manager.h"
#include "sdkconfig.h"

#include <M5Unified.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_STACKCHAN_DJI_MIC_DRIVE_VBUS
#define CONFIG_STACKCHAN_DJI_MIC_DRIVE_VBUS 0
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_VBUS_OFF_MS
#define CONFIG_STACKCHAN_DJI_MIC_VBUS_OFF_MS 800
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_HOST_LISTEN_MS
#define CONFIG_STACKCHAN_DJI_MIC_HOST_LISTEN_MS 400
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_VBUS_SETTLE_MS
#define CONFIG_STACKCHAN_DJI_MIC_VBUS_SETTLE_MS 2000
#endif

namespace {
constexpr const char* TAG = "DjiUsbPower";
constexpr int kServoQuietMsAfterVbus = 1500;
bool s_lease_held = false;
bool s_display_dimmed = false;
uint8_t s_saved_brightness = 180;

void dim_display_for_vbus()
{
    if (s_display_dimmed) {
        return;
    }
    s_saved_brightness = M5.Display.getBrightness();
    M5.Display.setBrightness(16);
    s_display_dimmed = true;
    ESP_LOGI(TAG, "Dimmed display for VBUS inrush: brightness %u -> 16",
             static_cast<unsigned>(s_saved_brightness));
}

void restore_display_after_vbus()
{
    if (!s_display_dimmed) {
        return;
    }
    M5.Display.setBrightness(s_saved_brightness);
    s_display_dimmed = false;
    ESP_LOGI(TAG, "Restored display brightness=%u", static_cast<unsigned>(s_saved_brightness));
}
}  // namespace

esp_err_t dji_mic_usb_power_prepare_host()
{
    if (!stackchan_power_manager_begin_usb_enumeration()) {
        return ESP_ERR_INVALID_STATE;
    }
    s_lease_held = true;
    dim_display_for_vbus();

#if CONFIG_STACKCHAN_DJI_MIC_DRIVE_VBUS
    if (!stackchan_power_manager_set_usb_output(false, "DJI host prepare")) {
        dji_mic_usb_power_abort("failed to disable VBUS");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(CONFIG_STACKCHAN_DJI_MIC_VBUS_OFF_MS));
#else
    ESP_LOGW(TAG, "CoreS3 USB VBUS drive disabled; external host-safe power is required");
#endif
    return ESP_OK;
}

esp_err_t dji_mic_usb_power_enable_after_host()
{
    if (!s_lease_held) {
        return ESP_ERR_INVALID_STATE;
    }
    vTaskDelay(pdMS_TO_TICKS(CONFIG_STACKCHAN_DJI_MIC_HOST_LISTEN_MS));

#if CONFIG_STACKCHAN_DJI_MIC_DRIVE_VBUS
    if (!stackchan_power_manager_set_usb_output(true, "DJI host ready")) {
        dji_mic_usb_power_abort("failed to enable VBUS");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(CONFIG_STACKCHAN_DJI_MIC_VBUS_SETTLE_MS));
#endif

    vTaskDelay(pdMS_TO_TICKS(kServoQuietMsAfterVbus));
    stackchan_power_manager_end_usb_enumeration();
    s_lease_held = false;
    restore_display_after_vbus();
    ESP_LOGI(TAG, "DJI USB host power sequence completed");
    return ESP_OK;
}

void dji_mic_usb_power_abort(const char* reason)
{
#if CONFIG_STACKCHAN_DJI_MIC_DRIVE_VBUS
    stackchan_power_manager_set_usb_output(false, reason != nullptr ? reason : "DJI USB abort");
#else
    (void)reason;
#endif
    restore_display_after_vbus();
    if (s_lease_held) {
        stackchan_power_manager_end_usb_enumeration();
        s_lease_held = false;
    }
}
