static bool execute_command_object(const cJSON* command)
{
    if (!cJSON_IsObject(command)) {
        return false;
    }

    std::string type = json_string_value(command, "type");
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(command, "payload");
    if (!cJSON_IsObject(payload) && !cJSON_IsArray(payload)) {
        payload = command;
    }

    if (type == "face") {
        std::string expression = json_string_value(payload, "expression");
        if (speech_output_is_busy()) {
            expression_set_speech_overridden(true);
            cancel_speaking_animation_now();
        }
        show_expression(expression.empty() ? kDefaultExpression : expression.c_str());
        return true;
    }
    if (type == "state" || type == "device_state" || type == "waiting" || type == "thinking") {
        std::string state = json_string_value(payload, "state");
        if (state.empty()) {
            state = type;
        }
        if (state == "wait" || state == "waiting" || state == "thinking") {
            local_voice_request_state(LocalVoiceState::Waiting, "state command");
            return true;
        }
        if (state == "wake" || state == "awake" || state == "listen" || state == "listening") {
            local_voice_request_state(LocalVoiceState::Listening, "state command");
            return true;
        }
        if (state == "sleep" || state == "sleeping" || state == "idle") {
            local_voice_request_state(LocalVoiceState::Idle, "state command");
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
        bool pause_listener = json_bool_value(payload, "pause_listener", false) ||
                              json_bool_value(payload, "pause_voice_listener", false);
        if (voice_recording_active) {
            return enqueue_speak_command("", text.c_str(), cache_name.c_str(), pause_listener, voice.c_str(),
                                         sample_rate, volume, speech_rate, pitch_rate);
        }
        return execute_speak_command_internal(text.c_str(), pause_listener, cache_name.c_str(), voice.c_str(),
                                              sample_rate, volume, speech_rate, pitch_rate);
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
        if (!cJSON_IsString(reply_item)) {
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
        for (int i = 0; i < count; ++i) {
            const cJSON* step = cJSON_GetArrayItem(payload, i);
            if (!execute_command_object(step)) {
                return false;
            }
        }
        return true;
    }
    if (type == "stop") {
        app2_stop_requested = true;
        M5.Speaker.stop();
        show_expression("stopped");
        local_voice_request_state(LocalVoiceState::Idle, "stop command");
        return true;
    }
    if (type == "sleep") {
        app2_stop_requested = true;
        M5.Speaker.stop();
        show_expression("sleep_dark");
        local_voice_request_state(LocalVoiceState::Idle, "sleep command");
        return true;
    }
    if (type == "wake" || type == "listen" || type == "listening") {
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
        bool pause_listener = json_bool_value(payload, "pause_listener", false) ||
                              json_bool_value(payload, "pause_voice_listener", false);
        bool queued = enqueue_speak_command(cmd_id.c_str(), text.c_str(), cache_name.c_str(), pause_listener,
                                            voice.c_str(), sample_rate, volume, speech_rate, pitch_rate);
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
    show_expression(kDefaultExpression);

    while (true) {
        std::string url = make_server_url("/device/next-command");
        url += "?device_id=";
        url += url_encode(mac_address().c_str());
        url += "&timeout=";
        url += std::to_string(kCommandLongPollSeconds);
        std::string response;
        if (http_get_string(url, &response, kCommandLongPollHttpTimeoutMs) && !response.empty()) {
            handle_command_response(response);
        } else {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
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

        show_expression(kDefaultExpression);
        enable_servo_power();
        if (init_camera_once()) {
            ESP_LOGI(TAG, "Camera pre-initialized for find-owner flow");
            release_camera_driver();
        } else {
            ESP_LOGW(TAG, "Camera pre-initialization failed; will retry on demand");
        }
        current_app = AppId::VoiceDemo;
        voice_status_screen_suppressed = true;
        start_head_touch_services();
        start_speak_command_service();

        if (xiaozhi_task_handle == nullptr) {
            app1_stop_requested = false;
            xTaskCreatePinnedToCore([](void*) {
                run_xiaozhi_ota_probe();
                xiaozhi_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "bg_voice", kApp1TaskStackBytes, nullptr, 3, &xiaozhi_task_handle, 1);
        }

        if (kUseHttpCommandLongPoll && command_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                run_command_http_loop();
                command_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "bg_command", kCommandTaskStackBytes, nullptr, 3, &command_task_handle, 0);
        }

        boot_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "bg_boot", kWifiTaskStackBytes, nullptr, 4, &boot_task_handle, 1);

}
