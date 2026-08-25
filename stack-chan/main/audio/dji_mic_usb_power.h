#pragma once

#include "esp_err.h"

esp_err_t dji_mic_usb_power_prepare_host();
esp_err_t dji_mic_usb_power_enable_after_host();
void dji_mic_usb_power_abort(const char* reason);

