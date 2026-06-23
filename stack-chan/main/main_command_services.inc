static bool execute_command_object_internal(const cJSON* command, bool in_sequence,
                                            bool* sequence_sleep_or_stop_executed);

static const cJSON* command_payload_or_self(const cJSON* command)
{
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
    if (!cJSON_IsObject(payload) && !cJSON_IsArray(payload)) {
        payload = command;
    }
    return payload;
}

static std::string command_expression_value(const cJSON* payload)
{
    std::string expression = json_string_value(payload, "expression");
    if (expression.empty()) {
        expression = json_string_value(payload, "face");
    }
    if (expression.empty()) {
        expression = json_string_value(payload, "name");
    }
    if (expression.empty()) {
        expression = json_string_value(payload, "action");
    }
    return expression;
}

static bool sequence_expression_should_restore(const std::string& expression)
{
    const std::string name = expression.empty() ? std::string(kDefaultExpression) : expression;
    if (name == "calm" || name == "default" || name == "listening" ||
        name == "wake" || name == "awake" ||
        name == "sleep_dark" || name == "screen_off" ||
        name == "sleep" || name == "sleeping" || name == "stopped" ||
        name == "node_head" || name == "nod_head") {
        return false;
    }
    return true;
}

static bool sequence_step_sets_temporary_expression(const cJSON* step)
{
    if (!cJSON_IsObject(step)) {
        return false;
    }
    std::string type = json_string_value(step, "type");
    const cJSON* payload = command_payload_or_self(step);
    if (type == "sequence" && cJSON_IsArray(payload)) {
        const int count = cJSON_GetArraySize(payload);
        for (int i = 0; i < count; ++i) {
            if (sequence_step_sets_temporary_expression(cJSON_GetArrayItem(payload, i))) {
                return true;
            }
        }
        return false;
    }
    if (type != "face" && type != "expression" && type != "action") {
        return false;
    }
    return sequence_expression_should_restore(command_expression_value(payload));
}

static bool command_is_sleep_or_stop(const cJSON* command)
{
    if (!cJSON_IsObject(command)) {
        return false;
    }
    std::string type = json_string_value(command, "type");
    const cJSON* payload = command_payload_or_self(command);
    if (type == "sleep" || type == "stop") {
        return true;
    }
    if (type == "state" || type == "device_state") {
        std::string state = json_string_value(payload, "state");
        if (state.empty()) {
            state = type;
        }
        return state == "sleep" || state == "sleeping" || state == "idle";
    }
    return false;
}

static bool sequence_step_failure_is_nonfatal(const cJSON* step)
{
    if (!cJSON_IsObject(step)) {
        return false;
    }
    if (json_bool_value(step, "continue_on_fail", false) ||
        json_bool_value(step, "continue_on_failure", false) ||
        json_bool_value(step, "optional", false)) {
        return true;
    }

    const cJSON* payload = command_payload_or_self(step);
    if (json_bool_value(payload, "continue_on_fail", false) ||
        json_bool_value(payload, "continue_on_failure", false) ||
        json_bool_value(payload, "optional", false)) {
        return true;
    }

    std::string type = json_string_value(step, "type");
    return type == "find_owner" || type == "locate_owner" ||
           type == "capture_image" || type == "track_once" || type == "camera";
}

static bool execute_command_object(const cJSON* command)
{
    return execute_command_object_internal(command, false, nullptr);
}

