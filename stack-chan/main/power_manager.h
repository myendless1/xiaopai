#pragma once

#include <cstdint>

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

enum class StackchanPowerState : uint8_t {
    kNormal = 0,
    kLow,
    kCritical,
    kSafeMode,
};

struct StackchanPowerStatus {
    StackchanPowerState state = StackchanPowerState::kNormal;
    int battery_mv = -1;
    int battery_percent = -1;
    int battery_current_ma = 0;
    int vbus_mv = -1;
    bool charging = false;
    bool usb_output = false;
    bool enumeration_lease = false;
    bool dji_allowed = true;
    uint32_t brownout_boot_count = 0;
};

void stackchan_power_manager_init(SemaphoreHandle_t m5_mutex, esp_reset_reason_t reset_reason);
bool stackchan_power_manager_start();
StackchanPowerStatus stackchan_power_manager_status();

bool stackchan_power_manager_begin_usb_enumeration();
void stackchan_power_manager_end_usb_enumeration();
bool stackchan_power_manager_set_usb_output(bool enabled, const char* reason);
bool stackchan_power_manager_dji_allowed();

bool stackchan_power_manager_allow_camera();
bool stackchan_power_manager_allow_servo();
int stackchan_power_manager_limit_volume(int requested_percent);
uint8_t stackchan_power_manager_limit_rgb(uint8_t requested);

