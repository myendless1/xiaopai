static const char* head_touch_event_name(HeadTouchEvent event)
{
    switch (event) {
        case HeadTouchEvent::Press:
            return "press";
        case HeadTouchEvent::Click:
            return "click";
        case HeadTouchEvent::LongPress:
            return "long_press";
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
    ESP_LOGI(TAG, "Screen touch event: %s", head_touch_event_name(event));
}

static const char* screen_touch_wake_reply_text()
{
    static constexpr const char* kReplies[] = {"你好呀", "你好~", "干嘛呀"};
    return kReplies[esp_random() % (sizeof(kReplies) / sizeof(kReplies[0]))];
}

static const char* screen_touch_wake_reply_cache_name(const char* text)
{
    if (text != nullptr && strcmp(text, "你好呀") == 0) {
        return "wake_reply_hello";
    }
    return "";
}

static void run_head_touch_audio_loop()
{
    while (true) {
        HeadTouchEvent event = HeadTouchEvent::Press;
        if (xQueueReceive(head_touch_event_queue, &event, portMAX_DELAY) == pdTRUE) {
            if (event == HeadTouchEvent::LongPress) {
                ESP_LOGW(TAG, "Screen 3-second press session reset triggered");
                request_speak_preempt("session reset");
                advance_speech_generation();
                cJSON* root = cJSON_CreateObject();
                cJSON_AddStringToObject(root, "device_id", mac_address().c_str());
                cJSON_AddStringToObject(root, "type", "reset_session");
                cJSON_AddNumberToObject(root, "generation", speech_command_generation);
                char* encoded = cJSON_PrintUnformatted(root);
                std::string body = encoded != nullptr ? encoded : "{}";
                if (encoded != nullptr) cJSON_free(encoded);
                cJSON_Delete(root);
                std::string response;
                if (!http_post_json_string(make_server_url("/device/event"), body, &response, 5000)) {
                    ESP_LOGW(TAG, "Failed to request Morrow session reset");
                }
                continue;
            }
            LocalVoiceState state = xiaopai_state_get().state;
            if (state == LocalVoiceState::DialogSleeping) {
                ESP_LOGI(TAG, "Screen touch ignored while dialog sleeping: %s", head_touch_event_name(event));
                continue;
            }
            mark_user_interaction("screen touch");
            if (state == LocalVoiceState::Waiting) {
                ESP_LOGI(TAG, "Screen touch ignored while waiting: %s", head_touch_event_name(event));
                continue;
            }
            if (expression_state_get().sleep_dark) {
                ESP_LOGI(TAG, "Screen touch wakes sleep-dark display: %s", head_touch_event_name(event));
                expression_state_set(kDefaultExpression);
                xiaopai_state_set(LocalVoiceState::Listening, "screen touch wake");
                continue;
            }
            if (state == LocalVoiceState::Idle) {
                const char* reply = screen_touch_wake_reply_text();
                ESP_LOGI(TAG, "Screen touch wakes local voice: %s -> %s", head_touch_event_name(event), reply);
                expression_state_set(kDefaultExpression);
                xiaopai_state_set(LocalVoiceState::Listening, "screen touch wake");
                if (!enqueue_speak_command("", reply, screen_touch_wake_reply_cache_name(reply), true)) {
                    execute_speak_command_internal(reply, true, screen_touch_wake_reply_cache_name(reply));
                }
                continue;
            }
            ESP_LOGI(TAG, "Screen touch local expression: %s -> shy", head_touch_event_name(event));
            expression_state_set_temporary("shy", kHeadTouchShyExpressionMs);
        }
    }
}

static void run_head_touch_loop()
{
    bool was_touched = false;
    bool long_press_sent = false;
    uint32_t pressed_at_ms = 0;
    static constexpr uint32_t kSessionResetLongPressMs = 3000;
    ESP_LOGI(TAG, "Screen touch handler started; hold 3 seconds to reset the Morrow session");

    while (true) {
        m5::Touch_Class::touch_detail_t touch;
        bool volume_dropdown_consumed = false;
        bool redraw_after_dropdown = false;
        {
            M5Lock lock;
            touch = M5.Touch.getDetail();
            volume_dropdown_consumed = handle_volume_dropdown_touch_locked(touch, &redraw_after_dropdown);
        }

        if (redraw_after_dropdown) {
            redraw_after_volume_dropdown();
        }

        if (volume_dropdown_consumed) {
            was_touched = touch.isPressed();
            vTaskDelay(pdMS_TO_TICKS(kScreenTouchPollMs));
            continue;
        }

        const bool is_touched = touch.isPressed() || touch.wasPressed();
        if (is_touched && !was_touched) {
            was_touched = true;
            long_press_sent = false;
            pressed_at_ms = M5.millis();
        } else if (touch.isPressed() && was_touched && !long_press_sent &&
                   static_cast<uint32_t>(M5.millis() - pressed_at_ms) >= kSessionResetLongPressMs) {
            long_press_sent = true;
            enqueue_head_touch_event(HeadTouchEvent::LongPress);
        } else if (!touch.isPressed() && was_touched) {
            was_touched = false;
            if (!long_press_sent) {
                enqueue_head_touch_event(HeadTouchEvent::Click);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(kScreenTouchPollMs));
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
        }, "ui_audio_task", kScreenTouchAudioTaskStackBytes, nullptr, 2, &head_touch_audio_task_handle, 1);
    }
    if (head_touch_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            run_head_touch_loop();
        }, "ui_task", kScreenTouchTaskStackBytes, nullptr, 2, &head_touch_task_handle, 1);
    }
}
