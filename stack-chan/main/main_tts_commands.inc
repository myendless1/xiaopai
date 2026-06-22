static bool play_stream_pcm_chunk(const int16_t* samples, size_t sample_count)
{
    if (sample_count == 0 || app2_stop_requested) {
        return true;
    }
    update_speaking_light_level(samples, sample_count);
    if (!M5.Speaker.playRaw(samples, sample_count, kTtsStreamSampleRate, false, 1, 0, false)) {
        set_tts_status("Play Fail", "M5.Speaker.playRaw failed", "", "", false, false);
        return false;
    }
    return true;
}

static void drain_speaker_playback()
{
    uint32_t started_ms = M5.millis();
    while (M5.Speaker.isPlaying() && !app2_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!app2_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(kSpeakerDmaTailDrainMs));
        mark_speech_output_finished();
    }
    ESP_LOGI(TAG, "Speaker playback %s after %u ms", app2_stop_requested ? "stopped" : "done",
             static_cast<unsigned>(M5.millis() - started_ms));
}

static bool stream_pcm_url(const std::string& url, const char* label, bool update_tts_status)
{
    ESP_LOGI(TAG, "Stream PCM URL: %s", url.c_str());
    if (update_tts_status) {
        set_tts_status("Requesting", url.c_str(), label != nullptr ? label : "");
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 60000;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        if (update_tts_status) {
            set_tts_status("HTTP Fail", "esp_http_client_init failed", "", "", false, false);
        }
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/octet-stream");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream PCM open failed: %s", esp_err_to_name(err));
        if (update_tts_status) {
            set_tts_status("HTTP Fail", esp_err_to_name(err), "Check PCM URL/IP", "", false, false);
        }
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Stream PCM HTTP status=%d content_length=%d", status, content_length);
    if (status < 200 || status >= 300) {
        if (update_tts_status) {
            char line[48];
            snprintf(line, sizeof(line), "HTTP status %d", status);
            set_tts_status("HTTP Status", line, "Expected PCM stream", "", false, false);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    if (update_tts_status) {
        set_tts_status("Playing", "Receiving pcm_s16le", "16kHz mono stream", "");
    }
    std::vector<std::vector<int16_t>> buffers(3, std::vector<int16_t>(kTtsStreamBufferSamples));
    int buffer_index = 0;
    size_t sample_pos = 0;
    uint8_t pending_byte = 0;
    bool has_pending_byte = false;
    size_t total_bytes = 0;
    uint8_t read_buffer[kHttpBufferSize];

    auto flush_samples = [&]() -> bool {
        if (sample_pos == 0) {
            return true;
        }
        bool ok = play_stream_pcm_chunk(buffers[buffer_index].data(), sample_pos);
        buffer_index = (buffer_index + 1) % buffers.size();
        sample_pos = 0;
        return ok;
    };

    while (!app2_stop_requested) {
        int read_len = esp_http_client_read(client, reinterpret_cast<char*>(read_buffer), sizeof(read_buffer));
        if (read_len < 0) {
            ESP_LOGE(TAG, "Stream PCM read failed");
            if (update_tts_status) {
                set_tts_status("Read Fail", "HTTP body read failed", "", "", false, false);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (read_len == 0) {
            break;
        }
        total_bytes += read_len;

        int offset = 0;
        if (has_pending_byte && read_len > 0) {
            buffers[buffer_index][sample_pos++] = static_cast<int16_t>(pending_byte | (read_buffer[0] << 8));
            has_pending_byte = false;
            offset = 1;
            if (sample_pos == kTtsStreamBufferSamples && !flush_samples()) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return false;
            }
        }

        while (offset + 1 < read_len) {
            buffers[buffer_index][sample_pos++] =
                static_cast<int16_t>(read_buffer[offset] | (read_buffer[offset + 1] << 8));
            offset += 2;
            if (sample_pos == kTtsStreamBufferSamples && !flush_samples()) {
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return false;
            }
        }

        if (offset < read_len) {
            pending_byte = read_buffer[offset];
            has_pending_byte = true;
        }
    }

    if (!app2_stop_requested && !flush_samples()) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    drain_speaker_playback();
    M5.Speaker.stop();

    if (app2_stop_requested) {
        if (update_tts_status) {
            set_tts_status("Stopped", "TTS stopped", "", "", false, false);
        }
        return false;
    }

    if (update_tts_status) {
        char line[64];
        snprintf(line, sizeof(line), "streamed %u bytes", static_cast<unsigned>(total_bytes));
        set_tts_status("Done", line, "Tap TTS to speak again", "", false, true);
    }
    return true;
}

static bool stream_tts_pcm_for_text(const char* text, const char* voice = nullptr, int sample_rate = 0, int volume = 0,
                                    int speech_rate = 0, int pitch_rate = 0)
{
    std::string url = make_stream_tts_url_for_text(text, voice, sample_rate, volume, speech_rate, pitch_rate);
    ESP_LOGI(TAG, "Stream TTS URL: %s", url.c_str());
    return stream_pcm_url(url, text, true);
}

static bool stream_cached_reply_pcm(const char* cache_name, const char* label)
{
    std::string url = make_event_audio_url(cache_name);
    ESP_LOGI(TAG, "Stream cached reply URL: %s", url.c_str());
    return stream_pcm_url(url, label, false);
}

static const char* cached_wake_reply_name_for_text(const char* text)
{
    if (text == nullptr) {
        return "";
    }
    if (strcmp(text, "我在") == 0 || strcmp(text, "我在。") == 0) {
        return "wake_reply";
    }
    if (strcmp(text, "有什么要帮忙的") == 0) {
        return "wake_reply_help";
    }
    if (strcmp(text, "你好呀") == 0) {
        return "wake_reply_hello";
    }
    if (strcmp(text, "我在呢") == 0) {
        return "wake_reply_here";
    }
    if (strcmp(text, "小派在呢") == 0) {
        return "wake_reply_xiaopai_here";
    }
    return "";
}

static bool stream_tts_pcm()
{
    return stream_tts_pcm_for_text(CONFIG_STACKCHAN_TTS_TEXT);
}

void run_stream_tts_demo()
{
    ensure_client_id();
    if (!ensure_wifi_connected()) {
        return;
    }
    if (!ensure_server_selected()) {
        return;
    }

    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    M5.Mic.end();

    if (!M5.Speaker.begin()) {
        set_tts_status("Speaker Fail", "M5.Speaker.begin failed", "", "", false, false);
        ESP_LOGE(TAG, "M5.Speaker.begin failed");
        return;
    }
    apply_speaker_volume();
    app2_stop_requested = false;
    start_mouth_animation();
    stream_tts_pcm();
    stop_mouth_animation();
}

static bool http_get_string(const std::string& url, std::string* response, int timeout_ms)
{
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = timeout_ms;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (response != nullptr) {
        char buffer[512];
        while (response->size() < 4096) {
            int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
            if (read_len <= 0) {
                break;
            }
            response->append(buffer, read_len);
            if (content_length > 0 && static_cast<int>(response->size()) >= content_length) {
                break;
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "HTTP GET %s -> status=%d content_length=%d response_len=%u", url.c_str(), status, content_length,
             response != nullptr ? static_cast<unsigned>(response->size()) : 0);
    return status >= 200 && status < 300;
}

static double json_number_value(const cJSON* root, const char* key, double default_value)
{
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(value) ? value->valuedouble : default_value;
}

static int json_int_value(const cJSON* root, const char* key, int default_value)
{
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(value)) {
        return static_cast<int>(value->valuedouble);
    }
    if (cJSON_IsString(value) && value->valuestring != nullptr && value->valuestring[0] != '\0') {
        return std::atoi(value->valuestring);
    }
    return default_value;
}

static bool json_bool_value(const cJSON* root, const char* key, bool default_value)
{
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(value)) {
        return cJSON_IsTrue(value);
    }
    if (cJSON_IsNumber(value)) {
        return value->valuedouble != 0;
    }
    if (cJSON_IsString(value)) {
        return strcmp(value->valuestring, "1") == 0 || strcmp(value->valuestring, "true") == 0 ||
               strcmp(value->valuestring, "yes") == 0 || strcmp(value->valuestring, "on") == 0;
    }
    return default_value;
}

static bool send_command_ack(const char* cmd_id, const char* status, const char* message = "")
{
    std::string url = make_server_url("/device/ack");
    url += "?device_id=";
    url += url_encode(mac_address().c_str());
    url += "&cmd_id=";
    url += url_encode(cmd_id != nullptr ? cmd_id : "");
    url += "&status=";
    url += url_encode(status != nullptr ? status : "received");
    if (message != nullptr && message[0] != '\0') {
        url += "&message=";
        url += url_encode(message);
    }
    std::string response;
    return http_get_string(url, &response, 5000);
}

static uint8_t light_bar_level_from_abs(int32_t level, int silence_level, int full_scale_level)
{
    if (level <= silence_level) {
        return 1;
    }
    int32_t scaled = (level - silence_level) * kLightStripSideLedCount /
                     std::max(1, full_scale_level - silence_level);
    return static_cast<uint8_t>(std::max<int32_t>(1, std::min<int32_t>(kLightStripSideLedCount, scaled + 1)));
}

static uint8_t listening_light_level_from_pcm(const int16_t* samples, size_t sample_count)
{
    if (samples == nullptr || sample_count == 0) {
        return 1;
    }
    int32_t level = average_abs_level(samples, sample_count);
    return light_bar_level_from_abs(level, kListeningLightSilenceLevel, kListeningLightFullScaleLevel);
}

static void update_listening_light_level(const int16_t* samples, size_t sample_count)
{
    uint32_t now = M5.millis();
    if (listening_light_last_update_ms != 0 &&
        static_cast<uint32_t>(now - listening_light_last_update_ms) < kListeningLightFrameMs) {
        return;
    }
    listening_light_last_update_ms = now;
    set_light_strip_listening_bar(listening_light_level_from_pcm(samples, sample_count));
}

static uint8_t speaking_light_level_from_pcm(const int16_t* samples, size_t sample_count)
{
    if (samples == nullptr || sample_count == 0) {
        return 1;
    }

    int32_t level = average_abs_level(samples, sample_count);
    return light_bar_level_from_abs(level, kSpeakingLightSilenceLevel, kSpeakingLightFullScaleLevel);
}

static void update_speaking_light_level(const int16_t* samples, size_t sample_count)
{
    if (!speaking_light_running) {
        return;
    }
    speaking_light_level = speaking_light_level_from_pcm(samples, sample_count);
    speaking_light_level_ms = M5.millis();
    update_mouth_from_speaking_level(speaking_light_level);
}

static void start_speaking_light_animation()
{
    speaking_light_level = 2;
    speaking_light_level_ms = 0;
    speaking_light_running = true;
    if (speaking_light_task_handle != nullptr) {
        return;
    }

    xTaskCreatePinnedToCore([](void*) {
        uint8_t displayed = 0;
        uint8_t fallback_phase = 0;
        while (speaking_light_running) {
            uint32_t now = M5.millis();
            uint8_t target = speaking_light_level;
            if (speaking_light_level_ms == 0 || static_cast<uint32_t>(now - speaking_light_level_ms) > 220) {
                static constexpr uint8_t fallback[] = {2, 3, 2, 4, 3, 2};
                target = fallback[fallback_phase % (sizeof(fallback) / sizeof(fallback[0]))];
                fallback_phase++;
            }

            if (displayed < target) {
                displayed++;
            } else if (displayed > target) {
                displayed--;
            }
            set_light_strip_speaking_bar(displayed);
            vTaskDelay(pdMS_TO_TICKS(kSpeakingLightFrameMs));
        }
        speaking_light_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "speak_light", 4 * 1024, nullptr, 2, &speaking_light_task_handle, 0);
}

static void stop_speaking_light_animation()
{
    speaking_light_running = false;
    for (int i = 0; i < 30 && speaking_light_task_handle != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    speaking_light_level = 0;
    speaking_light_level_ms = 0;
}

static void request_speak_preempt(const char* reason)
{
    app2_stop_requested = true;
    M5.Speaker.stop();
    cancel_mouth_animation_now();
    speaking_light_running = false;
    if (realtime_tts_playback_active) {
        finish_realtime_tts_playback();
    }
    ESP_LOGI(TAG, "Speak preempt requested: %s", reason != nullptr ? reason : "unspecified");
}

static bool execute_speak_command_internal(const char* text, bool pause_voice_listener, const char* cache_name,
                                           const char* voice, int sample_rate, int volume, int speech_rate,
                                           int pitch_rate)
{
    if (text == nullptr || text[0] == '\0') {
        return true;
    }

    local_voice_begin_speaking("speak command");
    bool should_pause_listener = pause_voice_listener || xiaozhi_task_handle != nullptr;
    VoiceListenerPauseGuard* voice_pause =
        should_pause_listener ? new VoiceListenerPauseGuard("speak command") : nullptr;

    if (audio_mutex != nullptr) {
        xSemaphoreTake(audio_mutex, portMAX_DELAY);
    }
    if (!M5.Speaker.begin()) {
        ESP_LOGE(TAG, "M5.Speaker.begin failed for command speak");
        if (audio_mutex != nullptr) {
            xSemaphoreGive(audio_mutex);
        }
        delete voice_pause;
        local_voice_end_speaking("speak begin failed");
        return false;
    }
    apply_speaker_volume();
    app2_stop_requested = false;
    M5.Speaker.stop();
    speech_playback_active = true;
    expression_set_speech_overridden(false);
    start_mouth_animation();
    const char* selected_cache_name = cache_name != nullptr && cache_name[0] != '\0' ? cache_name : "";
    bool ok = selected_cache_name[0] != '\0' ? stream_cached_reply_pcm(selected_cache_name, text)
                                             : stream_tts_pcm_for_text(text, voice, sample_rate, volume, speech_rate,
                                                                       pitch_rate);
    M5.Speaker.end();
    stop_mouth_animation();
    speech_playback_active = false;
    if (audio_mutex != nullptr) {
        xSemaphoreGive(audio_mutex);
    }
    delete voice_pause;
    local_voice_end_speaking("speak command finished");
    return ok;
}

static bool execute_speak_command(const char* text)
{
    if (voice_recording_active) {
        return enqueue_speak_command("", text, "", true);
    }
    return execute_speak_command_internal(text, true);
}

static void wait_for_voice_recording_to_finish()
{
    while (voice_recording_active && !app1_stop_requested) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static bool enqueue_speak_command(const char* cmd_id, const char* text, const char* cache_name, bool pause_voice_listener,
                                  const char* voice, int sample_rate, int volume, int speech_rate, int pitch_rate)
{
    if (speak_command_queue == nullptr || text == nullptr || text[0] == '\0') {
        return false;
    }

    SpeakCommandItem item = {};
    snprintf(item.cmd_id, sizeof(item.cmd_id), "%s", cmd_id != nullptr ? cmd_id : "");
    snprintf(item.text, sizeof(item.text), "%s", text);
    snprintf(item.cache_name, sizeof(item.cache_name), "%s", cache_name != nullptr ? cache_name : "");
    snprintf(item.voice, sizeof(item.voice), "%s", voice != nullptr ? voice : "");
    item.sample_rate = sample_rate;
    item.volume = volume;
    item.speech_rate = speech_rate;
    item.pitch_rate = pitch_rate;
    item.pause_voice_listener = pause_voice_listener;
    request_speak_preempt("new speak command");
    return xQueueOverwrite(speak_command_queue, &item) == pdPASS;
}

static void run_speak_command_loop()
{
    while (true) {
        if (voice_recording_active) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        SpeakCommandItem item = {};
        if (xQueueReceive(speak_command_queue, &item, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }

        wait_for_voice_recording_to_finish();
        app2_stop_requested = false;
        speak_command_active = true;
        bool ok = execute_speak_command_internal(item.text, item.pause_voice_listener, item.cache_name, item.voice,
                                                 item.sample_rate, item.volume, item.speech_rate, item.pitch_rate);
        speak_command_active = false;
        if (item.cmd_id[0] != '\0') {
            send_command_ack(item.cmd_id, ok ? "done" : "failed", ok ? "" : "speak command interrupted or failed");
        }
    }
}

static void start_speak_command_service()
{
    if (speak_command_queue == nullptr) {
        speak_command_queue = xQueueCreate(1, sizeof(SpeakCommandItem));
    }
    if (speak_command_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create speak command queue");
        return;
    }
    if (speak_command_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            run_speak_command_loop();
        }, "speak_cmd", kSpeakCommandTaskStackBytes, nullptr, 2, &speak_command_task_handle, 0);
    }
}

static bool execute_volume_command(const cJSON* payload)
{
    std::string mode = json_string_value(payload, "mode");
    std::string direction = json_string_value(payload, "direction");
    if (direction.empty()) {
        direction = json_string_value(payload, "action");
    }
    if (direction.empty()) {
        direction = json_string_value(payload, "type");
    }

    int step = static_cast<int>(json_number_value(payload, "step", kSpeakerVolumeDefaultStep));
    if (step <= 0) {
        step = kSpeakerVolumeDefaultStep;
    }

    const cJSON* value = cJSON_GetObjectItemCaseSensitive(payload, "value");
    if (mode == "set" || cJSON_IsNumber(value)) {
        speaker_volume_percent = static_cast<int>(json_number_value(payload, "value", speaker_volume_percent));
    } else if (direction == "down" || direction == "lower" || direction == "small" || direction == "quiet") {
        speaker_volume_percent -= step;
    } else {
        speaker_volume_percent += step;
    }
    speaker_volume_percent =
        std::max(kSpeakerVolumePercentMin, std::min(kSpeakerVolumePercentMax, speaker_volume_percent));

    char reply[64];
    snprintf(reply, sizeof(reply), "已经将声音调到%d", speaker_volume_percent);
    ESP_LOGI(TAG, "Speaker volume adjusted: percent=%d raw=%d", speaker_volume_percent,
             speaker_volume_raw_from_percent(speaker_volume_percent));
    if (!execute_speak_command(reply)) {
        ESP_LOGW(TAG, "Speaker volume reply failed after adjustment");
    }
    return true;
}
