#include "power_manager.h"

#include <M5Unified.h>

#include "audio/dji_mic_receiver_input.h"
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "xiaopai_psram_task.h"

#include <algorithm>
#include <atomic>

#ifndef CONFIG_STACKCHAN_BATTERY_DJI_START_PERCENT
#define CONFIG_STACKCHAN_BATTERY_DJI_START_PERCENT 30
#endif
#ifndef CONFIG_STACKCHAN_BATTERY_LOW_PERCENT
#define CONFIG_STACKCHAN_BATTERY_LOW_PERCENT 15
#endif
#ifndef CONFIG_STACKCHAN_BATTERY_LOW_MV
#define CONFIG_STACKCHAN_BATTERY_LOW_MV 3550
#endif
#ifndef CONFIG_STACKCHAN_BATTERY_CRITICAL_MV
#define CONFIG_STACKCHAN_BATTERY_CRITICAL_MV 3400
#endif
#ifndef CONFIG_STACKCHAN_BATTERY_SAMPLE_MS
#define CONFIG_STACKCHAN_BATTERY_SAMPLE_MS 1000
#endif
#ifndef CONFIG_STACKCHAN_BATTERY_DEBOUNCE_SAMPLES
#define CONFIG_STACKCHAN_BATTERY_DEBOUNCE_SAMPLES 5
#endif
#ifndef CONFIG_STACKCHAN_BATTERY_RECOVERY_HYSTERESIS_MV
#define CONFIG_STACKCHAN_BATTERY_RECOVERY_HYSTERESIS_MV 150
#endif
#ifndef CONFIG_STACKCHAN_BATTERY_EMERGENCY_POWEROFF
#define CONFIG_STACKCHAN_BATTERY_EMERGENCY_POWEROFF 0
#endif
#ifndef CONFIG_STACKCHAN_DJI_MIC_LOW_BATTERY_PROTECTION
#define CONFIG_STACKCHAN_DJI_MIC_LOW_BATTERY_PROTECTION 0
#endif

namespace {

constexpr const char* TAG = "PowerManager";
constexpr const char* kNvsNamespace = "xiaopai";
constexpr const char* kBrownoutCountKey = "pwr_faults";
constexpr int kLowVolumeLimitPercent = 35;
constexpr int kSafeVolumeLimitPercent = 20;
constexpr uint8_t kLowRgbLimit = 24;
constexpr uint8_t kSafeRgbLimit = 8;
constexpr uint32_t kSafeModeBrownoutCount = 3;

SemaphoreHandle_t s_m5_mutex = nullptr;
TaskHandle_t s_task = nullptr;
std::atomic<int> s_state{static_cast<int>(StackchanPowerState::kNormal)};
std::atomic<int> s_battery_mv{-1};
std::atomic<int> s_battery_percent{-1};
std::atomic<int> s_battery_current_ma{0};
std::atomic<int> s_vbus_mv{-1};
std::atomic<bool> s_charging{false};
std::atomic<bool> s_usb_output{false};
std::atomic<bool> s_enumeration_lease{false};
std::atomic<uint32_t> s_brownout_boot_count{0};
std::atomic<bool> s_initialized{false};
std::atomic<bool> s_poweroff_requested{false};

class M5Guard {
public:
    explicit M5Guard(TickType_t timeout = pdMS_TO_TICKS(500))
    {
        locked_ = s_m5_mutex == nullptr || xSemaphoreTake(s_m5_mutex, timeout) == pdTRUE;
    }

    ~M5Guard()
    {
        if (locked_ && s_m5_mutex != nullptr) {
            xSemaphoreGive(s_m5_mutex);
        }
    }

    bool locked() const { return locked_; }

private:
    bool locked_ = false;
};

const char* state_name(StackchanPowerState state)
{
    switch (state) {
        case StackchanPowerState::kLow:
            return "low";
        case StackchanPowerState::kCritical:
            return "critical";
        case StackchanPowerState::kSafeMode:
            return "safe_mode";
        case StackchanPowerState::kNormal:
        default:
            return "normal";
    }
}

bool valid_battery_percent(int percent)
{
    return percent >= 0 && percent <= 100;
}

bool valid_battery_mv(int mv)
{
    return mv >= 2500 && mv <= 5000;
}

void persist_boot_fault(esp_reset_reason_t reset_reason)
{
    uint32_t count = 0;
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }

