static bool start_realtime_tts_playback()
{
    if (realtime_tts_playback_active) {
        return true;
    }

    if (!supervisor_set_speech_playback_active(true, "realtime tts")) {
        return false;
    }
    if (!audio_service_start()) {
        ESP_LOGE(TAG, "audio_service_start failed for realtime TTS");
        supervisor_set_speech_playback_active(false, "realtime tts begin failed");
        return false;
    }

    apply_speaker_volume();
    supervisor_set_app2_stop_requested(false, "realtime tts");
    realtime_tts_playback_active = true;
    expression_state_start_speech_visual_feedback();
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
    expression_state_stop_speech_visual_feedback();
    realtime_tts_playback_active = false;
    supervisor_set_speech_playback_active(false, "realtime tts finished");
}

static bool play_realtime_opus_frame(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0) {
        return true;
    }
    if (!start_realtime_tts_playback()) {
        return false;
    }

    if (!audio_service_play_opus_frame_24k(data, len)) {
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
             "{\"type\":\"hello\",\"version\":1,\"features\":{\"audio\":true,\"asr\":true},"
             "\"transport\":\"websocket\",\"audio_params\":{\"format\":\"opus\","
             "\"sample_rate\":%d,\"channels\":1,\"frame_duration\":%d}}",
             kAudioSampleRate, kOpusFrameDurationMs);
    return std::string(message);
}

static std::string make_listen_message(const char* state)
{
    return std::string("{\"session_id\":\"") + realtime_config.session_id +
           "\",\"type\":\"listen\",\"state\":\"" + state + "\",\"mode\":\"manual\"}";
}

static bool send_ws_frame(esp_transport_handle_t ws, ws_transport_opcodes_t opcode, const char* data, size_t len,
                          const char* label, int timeout_ms = 5000)
{
    static constexpr int kWsFinalFrameBit = 0x80;
    static constexpr int kMaxZeroWriteRetries = 3;
    ws_transport_opcodes_t final_opcode = static_cast<ws_transport_opcodes_t>(
        static_cast<int>(opcode) | kWsFinalFrameBit);
    int written = -1;
    for (int attempt = 1; attempt <= kMaxZeroWriteRetries; ++attempt) {
        written = esp_transport_ws_send_raw(ws, final_opcode, data, len, timeout_ms);
        ESP_LOGI(TAG, "WS发送%s opcode=%d 第%d次 written=%d expected=%u", label,
                 static_cast<int>(final_opcode), attempt, written, static_cast<unsigned>(len));
        if (written == static_cast<int>(len)) {
            return true;
        }
        if (written > 0) {
            break;
        }
        if (attempt < kMaxZeroWriteRetries) {
            vTaskDelay(pdMS_TO_TICKS(30 * attempt));
        }
    }
    ESP_LOGE(TAG, "WS发送%s失败或短写: written=%d expected=%u", label, written,
             static_cast<unsigned>(len));
    return false;
}

static bool send_ws_text(esp_transport_handle_t ws, const std::string& text, const char* label)
{
    return send_ws_frame(ws, WS_TRANSPORT_OPCODES_TEXT, text.c_str(), text.size(), label);
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
    LocalVoiceState current = xiaopai_state_get().state;
    if (current != LocalVoiceState::Idle && current != LocalVoiceState::Waiting) {
        return;
    }

    mark_user_interaction("server command");
    xiaopai_state_set(LocalVoiceState::Listening, "server command");
    expression_state_set(kDefaultExpression);
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
        realtime_config.session_id = json_string_value(root, "session_id");
        got_hello = transport == "websocket" || !realtime_config.session_id.empty();
        ESP_LOGI(TAG, "WS hello transport=%s session_id=%s", transport.c_str(), realtime_config.session_id.c_str());
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
    } else if (type == "device_state" || type == "state") {
        std::string state = json_string_value(root, "state");
        ESP_LOGI(TAG, "Device state=%s", state.c_str());
        if (state == "sleep" || state == "sleeping" || state == "idle") {
            show_dialog_sleeping("ws device_state");
        } else if (state == "wake" || state == "awake" || state == "listen" || state == "listening") {
            mark_user_interaction("ws device_state wake");
            xiaopai_state_set(LocalVoiceState::Listening, "ws device_state");
        } else if (state == "wait" || state == "waiting" || state == "thinking") {
            mark_user_interaction("ws device_state waiting");
            xiaopai_state_set(LocalVoiceState::Waiting, "ws device_state");
            if (state == "thinking") {
                expression_state_set("thinking");
            }
        } else if (state == "speak" || state == "speaking") {
            xiaopai_state_set(LocalVoiceState::Speaking, "ws device_state");
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
