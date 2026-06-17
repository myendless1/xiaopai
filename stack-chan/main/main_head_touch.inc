static const char* head_touch_event_name(HeadTouchEvent event)
{
    switch (event) {
        case HeadTouchEvent::Press:
            return "press";
        case HeadTouchEvent::Click:
            return "click";
        case HeadTouchEvent::SwipeForward:
            return "swipe_forward";
        case HeadTouchEvent::SwipeBackward:
            return "swipe_backward";
    }
    return "press";
}

static void enqueue_head_touch_event(HeadTouchEvent event)
{
    if (head_touch_event_queue == nullptr) {
        return;
    }
    xQueueSend(head_touch_event_queue, &event, 0);
    ESP_LOGI(TAG, "Head touch event: %s", head_touch_event_name(event));
}

static bool si12t_write_reg(uint8_t reg, uint8_t value)
{
    M5Lock lock;
    return M5.In_I2C.writeRegister8(kSi12tAddress, reg, value, kSi12tI2cFreq);
}

static bool si12t_read_reg(uint8_t reg, uint8_t* value)
{
    if (value == nullptr) {
        return false;
    }
    M5Lock lock;
    if (!M5.In_I2C.scanID(kSi12tAddress, kSi12tI2cFreq)) {
        return false;
    }
    *value = M5.In_I2C.readRegister8(kSi12tAddress, reg, kSi12tI2cFreq);
    return true;
}

static bool init_head_touch_sensor()
{
    {
        M5Lock lock;
        if (!M5.In_I2C.scanID(kSi12tAddress, kSi12tI2cFreq)) {
            ESP_LOGW(TAG, "Si12T head touch sensor not found at 0x%02x", kSi12tAddress);
            return false;
        }
    }

    bool ok = true;
    ok &= si12t_write_reg(kSi12tRefRst1Reg, 0x00);
    ok &= si12t_write_reg(kSi12tRefRst2Reg, 0x00);
    ok &= si12t_write_reg(kSi12tChHold1Reg, 0x00);
    ok &= si12t_write_reg(kSi12tChHold2Reg, 0x00);
    ok &= si12t_write_reg(kSi12tCalHold1Reg, 0x00);
    ok &= si12t_write_reg(kSi12tCalHold2Reg, 0x00);
    ok &= si12t_write_reg(kSi12tCtrl2Reg, 0x0f);
    ok &= si12t_write_reg(kSi12tCtrl2Reg, 0x07);
    ok &= si12t_write_reg(kSi12tCtrl1Reg, 0x22);
    for (uint8_t reg = kSi12tSensitivity1Reg; reg < kSi12tSensitivity1Reg + 5; ++reg) {
        ok &= si12t_write_reg(reg, 0xcc);
    }
    ESP_LOGI(TAG, "Si12T head touch init %s", ok ? "ok" : "failed");
    return ok;
}

static bool read_head_touch_intensities(uint8_t intensities[3])
{
    uint8_t raw = 0;
    if (!si12t_read_reg(kSi12tOutput1Reg, &raw)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        intensities[(3 - 1) - i] = (raw >> (i * 2)) & 0x03;
    }
    return true;
}

static bool head_touch_is_touched(const uint8_t intensities[3])
{
    return intensities[0] > 0 || intensities[1] > 0 || intensities[2] > 0;
}

static int16_t head_touch_position(const uint8_t intensities[3])
{
    uint16_t total = intensities[0] + intensities[1] + intensities[2];
    if (total == 0) {
        return 0;
    }
    int32_t weighted = static_cast<int32_t>(intensities[0]) * -100 + static_cast<int32_t>(intensities[2]) * 100;
    return static_cast<int16_t>(weighted / total);
}

static void run_head_touch_audio_loop()
{
    while (true) {
        HeadTouchEvent event = HeadTouchEvent::Press;
        if (xQueueReceive(head_touch_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            if (local_voice_current_state() == LocalVoiceState::Waiting) {
                ESP_LOGI(TAG, "Head touch ignored while waiting: %s", head_touch_event_name(event));
                continue;
            }
            ESP_LOGI(TAG, "Head touch local expression: %s -> shy", head_touch_event_name(event));
            show_temporary_expression("shy", kHeadTouchShyExpressionMs);
        }
    }
}

static void run_head_touch_loop()
{
    if (!init_head_touch_sensor()) {
        head_touch_task_handle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    bool was_touched = false;
    bool swipe_reported = false;
    int16_t initial_position = 0;
    int64_t touch_started_us = 0;
    uint8_t intensities[3] = {};

    while (true) {
        if (camera_owns_internal_i2c) {
            vTaskDelay(pdMS_TO_TICKS(kHeadTouchPollMs));
            continue;
        }

        if (!read_head_touch_intensities(intensities)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        bool touched = head_touch_is_touched(intensities);
        int16_t position = head_touch_position(intensities);
        int64_t now_us = esp_timer_get_time();

        if (touched && !was_touched) {
            was_touched = true;
            swipe_reported = false;
            initial_position = position;
            touch_started_us = now_us;
        } else if (touched && was_touched && !swipe_reported) {
            int16_t delta = position - initial_position;
            if (delta > kHeadTouchSwipeThreshold) {
                swipe_reported = true;
                enqueue_head_touch_event(HeadTouchEvent::SwipeBackward);
            } else if (delta < -kHeadTouchSwipeThreshold) {
                swipe_reported = true;
                enqueue_head_touch_event(HeadTouchEvent::SwipeForward);
            }
        } else if (!touched && was_touched) {
            uint32_t held_ms = static_cast<uint32_t>((now_us - touch_started_us) / 1000);
            if (!swipe_reported && held_ms <= kHeadTouchClickMaxMs) {
                enqueue_head_touch_event(HeadTouchEvent::Click);
            } else if (!swipe_reported) {
                enqueue_head_touch_event(HeadTouchEvent::Press);
            }
            was_touched = false;
        }

        vTaskDelay(pdMS_TO_TICKS(kHeadTouchPollMs));
    }
}

static void start_head_touch_services()
{
    if (head_touch_event_queue == nullptr) {
        head_touch_event_queue = xQueueCreate(8, sizeof(HeadTouchEvent));
    }
    if (head_touch_event_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create head touch event queue");
        return;
    }
    if (head_touch_audio_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            run_head_touch_audio_loop();
        }, "headtouch_audio", kHeadTouchAudioTaskStackBytes, nullptr, 2, &head_touch_audio_task_handle, 0);
    }
    if (head_touch_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            run_head_touch_loop();
        }, "headtouch", kHeadTouchTaskStackBytes, nullptr, 2, &head_touch_task_handle, 1);
    }
}