    nvs_get_u32(handle, kBrownoutCountKey, &count);
    if (reset_reason == ESP_RST_BROWNOUT || reset_reason == ESP_RST_PWR_GLITCH) {
        count = std::min<uint32_t>(count + 1, 255);
    } else {
        count = 0;
    }
    nvs_set_u32(handle, kBrownoutCountKey, count);
    nvs_commit(handle);
    nvs_close(handle);
    s_brownout_boot_count = count;

    if (count >= kSafeModeBrownoutCount) {
        s_state = static_cast<int>(StackchanPowerState::kSafeMode);
        ESP_LOGW(TAG, "Power fault safe mode enabled: reset=%d count=%u",
                 static_cast<int>(reset_reason), static_cast<unsigned>(count));
    } else if (count > 0) {
        ESP_LOGW(TAG, "Brownout recorded without safe mode: reset=%d count=%u",
                 static_cast<int>(reset_reason), static_cast<unsigned>(count));
    }
}

void sample_power()
{
    int battery_mv = -1;
    int battery_percent = -1;
    int battery_current_ma = 0;
    int vbus_mv = -1;
    bool charging = false;
    bool usb_output = false;
    {
        M5Guard lock;
        if (!lock.locked()) {
            ESP_LOGW(TAG, "Power sample skipped: M5 mutex timeout");
            return;
        }
        battery_mv = M5.Power.getBatteryVoltage();
        battery_percent = M5.Power.getBatteryLevel();
        battery_current_ma = M5.Power.getBatteryCurrent();
        vbus_mv = M5.Power.getVBUSVoltage();
        charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
        usb_output = M5.Power.getUsbOutput();
    }
    s_battery_mv = battery_mv;
    s_battery_percent = battery_percent;
    s_battery_current_ma = battery_current_ma;
    s_vbus_mv = vbus_mv;
    s_charging = charging;
    s_usb_output = usb_output;
}

