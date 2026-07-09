static bool start_realtime_tts_playback()
{
    if (realtime_tts_playback_active) {
        return true;
    }

    local_voice_begin_speaking("realtime tts");
    if (!audio_service_start()) {
        ESP_LOGE(TAG, "audio_service_start failed for realtime TTS");
        local_voice_end_speaking("realtime tts begin failed");
        return false;
    }

    apply_speaker_volume();
    app2_stop_requested = false;
    speech_playback_active = true;
    realtime_tts_playback_active = true;
    start_speech_visual_feedback();
    set_app1_status("Realtime TTS", "Playing server audio", "WebSocket Opus stream", "");
    return true;
}

static void finish_realtime_tts_playback()
{
    if (!realtime_tts_playback_active) {
        return;
    }
    uint32_t started_ms = M5.millis();
    if (!app2_stop_requested) {
        audio_service_wait_playback_idle(pdMS_TO_TICKS(1000));
        mark_speech_output_finished();
    }
    ESP_LOGI(TAG, "Realtime TTS playback %s after %u ms", app2_stop_requested ? "stopped" : "done",
             static_cast<unsigned>(M5.millis() - started_ms));
    stop_speech_visual_feedback();
    speech_playback_active = false;
    realtime_tts_playback_active = false;
    local_voice_end_speaking("realtime tts finished");
}

static bool play_realtime_opus_frame(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0) {
        return true;
    }
    if (!start_realtime_tts_playback()) {
        return false;
    }

    if (!audio_service_play_opus_frame_16k(data, len)) {
        ESP_LOGE(TAG, "Realtime Opus playback failed");
        return false;
    }
    return true;
}

static std::string json_string_value(const cJSON* root, const char* key)
{
    cJSON* item = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(item) ? std::string(item->valuestring) : std::string();
}

static bool execute_command_object(const cJSON* command);

static std::string make_ws_hello_message()
{
    char message[256];
    snprintf(message, sizeof(message),
             "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":true},"
             "\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\","
             "\"sample_rate\":%d,\"channels\":1,\"frame_duration\":%d}}",
             kAudioSampleRate, kOpusFrameDurationMs);
    return std::string(message);
}

static std::string make_listen_message(const char* state)
{
    return std::string("{\"session_id\":\"") + xiaozhi_config.session_id +
           "\",\"type\":\"listen\",\"state\":\"" + state + "\",\"mode\":\"manual\"}";
}

static bool send_ws_frame(esp_transport_handle_t ws, ws_transport_opcodes_t opcode, const char* data, size_t len,
                          const char* label, int timeout_ms = 5000)
{
    static constexpr int kWsFinalFrameBit = 0x80;
    ws_transport_opcodes_t final_opcode = static_cast<ws_transport_opcodes_t>(
        static_cast<int>(opcode) | kWsFinalFrameBit);
    int written = esp_transport_ws_send_raw(ws, final_opcode, data, len, timeout_ms);
    ESP_LOGI(TAG, "WS send %s opcode=%d written=%d expected=%u", label, static_cast<int>(final_opcode), written,
             static_cast<unsigned>(len));
    if (written < 0 || written < static_cast<int>(len)) {
        ESP_LOGE(TAG, "WS send %s failed or short write: written=%d expected=%u", label, written,
                 static_cast<unsigned>(len));
        return false;
    }
    return true;
}

static bool send_ws_text(esp_transport_handle_t ws, const std::string& text, const char* label)
{
    return send_ws_frame(ws, WS_TRANSPORT_OPCODES_TEXT, text.c_str(), text.size(), label);
}

static cJSON* duplicate_json_or_empty_object(const cJSON* item)
{
    if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
        cJSON* copy = cJSON_Duplicate(item, true);
        if (copy != nullptr) {
            return copy;
        }
    }
    return cJSON_CreateObject();
}

static cJSON* create_command_with_payload(const char* type, cJSON* payload)
{
    cJSON* command = cJSON_CreateObject();
    if (command == nullptr) {
        if (payload != nullptr) {
            cJSON_Delete(payload);
        }
        return nullptr;
    }
    cJSON_AddStringToObject(command, "type", type);
    cJSON_AddItemToObject(command, "payload", payload != nullptr ? payload : cJSON_CreateObject());
    return command;
}

