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
        if (!xiaopai_supervisor_admit(SupervisorJobKind::Speech, SupervisorSafetyClass::Normal,
                                      "audio test command")) {
            return false;
        }
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

    if (type == "face" || type == "expression" || type == "action") {
        std::string expression = command_expression_value(payload);
        if (expression == "thinking") {
            xiaopai_state_set(LocalVoiceState::Waiting, "thinking expression command");
        }
        expression_state_set(expression.empty() ? kDefaultExpression : expression.c_str());
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
            xiaopai_state_set(LocalVoiceState::Waiting, "state command");
            if (state == "thinking") {
                expression_state_set("thinking");
            }
            return true;
        }
        if (state == "wake" || state == "awake" || state == "listen" || state == "listening") {
            mark_user_interaction("wake state command");
            xiaopai_state_set(LocalVoiceState::Listening, "state command");
            return true;
        }
        if (state == "sleep" || state == "sleeping" || state == "idle") {
            show_dialog_sleeping("state sleep command");
            return true;
        }
        if (state == "speak" || state == "speaking") {
            xiaopai_state_set(LocalVoiceState::Speaking, "state command");
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
        if (voice_recording_active && !in_sequence) {
            bool queued = enqueue_speak_command("", text.c_str(), cache_name.c_str(), pause_listener, voice.c_str(),
                                                sample_rate, volume, speech_rate, pitch_rate);
            return queued;
        }
        if (in_sequence) {
            wait_for_voice_recording_to_finish();
            if (voice_recording_active) {
                ESP_LOGW(TAG, "Sequence speak could not wait for voice recording to finish");
                return false;
            }
            return execute_speak_command_internal(text.c_str(), pause_listener, cache_name.c_str(), voice.c_str(),
                                                  sample_rate, volume, speech_rate, pitch_rate);
        }
        return execute_speak_command_internal(text.c_str(), pause_listener, cache_name.c_str(), voice.c_str(),
                                              sample_rate, volume, speech_rate, pitch_rate);
    }
    if (type == "volume" || type == "sound") {
        return execute_volume_command(payload);
    }
    if (type == "capture_image" || type == "track_once" || type == "camera") {
        if (!xiaopai_supervisor_admit(SupervisorJobKind::Vision, SupervisorSafetyClass::Normal,
                                      "camera command")) {
            return false;
        }
        run_camera_upload_app();
        return true;
    }
    if (type == "find_owner" || type == "locate_owner") {
        if (!xiaopai_supervisor_admit(SupervisorJobKind::Vision, SupervisorSafetyClass::Normal,
                                      "find owner command")) {
            return false;
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
        if (!xiaopai_supervisor_admit(SupervisorJobKind::Motion, SupervisorSafetyClass::Normal,
                                      "motion command")) {
            return false;
        }
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
        if (!xiaopai_supervisor_admit(SupervisorJobKind::Motion, SupervisorSafetyClass::Normal,
                                      "nod head command")) {
            return false;
        }
        return run_node_head_command();
    }
    if (type == "sequence") {
        if (!cJSON_IsArray(payload)) {
            return false;
        }
        const int count = cJSON_GetArraySize(payload);
        if (count > 8) {
            ESP_LOGW(TAG, "Sequence rejected: too many steps count=%d", count);
            return false;
        }
        const bool restore_expression = sequence_step_sets_temporary_expression(command);
        begin_sequence_expression_hold(restore_expression);
        bool local_sleep_or_stop_executed = false;
        bool* sleep_or_stop_executed = sequence_sleep_or_stop_executed != nullptr
            ? sequence_sleep_or_stop_executed
            : &local_sleep_or_stop_executed;
        bool ok = true;
        for (int i = 0; i < count; ++i) {
            const cJSON* step = cJSON_GetArrayItem(payload, i);
            if (json_string_value(step, "type") == "sequence") {
                ESP_LOGW(TAG, "Sequence rejected: nested sequence at index=%d", i);
                ok = false;
                break;
            }
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
        end_sequence_expression_hold(restore_expression);
        if (restore_expression && !*sleep_or_stop_executed && !in_sequence) {
            expression_state_set(kDefaultExpression);
        }
        return ok;
    }
    if (type == "stop") {
        supervisor_set_app2_stop_requested(true, "stop command", SupervisorSafetyClass::LocalStop);
        codec_audio_output_stop();
        expression_state_set("stopped");
        xiaopai_state_set(LocalVoiceState::Idle, "stop command");
        return true;
    }
    if (type == "sleep") {
        supervisor_set_app2_stop_requested(true, "sleep command", SupervisorSafetyClass::LocalStop);
        codec_audio_output_stop();
        show_dialog_sleeping("sleep command");
        return true;
    }
    if (type == "wake" || type == "listen" || type == "listening") {
        mark_user_interaction("wake command");
        expression_state_set(kDefaultExpression);
        xiaopai_state_set(LocalVoiceState::Listening, "command");
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
    uint32_t attempt = static_cast<uint32_t>(json_int_value(command, "attempt", 0));
    uint32_t ttl_ms = static_cast<uint32_t>(json_int_value(command, "ttl_ms", 30000));
    bool persist_dedupe = cmd_type == "speak" || cmd_type == "sequence" || cmd_type == "motion" ||
                          cmd_type == "move" || cmd_type == "find_owner" || cmd_type == "locate_owner" ||
                          cmd_type == "capture_image" || cmd_type == "camera" || cmd_type == "track_once" ||
                          cmd_type == "check_ota" || cmd_type == "ota_check" || cmd_type == "firmware_ota";
    ESP_LOGI(TAG, "Command received: id=%s type=%s", cmd_id.c_str(), cmd_type.c_str());
    const CommandDedupeEntry* duplicate = command_dedupe_find(cmd_id.c_str());
    if (duplicate != nullptr) {
        ESP_LOGW(TAG, "Duplicate command suppressed: id=%s state=%s", cmd_id.c_str(), duplicate->state);
        send_command_ack(cmd_id.c_str(), duplicate->state, "duplicate command", duplicate->attempt);
        cJSON_Delete(root);
        return true;
    }
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
        uint32_t generation = static_cast<uint32_t>(json_int_value(payload, "generation", 0));
        uint16_t segment_index = static_cast<uint16_t>(json_int_value(payload, "segment_index", 0));
        bool pause_listener = json_bool_value(payload, "pause_listener", false) ||
                              json_bool_value(payload, "pause_voice_listener", false);
        bool queued = enqueue_speak_command(cmd_id.c_str(), text.c_str(), cache_name.c_str(), pause_listener,
                                            voice.c_str(), sample_rate, volume, speech_rate, pitch_rate,
                                            attempt, generation, segment_index, ttl_ms);
        if (!queued) {
            send_command_ack(cmd_id.c_str(), "failed", "speak queue unavailable or empty text", attempt);
        } else {
            send_command_ack(cmd_id.c_str(), "received", "", attempt);
            if (persist_dedupe) command_dedupe_mark_persistent(cmd_id.c_str());
        }
        cJSON_Delete(root);
        return queued;
    }

    if (cmd_type == "stop") {
        request_speak_preempt("stop command");
        const cJSON* payload = command_payload_or_self(command);
        advance_speech_generation(static_cast<uint32_t>(json_int_value(payload, "generation", 0)));
    }

    send_command_ack(cmd_id.c_str(), "received", "", attempt);
    if (persist_dedupe) command_dedupe_mark_persistent(cmd_id.c_str());
    bool ok = execute_command_object(command);
    char message[96] = {};
    if (!ok) {
        snprintf(message, sizeof(message), "command execution failed: %s", cmd_type.c_str());
    }
    send_command_ack(cmd_id.c_str(), ok ? "done" : "failed", message, attempt);
    cJSON_Delete(root);
    return ok;
}

static bool send_v3_control_hello()
{
    if (xiaopai_boot_id == 0) {
        xiaopai_boot_id = esp_random();
    }
    const esp_app_desc_t* app = esp_app_get_description();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "device_id", mac_address().c_str());
    cJSON_AddNumberToObject(root, "boot_id", xiaopai_boot_id);
    cJSON_AddStringToObject(root, "firmware_version", app != nullptr ? app->version : "unknown");
    cJSON_AddNumberToObject(root, "protocol_version", 3);
    cJSON_AddNumberToObject(root, "last_ack_seq", command_ack_seq);
    cJSON* capabilities = cJSON_AddArrayToObject(root, "capabilities");
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("internal_mic"));
#if CONFIG_STACKCHAN_DJI_MIC_USB_INPUT || CONFIG_STACKCHAN_DJI_MIC_AUTO_START
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("dji_uac"));
#endif
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("camera"));
    cJSON_AddItemToArray(capabilities, cJSON_CreateString("servo"));
    char* encoded = cJSON_PrintUnformatted(root);
    std::string body = encoded != nullptr ? encoded : "{}";
    if (encoded != nullptr) {
        cJSON_free(encoded);
    }
    cJSON_Delete(root);
    std::string response;
    return http_post_json_string(make_server_url("/v3/control/hello"), body, &response, 5000);
}