void power_task(void*)
{
    uint32_t low_samples = 0;
    uint32_t critical_samples = 0;
    uint32_t recovery_samples = 0;
    int previous_state = s_state.load();

    while (true) {
        sample_power();

        const int battery_mv = s_battery_mv.load();
        const int battery_percent = s_battery_percent.load();
        const bool low = valid_battery_mv(battery_mv)
                             ? battery_mv <= CONFIG_STACKCHAN_BATTERY_LOW_MV
                             : (valid_battery_percent(battery_percent) &&
                                battery_percent <= CONFIG_STACKCHAN_BATTERY_LOW_PERCENT);
        const bool critical = valid_battery_mv(battery_mv) &&
                              battery_mv <= CONFIG_STACKCHAN_BATTERY_CRITICAL_MV;

        low_samples = low ? low_samples + 1 : 0;
        critical_samples = critical ? critical_samples + 1 : 0;
        const bool recovered = valid_battery_mv(battery_mv)
                                   ? battery_mv >= CONFIG_STACKCHAN_BATTERY_LOW_MV +
                                                     CONFIG_STACKCHAN_BATTERY_RECOVERY_HYSTERESIS_MV
                                   : (!valid_battery_percent(battery_percent) ||
                                      battery_percent >= CONFIG_STACKCHAN_BATTERY_LOW_PERCENT + 5);
        recovery_samples = recovered ? recovery_samples + 1 : 0;

        StackchanPowerState next = static_cast<StackchanPowerState>(s_state.load());
        if (next != StackchanPowerState::kSafeMode) {
            if (critical_samples >= CONFIG_STACKCHAN_BATTERY_DEBOUNCE_SAMPLES) {
                next = StackchanPowerState::kCritical;
            } else if (low_samples >= CONFIG_STACKCHAN_BATTERY_DEBOUNCE_SAMPLES) {
                next = StackchanPowerState::kLow;
            } else if (next == StackchanPowerState::kLow &&
                       recovery_samples >= CONFIG_STACKCHAN_BATTERY_DEBOUNCE_SAMPLES) {
                next = StackchanPowerState::kNormal;
            } else if (next == StackchanPowerState::kNormal) {
                next = StackchanPowerState::kNormal;
            }
        }
        s_state = static_cast<int>(next);

        if (previous_state != static_cast<int>(next)) {
            ESP_LOGW(TAG, "Power state changed: %s -> %s battery=%dmV level=%d%% vbus=%dmV",
                     state_name(static_cast<StackchanPowerState>(previous_state)), state_name(next),
                     battery_mv, battery_percent, s_vbus_mv.load());
            previous_state = static_cast<int>(next);
            const bool must_stop_dji = next == StackchanPowerState::kSafeMode
#if CONFIG_STACKCHAN_DJI_MIC_LOW_BATTERY_PROTECTION
                                       || next == StackchanPowerState::kLow ||
                                              next == StackchanPowerState::kCritical
#endif
                ;
            if (must_stop_dji) {
                dji_mic_receiver_input_stop("battery power downgrade");
            }
        }

        bool must_disable_usb = next == StackchanPowerState::kSafeMode;
#if CONFIG_STACKCHAN_DJI_MIC_LOW_BATTERY_PROTECTION
        must_disable_usb = must_disable_usb || next == StackchanPowerState::kLow ||
                           next == StackchanPowerState::kCritical;
#endif
        if (must_disable_usb && s_usb_output.load()) {
            stackchan_power_manager_set_usb_output(false, "power downgrade");
        }

#if CONFIG_STACKCHAN_BATTERY_EMERGENCY_POWEROFF
        if (next == StackchanPowerState::kCritical && !s_poweroff_requested.exchange(true)) {
            ESP_LOGE(TAG, "Critical battery persisted; shedding load without powerOff");
            dji_mic_receiver_input_stop("critical battery");
            stackchan_power_manager_set_usb_output(false, "critical battery");
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(CONFIG_STACKCHAN_BATTERY_SAMPLE_MS));
    }
}

}  // namespace

void stackchan_power_manager_init(SemaphoreHandle_t m5_mutex, esp_reset_reason_t reset_reason)
{
    if (s_initialized.exchange(true)) {
        return;
    }
    s_m5_mutex = m5_mutex;
    persist_boot_fault(reset_reason);
    sample_power();
    ESP_LOGI(TAG, "Initialized: state=%s battery=%dmV level=%d%% current=%dmA vbus=%dmV usb_output=%d",
             state_name(static_cast<StackchanPowerState>(s_state.load())), s_battery_mv.load(),
             s_battery_percent.load(), s_battery_current_ma.load(), s_vbus_mv.load(),
             static_cast<int>(s_usb_output.load()));
}

bool stackchan_power_manager_start()
{
    if (!s_initialized.load()) {
        return false;
    }
    if (s_task != nullptr) {
        return true;
    }
    return xiaopai_task_create_psram_unpinned(power_task, "power_manager", 4096, nullptr, 4, &s_task) == pdPASS;
}

StackchanPowerStatus stackchan_power_manager_status()
{
    StackchanPowerStatus status;
    status.state = static_cast<StackchanPowerState>(s_state.load());
    status.battery_mv = s_battery_mv.load();
    status.battery_percent = s_battery_percent.load();
    status.battery_current_ma = s_battery_current_ma.load();
    status.vbus_mv = s_vbus_mv.load();
    status.charging = s_charging.load();
    status.usb_output = s_usb_output.load();
    status.enumeration_lease = s_enumeration_lease.load();
    status.dji_allowed = stackchan_power_manager_dji_allowed();
    status.brownout_boot_count = s_brownout_boot_count.load();
    return status;
}