static cJSON* create_mcp_tool_command(const std::string& tool_name, const cJSON* arguments)
{
    if (tool_name == "self.stackchan.sequence.run") {
        const cJSON* steps = cJSON_GetObjectItemCaseSensitive(arguments, "steps");
        return create_command_with_payload("sequence", duplicate_json_or_empty_object(steps));
    }
    if (tool_name == "self.stackchan.face.set_expression") {
        cJSON* payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "expression", json_string_value(arguments, "expression").c_str());
        return create_command_with_payload("face", payload);
    }
    if (tool_name == "self.stackchan.face.play_action") {
        cJSON* payload = cJSON_CreateObject();
        std::string action = json_string_value(arguments, "action");
        if (action.empty()) {
            action = json_string_value(arguments, "expression");
        }
        cJSON_AddStringToObject(payload, "expression", action.c_str());
        return create_command_with_payload("face", payload);
    }
    if (tool_name == "self.stackchan.head.move" || tool_name == "self.stackchan.head.set_pose") {
        return create_command_with_payload("motion", duplicate_json_or_empty_object(arguments));
    }
    if (tool_name == "self.stackchan.head.find_owner") {
        return create_command_with_payload("find_owner", duplicate_json_or_empty_object(arguments));
    }
    if (tool_name == "self.stackchan.camera.capture") {
        return create_command_with_payload("capture_image", duplicate_json_or_empty_object(arguments));
    }
    if (tool_name == "self.stackchan.volume.set") {
        cJSON* payload = duplicate_json_or_empty_object(arguments);
        cJSON_AddStringToObject(payload, "mode", "set");
        return create_command_with_payload("volume", payload);
    }
    if (tool_name == "self.stackchan.volume.adjust") {
        return create_command_with_payload("volume", duplicate_json_or_empty_object(arguments));
    }
    if (tool_name == "self.stackchan.stop") {
        return create_command_with_payload("stop", cJSON_CreateObject());
    }
    return nullptr;
}

static bool send_mcp_response(esp_transport_handle_t ws, const cJSON* request_payload, bool ok, const char* message)
{
    if (ws == nullptr) {
        return false;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* payload = cJSON_CreateObject();
    if (root == nullptr || payload == nullptr) {
        cJSON_Delete(root);
        cJSON_Delete(payload);
        return false;
    }
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "jsonrpc", "2.0");

    const cJSON* request_id = cJSON_GetObjectItemCaseSensitive(request_payload, "id");
    if (cJSON_IsString(request_id)) {
        cJSON_AddStringToObject(payload, "id", request_id->valuestring);
    } else if (cJSON_IsNumber(request_id)) {
        cJSON_AddNumberToObject(payload, "id", request_id->valuedouble);
    } else {
        cJSON_AddNullToObject(payload, "id");
    }

    if (ok) {
        cJSON* result = cJSON_CreateObject();
        cJSON_AddBoolToObject(result, "ok", true);
        cJSON_AddItemToObject(payload, "result", result);
    } else {
        cJSON* error = cJSON_CreateObject();
        cJSON_AddNumberToObject(error, "code", -32000);
        cJSON_AddStringToObject(error, "message", message != nullptr && message[0] != '\0' ? message : "tool call failed");
        cJSON_AddItemToObject(payload, "error", error);
    }

    char* text = cJSON_PrintUnformatted(root);
    bool sent = false;
    if (text != nullptr) {
        sent = send_ws_text(ws, std::string(text), "mcp response");
        cJSON_free(text);
    }
    cJSON_Delete(root);
    return sent;
}

static void wake_for_server_command(const char* command_type)
{
    const char* type = command_type != nullptr ? command_type : "";
    if (strcmp(type, "sleep") == 0 || strcmp(type, "stop") == 0 ||
        strcmp(type, "wake") == 0 || strcmp(type, "listen") == 0 || strcmp(type, "listening") == 0 ||
        strcmp(type, "state") == 0 || strcmp(type, "device_state") == 0 ||
        strcmp(type, "waiting") == 0 || strcmp(type, "thinking") == 0 ||
        strcmp(type, "check_ota") == 0 || strcmp(type, "ota_check") == 0 ||
        strcmp(type, "firmware_ota") == 0) {
        return;
    }
    LocalVoiceState current = local_voice_current_state();
    if (current != LocalVoiceState::Idle && current != LocalVoiceState::Waiting) {
        return;
    }

    mark_user_interaction("server command");
    local_voice_request_state(LocalVoiceState::Listening, "server command");
    wake_find_owner_pending = false;
    show_expression(kDefaultExpression);
}

static bool handle_mcp_message(esp_transport_handle_t ws, const cJSON* root)
{
    const cJSON* payload = cJSON_GetObjectItemCaseSensitive(root, "payload");
    if (!cJSON_IsObject(payload)) {
        ESP_LOGW(TAG, "MCP message without payload");
        return true;
    }

    std::string method = json_string_value(payload, "method");
    if (method.empty()) {
        ESP_LOGI(TAG, "MCP response/notification received");
        return true;
    }
    if (method != "tools/call") {
        ESP_LOGW(TAG, "Unsupported MCP method: %s", method.c_str());
        send_mcp_response(ws, payload, false, "unsupported MCP method");
        return true;
    }

    const cJSON* params = cJSON_GetObjectItemCaseSensitive(payload, "params");
    const cJSON* arguments = nullptr;
    std::string tool_name;
    if (cJSON_IsObject(params)) {
        tool_name = json_string_value(params, "name");
        arguments = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    }
    if (!cJSON_IsObject(arguments)) {
        arguments = nullptr;
    }

    cJSON* command = create_mcp_tool_command(tool_name, arguments);
    if (command == nullptr) {
        ESP_LOGW(TAG, "Unsupported MCP tool: %s", tool_name.c_str());
        send_mcp_response(ws, payload, false, "unsupported MCP tool");
        return true;
    }

    ESP_LOGI(TAG, "MCP tool call: %s", tool_name.c_str());
    wake_for_server_command(json_string_value(command, "type").c_str());
    bool ok = execute_command_object(command);
    cJSON_Delete(command);
    send_mcp_response(ws, payload, ok, ok ? "" : "command execution failed");
    return true;
}