static bool execute_command_object_internal(const cJSON* command, bool in_sequence,
                                            bool* sequence_sleep_or_stop_executed)
{
    if (!cJSON_IsObject(command)) {
        return false;
    }

    std::string type = json_string_value(command, "type");
    const cJSON* payload = command_payload_or_self(command);
    if (in_sequence && sequence_sleep_or_stop_executed != nullptr && command_is_sleep_or_stop(command)) {
        *sequence_sleep_or_stop_executed = true;
    }
    if (type != "sleep" && type != "state" && type != "device_state") {
        mark_user_interaction("command");
    }

    if (type == "audio_test" || type == "codec_test" || type == "tone") {
        int sample_rate = json_int_value(payload, "sample_rate", kTtsStreamSampleRate);
        int tone_hz = json_int_value(payload, "freq", json_int_value(payload, "tone_hz", 1000));
        int duration_ms = json_int_value(payload, "duration_ms", 1200);
        int volume = json_int_value(payload, "volume", speaker_volume_percent);
        VoiceListenerPauseGuard voice_pause("audio test");
        if (audio_mutex != nullptr) {
            xSemaphoreTake(audio_mutex, portMAX_DELAY);
        }
        codec_audio_output_dump_state();
        bool ok = codec_audio_output_test_tone(sample_rate, tone_hz, duration_ms, volume);
        if (audio_mutex != nullptr) {
            xSemaphoreGive(audio_mutex);
        }
        return ok;
    }

    if (type == "mouth") {
        std::string mouth = json_string_value(payload, "mouth");
        if (mouth.empty()) {
            mouth = json_string_value(payload, "name");
        }
        if (mouth.empty()) {
            mouth = json_string_value(payload, "shape");
        }
        set_mouth_shape(mouth.c_str());
        return true;
    }

    if (type == "face" || type == "expression" || type == "action") {
        std::string expression = command_expression_value(payload);
        if (expression == "thinking") {
            local_voice_request_state(LocalVoiceState::Waiting, "thinking expression command");
        }
        std::string mouth = json_string_value(payload, "mouth");
        if (mouth.empty()) {
            mouth = json_string_value(payload, "mouth_shape");
        }
        show_expression_with_mouth(expression.empty() ? kDefaultExpression : expression.c_str(), mouth.c_str());
        return true;
    }
    if (type == "check_ota" || type == "ota_check" || type == "firmware_ota") {
        check_and_apply_firmware_ota_once();
        return true;
    }
    if (type == "state" || type == "device_state" || type == "waiting" || type == "thinking") {
        std::string state = json_string_value(payload, "state");
        if (state.empty()) {
            state = type;
        }
        if (state == "wait" || state == "waiting" || state == "thinking") {
            local_voice_request_state(LocalVoiceState::Waiting, "state command");
            if (state == "thinking") {
                show_expression("thinking");
            }
            return true;
        }
        if (state == "wake" || state == "awake" || state == "listen" || state == "listening") {
            mark_user_interaction("wake state command");
            local_voice_request_state(LocalVoiceState::Listening, "state command");
            return true;
        }
        if (state == "sleep" || state == "sleeping" || state == "idle") {
            show_sleep_dark_listening("state sleep command");
            return true;
        }
        if (state == "speak" || state == "speaking") {
            local_voice_request_state(LocalVoiceState::Speaking, "state command");
            return true;
        }
        ESP_LOGW(TAG, "Unknown state command: %s", state.c_str());
        return false;
    }
    if (type == "speak") {
        std::string text = json_string_value(payload, "text");
        std::string cache_name = json_string_value(payload, "cache_name");
        std::string voice = json_string_value(payload, "voice");
        int sample_rate = json_int_value(payload, "sample_rate", 0);
        int volume = json_int_value(payload, "volume", 0);
        int speech_rate = json_int_value(payload, "speech_rate", 0);
        int pitch_rate = json_int_value(payload, "pitch_rate", 0);
        bool animate_mouth = json_bool_value(payload, "animate_mouth",
                                  json_bool_value(payload, "mouth_animation",
                                      json_bool_value(payload, "lip_sync", true)));
        bool pause_listener = json_bool_value(payload, "pause_listener", false) ||
                              json_bool_value(payload, "pause_voice_listener", false);
        if (voice_recording_active) {
            bool queued = enqueue_speak_command("", text.c_str(), cache_name.c_str(), pause_listener, voice.c_str(),
                                                sample_rate, volume, speech_rate, pitch_rate, animate_mouth);
            if (in_sequence) {
                ESP_LOGI(TAG, "Sequence speak queued until current recording finishes: queued=%d",
                         static_cast<int>(queued));
            }
            return queued;
        }
        if (in_sequence) {
            wait_for_voice_recording_to_finish();
            if (voice_recording_active) {
                ESP_LOGW(TAG, "Sequence speak could not wait for voice recording to finish");
                return false;
            }
            return execute_speak_command_internal(text.c_str(), pause_listener, cache_name.c_str(), voice.c_str(),
                                                  sample_rate, volume, speech_rate, pitch_rate, animate_mouth);
        }
        return execute_speak_command_internal(text.c_str(), pause_listener, cache_name.c_str(), voice.c_str(),
                                              sample_rate, volume, speech_rate, pitch_rate, animate_mouth);
    }
    if (type == "volume" || type == "sound") {
        return execute_volume_command(payload);
    }
    if (type == "capture_image" || type == "track_once" || type == "camera") {
        run_camera_upload_app();
        return true;
    }
    if (type == "find_owner" || type == "locate_owner") {
        uint32_t now = M5.millis();
        if (wake_find_owner_pending &&
            static_cast<uint32_t>(now - wake_find_owner_requested_ms) < kWakeFindOwnerPendingMaxMs) {
            ESP_LOGI(TAG, "Server find-owner consumes wake pending request");
            wake_find_owner_pending = false;
            wake_find_owner_last_run_ms = now;
        } else if (wake_find_owner_last_run_ms != 0 &&
                   static_cast<uint32_t>(now - wake_find_owner_last_run_ms) < kWakeFindOwnerDuplicateGuardMs) {
            ESP_LOGI(TAG, "Skipping duplicate wake find-owner command");
            return true;
        }
        int rounds = static_cast<int>(json_number_value(payload, "rounds", kFindOwnerMaxRounds));
        float gain_x = static_cast<float>(json_number_value(payload, "gain_x", kFindOwnerYawGain));
        float gain_y = static_cast<float>(json_number_value(payload, "gain_y", kFindOwnerPitchGain));
        float stop_pixels = static_cast<float>(json_number_value(payload, "stop_pixels", kFindOwnerStopPixels));
        const cJSON* reply_item = cJSON_GetObjectItemCaseSensitive(payload, "reply");
        std::string reply = cJSON_IsString(reply_item) ? reply_item->valuestring : "";
        bool speak = json_bool_value(payload, "speak", true);
        if (!speak) {
            reply.clear();
        } else if (!cJSON_IsString(reply_item)) {
            reply = "我在";
        }
        bool preserve_speech = json_bool_value(payload, "preserve_speech", false);
        bool wait_for_speech = json_bool_value(payload, "wait_for_speech", false);
        return run_find_owner_command(rounds, reply.c_str(), gain_x, gain_y, stop_pixels, preserve_speech,
                                      wait_for_speech);
    }
    if (type == "motion" || type == "move") {
        std::string motion_type = json_string_value(payload, "type");
        if (motion_type == "motion" || motion_type == "move") {
            motion_type.clear();
        }
        if (motion_type.empty()) {
            motion_type = json_string_value(payload, "action");
        }
        if (motion_type.empty()) {
            motion_type = json_string_value(payload, "direction");
        }
        float degree = static_cast<float>(json_number_value(payload, "degree", json_number_value(payload, "degrees", 15)));
        float pan = static_cast<float>(json_number_value(payload, "pan", tracking_yaw_deg));
        float tilt = static_cast<float>(json_number_value(payload, "tilt", tracking_pitch_deg));
        int duration_ms = static_cast<int>(json_number_value(payload, "duration_ms", 500));

        if (motion_type == "left") {
            pan = tracking_yaw_deg - degree;
            tilt = tracking_pitch_deg;
        } else if (motion_type == "right") {
            pan = tracking_yaw_deg + degree;
            tilt = tracking_pitch_deg;
        } else if (motion_type == "up") {
            pan = tracking_yaw_deg;
            tilt = tracking_pitch_deg + degree;
        } else if (motion_type == "down") {
            pan = tracking_yaw_deg;
            tilt = tracking_pitch_deg - degree;
        } else if (motion_type == "center" || motion_type == "home") {
            pan = 0.0f;
            tilt = kTrackingHomePitchDeg;
        }
        return move_head_to_tracking_angles(pan, tilt, duration_ms);
    }
    if (type == "node_head" || type == "nod_head") {
        return run_node_head_command();
    }
    if (type == "sequence") {
        if (!cJSON_IsArray(payload)) {
            return false;
        }
        const int count = cJSON_GetArraySize(payload);
        const bool restore_expression = sequence_step_sets_temporary_expression(command);
        bool local_sleep_or_stop_executed = false;
        bool* sleep_or_stop_executed = sequence_sleep_or_stop_executed != nullptr
            ? sequence_sleep_or_stop_executed
            : &local_sleep_or_stop_executed;
        bool ok = true;
        for (int i = 0; i < count; ++i) {
            const cJSON* step = cJSON_GetArrayItem(payload, i);
            if (!execute_command_object_internal(step, true, sleep_or_stop_executed)) {
                if (sequence_step_failure_is_nonfatal(step)) {
                    std::string step_type = json_string_value(step, "type");
                    ESP_LOGW(TAG, "Sequence step failed but continuing: index=%d type=%s",
                             i, step_type.c_str());
                    continue;
                }
                ok = false;
                break;
            }
        }
        if (restore_expression && !*sleep_or_stop_executed && !in_sequence) {
            show_expression(kDefaultExpression);
        }
        return ok;
    }
    if (type == "stop") {
        app2_stop_requested = true;
        codec_audio_output_stop();
        show_expression("stopped");
        local_voice_request_state(LocalVoiceState::Idle, "stop command");
        return true;
    }
    if (type == "sleep") {
        app2_stop_requested = true;
        codec_audio_output_stop();
        show_sleep_dark_listening("sleep command");
        return true;
    }
    if (type == "wake" || type == "listen" || type == "listening") {
        mark_user_interaction("wake command");
        show_expression(kDefaultExpression);
        local_voice_request_state(LocalVoiceState::Listening, "command");
        return true;
    }
    if (type == "play_audio") {
        ESP_LOGW(TAG, "play_audio command is not implemented yet");
        return false;
    }

    ESP_LOGW(TAG, "Unknown command type: %s", type.c_str());
    return false;
}

