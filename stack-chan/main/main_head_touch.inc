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
            mark_user_interaction("screen touch");
            LocalVoiceState state = local_voice_current_state();
            if (state == LocalVoiceState::Waiting) {
                ESP_LOGI(TAG, "Screen touch ignored while waiting: %s", head_touch_event_name(event));
                continue;
            }
            if (sleep_dark_is_visible()) {
                ESP_LOGI(TAG, "Screen touch wakes sleep-dark display: %s", head_touch_event_name(event));
                show_expression(kDefaultExpression);
                local_voice_request_state(LocalVoiceState::Listening, "screen touch wake");
                continue;
            }
            if (state == LocalVoiceState::Idle) {
                const char* reply = screen_touch_wake_reply_text();
                ESP_LOGI(TAG, "Screen touch wakes local voice: %s -> %s", head_touch_event_name(event), reply);
                show_expression(kDefaultExpression);
                local_voice_request_state(LocalVoiceState::Listening, "screen touch wake");
                if (!enqueue_speak_command("", reply, screen_touch_wake_reply_cache_name(reply), true)) {
                    execute_speak_command_internal(reply, true, screen_touch_wake_reply_cache_name(reply));
                }
                continue;
            }
            ESP_LOGI(TAG, "Screen touch local expression: %s -> shy", head_touch_event_name(event));
            show_temporary_expression("shy", kHeadTouchShyExpressionMs);
        }
    }
}

static void run_head_touch_loop()
{
    bool was_touched = false;
    ESP_LOGI(TAG, "Screen touch local shy trigger started");

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

        const bool is_touched = touch.isPressed() || touch.wasPressed() || touch.wasClicked();
        if (is_touched && !was_touched) {
            was_touched = true;
            enqueue_head_touch_event(HeadTouchEvent::Click);
        } else if (!touch.isPressed() && was_touched) {
            was_touched = false;
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
        }, "screen_touch_audio", kScreenTouchAudioTaskStackBytes, nullptr, 2, &head_touch_audio_task_handle, 0);
    }
    if (head_touch_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            run_head_touch_loop();
        }, "screen_touch", kScreenTouchTaskStackBytes, nullptr, 2, &head_touch_task_handle, 1);
    }
}