bool stackchan_power_manager_begin_usb_enumeration()
{
    if (!stackchan_power_manager_dji_allowed()) {
        ESP_LOGW(TAG, "USB enumeration denied by power policy");
        return false;
    }
    bool expected = false;
    if (!s_enumeration_lease.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "USB enumeration lease already held");
        return false;
    }
    ESP_LOGI(TAG, "USB enumeration lease acquired");
    return true;
}

void stackchan_power_manager_end_usb_enumeration()
{
    if (s_enumeration_lease.exchange(false)) {
        ESP_LOGI(TAG, "USB enumeration lease released");
    }
}

bool stackchan_power_manager_set_usb_output(bool enabled, const char* reason)
{
    if (enabled && !stackchan_power_manager_dji_allowed()) {
        ESP_LOGW(TAG, "USB VBUS enable denied: reason=%s", reason != nullptr ? reason : "-");
        return false;
    }
    M5Guard lock;
    if (!lock.locked()) {
        ESP_LOGE(TAG, "USB VBUS update failed: M5 mutex timeout");
        return false;
    }
    M5.Power.setUsbOutput(enabled);
    bool actual = M5.Power.getUsbOutput();
    s_usb_output = actual;
    ESP_LOGI(TAG, "USB VBUS requested=%d actual=%d battery=%dmV reason=%s",
             static_cast<int>(enabled), static_cast<int>(actual), s_battery_mv.load(),
             reason != nullptr ? reason : "-");
    return actual == enabled;
}

bool stackchan_power_manager_dji_allowed()
{
    const StackchanPowerState state = static_cast<StackchanPowerState>(s_state.load());
    if (state == StackchanPowerState::kSafeMode) {
        return false;
    }
#if !CONFIG_STACKCHAN_DJI_MIC_LOW_BATTERY_PROTECTION
    return true;
#else
    if (state != StackchanPowerState::kNormal) {
        return false;
    }
    const int percent = s_battery_percent.load();
    const int battery_mv = s_battery_mv.load();
    if (valid_battery_percent(percent) && percent >= CONFIG_STACKCHAN_BATTERY_DJI_START_PERCENT) {
        return true;
    }
    if (valid_battery_mv(battery_mv)) {
        return battery_mv > CONFIG_STACKCHAN_BATTERY_LOW_MV;
    }
    return !valid_battery_percent(percent);
#endif
}

bool stackchan_power_manager_allow_camera()
{
    const StackchanPowerState state = static_cast<StackchanPowerState>(s_state.load());
    return !s_enumeration_lease.load() && state == StackchanPowerState::kNormal;
}

bool stackchan_power_manager_allow_servo()
{
    const StackchanPowerState state = static_cast<StackchanPowerState>(s_state.load());
    return !s_enumeration_lease.load() && state == StackchanPowerState::kNormal;
}

int stackchan_power_manager_limit_volume(int requested_percent)
{
    const StackchanPowerState state = static_cast<StackchanPowerState>(s_state.load());
    int limit = 100;
    if (s_enumeration_lease.load() || state == StackchanPowerState::kLow) {
        limit = kLowVolumeLimitPercent;
    } else if (state == StackchanPowerState::kCritical || state == StackchanPowerState::kSafeMode) {
        limit = kSafeVolumeLimitPercent;
    }
    return std::max(0, std::min(requested_percent, limit));
}

uint8_t stackchan_power_manager_limit_rgb(uint8_t requested)
{
    const StackchanPowerState state = static_cast<StackchanPowerState>(s_state.load());
    uint8_t limit = 255;
    if (s_enumeration_lease.load() || state == StackchanPowerState::kLow) {
        limit = kLowRgbLimit;
    } else if (state == StackchanPowerState::kCritical || state == StackchanPowerState::kSafeMode) {
        limit = kSafeRgbLimit;
    }
    return std::min(requested, limit);
}

extern "C" void usb_streaming_overcurrent_callback(void)
{
    ESP_LOGE(TAG, "USB host overcurrent detected; disabling VBUS and entering safe mode");
    s_state = static_cast<int>(StackchanPowerState::kSafeMode);
    stackchan_power_manager_set_usb_output(false, "USB overcurrent");
}