static bool handle_command_response(const std::string& response)
{
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Command response is not JSON: %s", response.c_str());
        return false;
    }

    std::string response_type = json_string_value(root, "type");
    if (response_type == "noop") {
        cJSON_Delete(root);
        return true;
    }
    if (response_type != "command") {
        ESP_LOGW(TAG, "Unexpected command response type: %s", response_type.c_str());
        cJSON_Delete(root);
        return false;
    }

    const cJSON* command = cJSON_GetObjectItemCaseSensitive(root, "command");
    std::string cmd_id = json_string_value(command, "cmd_id");
    std::string cmd_type = json_string_value(command, "type");
    ESP_LOGI(TAG, "Command received: id=%s type=%s", cmd_id.c_str(), cmd_type.c_str());
    send_command_ack(cmd_id.c_str(), "received");
    if (cmd_type != "sleep" && cmd_type != "state" && cmd_type != "device_state") {
        mark_user_interaction("http command");
    }
    wake_for_server_command(cmd_type.c_str());

    if (cmd_type == "speak") {
        const cJSON* payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
        if (!cJSON_IsObject(payload)) {
            payload = command;
        }
        std::string text = json_string_value(payload, "text");
        std::string cache_name = json_string_value(payload, "cache_name");
        std::string voice = json_string_value(payload, "voice");
        int sample_rate = json_int_value(payload, "sample_rate", 0);
        int volume = json_int_value(payload, "volume", 0);
        int speech_rate = json_int_value(payload, "speech_rate", 0);
        int pitch_rate = json_int_value(payload, "pitch_rate", 0);
        bool animate_mouth = json_bool_value(payload, "animate_mouth",
                                  json_bool_value(payload, "mouth_animation",
                                      json_bool_value(payload, "lip_sync", true)));
        bool pause_listener = json_bool_value(payload, "pause_listener", false) ||
                              json_bool_value(payload, "pause_voice_listener", false);
        bool queued = enqueue_speak_command(cmd_id.c_str(), text.c_str(), cache_name.c_str(), pause_listener,
                                            voice.c_str(), sample_rate, volume, speech_rate, pitch_rate,
                                            animate_mouth);
        if (!queued) {
            send_command_ack(cmd_id.c_str(), "failed", "speak queue unavailable or empty text");
        }
        cJSON_Delete(root);
        return queued;
    }

    if (cmd_type == "stop") {
        request_speak_preempt("stop command");
    }

    bool ok = execute_command_object(command);
    char message[96] = {};
    if (!ok) {
        snprintf(message, sizeof(message), "command execution failed: %s", cmd_type.c_str());
    }
    send_command_ack(cmd_id.c_str(), ok ? "done" : "failed", message);
    cJSON_Delete(root);
    return ok;
}