static bool send_v3_control_heartbeat()
{
    if (xiaopai_boot_id == 0) {
        xiaopai_boot_id = esp_random();
    }
    const esp_app_desc_t* app = esp_app_get_description();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddStringToObject(root, "device_id", mac_address().c_str());
    cJSON_AddNumberToObject(root, "boot_id", xiaopai_boot_id);
    cJSON_AddStringToObject(root, "firmware_version", app != nullptr ? app->version : "unknown");
    cJSON_AddNumberToObject(root, "last_ack_seq", command_ack_seq);
    cJSON_AddStringToObject(root, "mode", xiaopai_state_get().name);
    cJSON_AddStringToObject(root, "expression", expression_state_get().name);
    cJSON_AddNumberToObject(root, "free_internal_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root, "speech_queue_depth", speak_command_queue != nullptr ? uxQueueMessagesWaiting(speak_command_queue) : 0);

    char* encoded = cJSON_PrintUnformatted(root);
    std::string body = encoded != nullptr ? encoded : "{}";
    if (encoded != nullptr) {
        cJSON_free(encoded);
    }
    cJSON_Delete(root);
    std::string response;
    return http_post_json_string(make_server_url("/v3/control/heartbeat"), body, &response, 2000);
}

static void run_heartbeat_loop()
{
    ESP_LOGI(TAG, "Heartbeat task started");
    uint32_t count = 0;
    while (true) {
        if (wifi_is_connected() && active_server_selected) {
            send_v3_control_heartbeat();
        }
        if (++count % 30 == 0) {
            audio_service_log_diagnostics();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void run_command_http_loop()
{
    ensure_client_id();
    while (!wifi_is_connected() || !active_server_selected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "HTTP command long-poll started");
    expression_state_set(kDefaultExpression);
    restore_terminal_command_dedupe_log();
    send_v3_control_hello();

    while (true) {
        if (xiaopai_boot_id == 0) {
            xiaopai_boot_id = esp_random();
        }
        replay_pending_command_acks();
        std::string url = make_server_url("/v3/device/next-command");
        url += "?device_id=";
        url += url_encode(mac_address().c_str());
        url += "&boot_id=";
        url += std::to_string(xiaopai_boot_id);
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

static void command_task_entry(void*)
{
    ESP_LOGI(TAG, "Static HTTP command long-poll task started, waiting for network...");
    while (true) {
        run_command_http_loop();
        ESP_LOGE(TAG, "HTTP command long-poll loop exited unexpectedly; retrying in 1 second");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

bool create_command_task_early()
{
    if (!kUseHttpCommandLongPoll || command_task_handle != nullptr) {
        return true;
    }

    command_task_handle = xTaskCreateStaticPinnedToCore(
        command_task_entry,
        "control_transport_task",
        sizeof(s_command_stack),
        nullptr,
        5,
        s_command_stack,
        &s_command_tcb,
        0
    );
    if (command_task_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create static HTTP command long-poll task");
        return false;
    }
    ESP_LOGI(TAG, "Static HTTP command long-poll task created, stack=%u bytes",
             static_cast<unsigned>(sizeof(s_command_stack)));
    return true;
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
    log_task_stack_watermark("realtime_audio_task", realtime_task_handle);
    log_task_stack_watermark("control_transport_task", command_task_handle);
    log_task_stack_watermark("wifi_debug", wifi_debug_task_handle);
    log_task_stack_watermark("speech_task", speak_command_task_handle);
    log_task_stack_watermark("ui_task", head_touch_task_handle);
    log_task_stack_watermark("touch_audio", head_touch_audio_task_handle);
    log_task_stack_watermark("vision_health_task", sedentary_task_handle);
}

static void realtime_task_entry(void*)
{
    ESP_LOGI(TAG, "Static realtime task entry started, waiting for notification...");
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "Static realtime task notified, starting speech loop...");
        realtime_task_active.store(true);

        run_realtime_speech();

        realtime_task_active.store(false);
        ESP_LOGI(TAG, "Static realtime task speech loop finished, waiting again...");
    }
}

bool create_realtime_task_early()
{
    if (realtime_task_handle != nullptr) {
        return true;
    }

    realtime_task_handle = xTaskCreateStaticPinnedToCore(
        realtime_task_entry,
        "realtime_audio_task",
        sizeof(s_realtime_stack),
        nullptr,
        5,
        s_realtime_stack,
        &s_realtime_tcb,
        0
    );

    if (realtime_task_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create static realtime voice task");
        return false;
    }

    ESP_LOGI(TAG, "Static realtime task created, stack=%u bytes",
             static_cast<unsigned>(sizeof(s_realtime_stack)));
    return true;
}

static void start_background_services()
{
    supervisor_set_current_app(AppId::WifiConnect, "wifi provisioning command");
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

        supervisor_set_current_app(AppId::VoiceDemo, "wifi provisioning done");
        voice_status_screen_suppressed = true;
        expression_state_set(kDefaultExpression);
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
        start_wifi_debug_service();
        if (heartbeat_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                run_heartbeat_loop();
                heartbeat_task_handle = nullptr;
                audio_service_register_heartbeat_task(nullptr);
                vTaskDelete(nullptr);
            }, "heartbeat_task", 4 * 1024, nullptr, 1, &heartbeat_task_handle, 0);
            if (heartbeat_task_handle != nullptr) {
                audio_service_register_heartbeat_task(heartbeat_task_handle);
            }
        }
        start_sedentary_find_owner_service();
        log_memory_snapshot("after helper tasks");

        if (!realtime_task_active.load()) {
            supervisor_set_app1_stop_requested(false, "command starts voice demo");
            log_memory_snapshot("before bg_voice notify");
            if (realtime_task_handle != nullptr) {
                xTaskNotifyGive(realtime_task_handle);
                ESP_LOGI(TAG, "Realtime voice task notified");
            } else {
                ESP_LOGE(TAG, "Static realtime task not initialized");
            }
            log_memory_snapshot("after bg_voice notify");
        }

        if (kUseHttpCommandLongPoll && command_task_handle == nullptr) {
            ESP_LOGW(TAG, "HTTP command task missing after early boot; retrying static creation");
            create_command_task_early();
        }

        boot_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "bg_boot", kWifiTaskStackBytes, nullptr, 4, &boot_task_handle, 1);

}