static bool handle_ws_command_message(const cJSON* root)
{
    const cJSON* command = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsObject(command)) {
        ESP_LOGW(TAG, "WS command message without command object");
        return true;
    }
    std::string cmd_type = json_string_value(command, "type");
    ESP_LOGI(TAG, "WS command received: type=%s", cmd_type.c_str());
    wake_for_server_command(cmd_type.c_str());
    execute_command_object(command);
    return true;
}

static bool handle_ws_text_message(esp_transport_handle_t ws, const char* data, size_t len, bool& got_hello,
                                   std::string& stt_text)
{
    ESP_LOGI(TAG, "WS text (%u bytes): %.*s", static_cast<unsigned>(len), static_cast<int>(len), data);

    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGW(TAG, "WS text is not JSON");
        return false;
    }

    std::string type = json_string_value(root, "type");
    if (type == "hello") {
        std::string transport = json_string_value(root, "transport");
        xiaozhi_config.session_id = json_string_value(root, "session_id");
        got_hello = transport == "websocket" || !xiaozhi_config.session_id.empty();
        ESP_LOGI(TAG, "WS hello transport=%s session_id=%s", transport.c_str(), xiaozhi_config.session_id.c_str());
    } else if (type == "stt") {
        stt_text = json_string_value(root, "text");
        ESP_LOGI(TAG, "STT result: %s", stt_text.c_str());
        show_recognition_text("Speech Text", stt_text.c_str());
    } else if (type == "tts") {
        std::string state = json_string_value(root, "state");
        std::string text = json_string_value(root, "text");
        ESP_LOGI(TAG, "TTS state=%s text=%s", state.c_str(), text.c_str());
        if (state == "start" || state == "sentence_start") {
            start_realtime_tts_playback();
        } else if (state == "stop") {
            finish_realtime_tts_playback();
        }
        if (!text.empty()) {
            set_app1_status("TTS Text", text.c_str(), "Streaming audio over WebSocket", "", false, true);
        }
    } else if (type == "llm") {
        std::string text = json_string_value(root, "text");
        ESP_LOGI(TAG, "LLM text=%s", text.c_str());
    } else if (type == "mcp") {
        handle_mcp_message(ws, root);
    } else if (type == "command") {
        handle_ws_command_message(root);
    } else if (type == "device_state" || type == "state") {
        std::string state = json_string_value(root, "state");
        ESP_LOGI(TAG, "Device state=%s", state.c_str());
        if (state == "sleep" || state == "sleeping" || state == "idle") {
            show_sleep_dark_listening("ws device_state");
        } else if (state == "wake" || state == "awake" || state == "listen" || state == "listening") {
            mark_user_interaction("ws device_state wake");
            local_voice_request_state(LocalVoiceState::Listening, "ws device_state");
        } else if (state == "wait" || state == "waiting" || state == "thinking") {
            mark_user_interaction("ws device_state waiting");
            local_voice_request_state(LocalVoiceState::Waiting, "ws device_state");
            if (state == "thinking") {
                show_expression("thinking");
            }
        } else if (state == "speak" || state == "speaking") {
            local_voice_request_state(LocalVoiceState::Speaking, "ws device_state");
        }
    } else if (!type.empty()) {
        ESP_LOGI(TAG, "Unhandled WS message type=%s", type.c_str());
    }

    cJSON_Delete(root);
    return true;
}

static bool receive_ws_once(esp_transport_handle_t ws, int timeout_ms, bool& got_hello, std::string& stt_text)
{
    int polled = esp_transport_poll_read(ws, timeout_ms);
    if (polled == 0) {
        return true;
    }
    if (polled < 0) {
        ESP_LOGE(TAG, "WS poll failed");
        return false;
    }

    char buffer[4096];
    int read_len = esp_transport_read(ws, buffer, sizeof(buffer) - 1, timeout_ms);
    if (read_len <= 0) {
        ESP_LOGE(TAG, "WS read failed: %d", read_len);
        return false;
    }

    ws_transport_opcodes_t opcode = esp_transport_ws_get_read_opcode(ws);
    if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
        buffer[read_len] = '\0';
        return handle_ws_text_message(ws, buffer, read_len, got_hello, stt_text);
    }
    if (opcode == WS_TRANSPORT_OPCODES_BINARY) {
        ESP_LOGD(TAG, "WS binary audio from server (%d bytes)", read_len);
        return play_realtime_opus_frame(reinterpret_cast<const uint8_t*>(buffer), read_len);
    }
    if (opcode == WS_TRANSPORT_OPCODES_CLOSE) {
        ESP_LOGW(TAG, "WS close frame received");
        return false;
    }

    ESP_LOGI(TAG, "WS opcode=%d len=%d", static_cast<int>(opcode), read_len);
    return true;
}