static void run_command_http_loop()
{
    ensure_client_id();
    while (!wifi_is_connected() || !active_server_selected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "HTTP command long-poll started");
    show_expression(kDefaultExpression);

    while (true) {
        std::string url = make_server_url("/device/next-command");
        url += "?device_id=";
        url += url_encode(mac_address().c_str());
        url += "&timeout=";
        url += std::to_string(kCommandLongPollSeconds);
        ESP_LOGI(TAG, "HTTP command poll: %s", url.c_str());
        std::string response;
        if (http_get_string(url, &response, kCommandLongPollHttpTimeoutMs) && !response.empty()) {
            handle_command_response(response);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static void log_task_stack_watermark(const char* name, TaskHandle_t handle)
{
    if (handle == nullptr) {
        ESP_LOGI(TAG, "MEM task %-18s inactive", name);
        return;
    }
    ESP_LOGI(TAG, "MEM task %-18s stack_free=%u", name,
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(handle)));
}

static void log_memory_snapshot(const char* reason)
{
    uint32_t total_free = esp_get_free_heap_size();
    uint32_t total_min = esp_get_minimum_free_heap_size();
    uint32_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint32_t dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint32_t dma_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint32_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    uint32_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t spiram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t spiram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(TAG,
             "MEM %s total_free=%u total_min=%u internal_free=%u internal_min=%u internal_largest=%u "
             "dma_free=%u dma_min=%u dma_largest=%u spiram_free=%u spiram_min=%u spiram_largest=%u",
             reason != nullptr ? reason : "snapshot",
             static_cast<unsigned>(total_free), static_cast<unsigned>(total_min),
             static_cast<unsigned>(internal_free), static_cast<unsigned>(internal_min),
             static_cast<unsigned>(internal_largest), static_cast<unsigned>(dma_free),
             static_cast<unsigned>(dma_min), static_cast<unsigned>(dma_largest),
             static_cast<unsigned>(spiram_free), static_cast<unsigned>(spiram_min),
             static_cast<unsigned>(spiram_largest));
    ESP_LOGI(TAG,
             "MEM stack_budget voice=%u command=%u speak=%u touch=%u touch_audio=%u sedentary=%u monitor=%u boot=%u",
             static_cast<unsigned>(kApp1TaskStackBytes), static_cast<unsigned>(kCommandTaskStackBytes),
             static_cast<unsigned>(kSpeakCommandTaskStackBytes), static_cast<unsigned>(kScreenTouchTaskStackBytes),
             static_cast<unsigned>(kScreenTouchAudioTaskStackBytes), static_cast<unsigned>(kTrackingTaskStackBytes),
             static_cast<unsigned>(kMemoryMonitorTaskStackBytes), static_cast<unsigned>(kWifiTaskStackBytes));

    log_task_stack_watermark("boot", boot_task_handle);
    log_task_stack_watermark("bg_voice", xiaozhi_task_handle);
    log_task_stack_watermark("bg_command", command_task_handle);
    log_task_stack_watermark("speak_cmd", speak_command_task_handle);
    log_task_stack_watermark("screen_touch", head_touch_task_handle);
    log_task_stack_watermark("touch_audio", head_touch_audio_task_handle);
    log_task_stack_watermark("sedentary_owner", sedentary_task_handle);
    log_task_stack_watermark("mem_mon", memory_monitor_task_handle);
}

static void run_memory_monitor_loop()
{
    ESP_LOGI(TAG, "Memory monitor started");
    while (true) {
        log_memory_snapshot("periodic");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

static void start_memory_monitor_service()
{
    if (memory_monitor_task_handle != nullptr) {
        return;
    }
    BaseType_t created = xTaskCreatePinnedToCore([](void*) {
        run_memory_monitor_loop();
    }, "mem_mon", kMemoryMonitorTaskStackBytes, nullptr, 1, &memory_monitor_task_handle, 0);
    if (created == pdPASS) {
        ESP_LOGI(TAG, "Memory monitor task created");
    } else {
        memory_monitor_task_handle = nullptr;
        ESP_LOGE(TAG, "Failed to create memory monitor task");
    }
}

static void start_background_services()
{
    current_app = AppId::WifiConnect;
    voice_status_screen_suppressed = false;

    if (boot_task_handle != nullptr) {
        return;
    }

    xTaskCreatePinnedToCore([](void*) {
        ensure_client_id();
        while (!ensure_network_ready()) {
            ESP_LOGW(TAG, "WiFi or server not ready, retrying in 3 seconds");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        check_and_apply_firmware_ota_once();
        log_memory_snapshot("after ota");

        current_app = AppId::VoiceDemo;
        voice_status_screen_suppressed = true;
        show_expression(kDefaultExpression);
        enable_servo_power();
        log_memory_snapshot("before camera preinit");
        if (init_camera_once()) {
            ESP_LOGI(TAG, "Camera pre-initialized for find-owner flow");
            release_camera_driver();
        } else {
            ESP_LOGW(TAG, "Camera pre-initialization failed; will retry on demand");
        }
        log_memory_snapshot("after camera preinit");

        start_head_touch_services();
        start_speak_command_service();
        start_sedentary_find_owner_service();
        log_memory_snapshot("after helper tasks");

        if (xiaozhi_task_handle == nullptr) {
            app1_stop_requested = false;
            log_memory_snapshot("before bg_voice create");
            BaseType_t created = xTaskCreatePinnedToCore([](void*) {
                run_xiaozhi_ota_probe();
                xiaozhi_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "bg_voice", kApp1TaskStackBytes, nullptr, 3, &xiaozhi_task_handle, 1);
            if (created == pdPASS) {
                ESP_LOGI(TAG, "Realtime voice task created");
            } else {
                xiaozhi_task_handle = nullptr;
                ESP_LOGE(TAG, "Failed to create realtime voice task");
            }
            log_memory_snapshot("after bg_voice create");
        }

        start_memory_monitor_service();

        if (kUseHttpCommandLongPoll && command_task_handle == nullptr) {
            BaseType_t created = xTaskCreatePinnedToCore([](void*) {
                run_command_http_loop();
                command_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "bg_command", kCommandTaskStackBytes, nullptr, 4, &command_task_handle, 0);
            if (created == pdPASS) {
                ESP_LOGI(TAG, "HTTP command long-poll task created");
            } else {
                command_task_handle = nullptr;
                ESP_LOGE(TAG, "Failed to create HTTP command long-poll task");
            }
        }

        boot_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "bg_boot", kWifiTaskStackBytes, nullptr, 4, &boot_task_handle, 1);

}
