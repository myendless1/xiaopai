struct PcmPlaybackStats {
    size_t queued_chunks = 0;
    size_t played_samples = 0;
};

static bool stream_pcm_url(const std::string& url, const char* label, bool update_tts_status,
                           const std::string* post_body = nullptr)
{
    ESP_LOGI(TAG, "Stream PCM URL: %s", url.c_str());
    if (update_tts_status) {
        set_tts_status("Requesting", url.c_str(), label != nullptr ? label : "");
    }

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = post_body != nullptr ? HTTP_METHOD_POST : HTTP_METHOD_GET;
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
    if (post_body != nullptr) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    esp_err_t err = esp_http_client_open(client, post_body != nullptr ? post_body->size() : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Stream PCM open failed: %s", esp_err_to_name(err));
        if (update_tts_status) {
            set_tts_status("HTTP Fail", esp_err_to_name(err), "Check PCM URL/IP", "", false, false);
        }
        esp_http_client_cleanup(client);
        return false;
    }
    if (post_body != nullptr) {
        int written = esp_http_client_write(client, post_body->c_str(), post_body->size());
        if (written != static_cast<int>(post_body->size())) {
            ESP_LOGE(TAG, "Stream PCM POST short write: written=%d expected=%u", written,
                     static_cast<unsigned>(post_body->size()));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
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

    char* header_format = nullptr;
    char* header_rate = nullptr;
    if (esp_http_client_get_header(client, "X-Audio-Format", &header_format) == ESP_OK && header_format != nullptr && header_format[0] != '\0') {
        if (strcasecmp(header_format, "pcm_s16le") != 0 && strcasecmp(header_format, "pcm") != 0 && strcasecmp(header_format, "wav") != 0) {
            ESP_LOGE(TAG, "Refusing playback: unsupported format '%s'", header_format);
            if (update_tts_status) {
                set_tts_status("Format Err", "Unsupported format", header_format, "", false, false);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
    }
    if (esp_http_client_get_header(client, "X-Sample-Rate", &header_rate) == ESP_OK && header_rate != nullptr && header_rate[0] != '\0') {
        char expected_rate[12];
        snprintf(expected_rate, sizeof(expected_rate), "%d", kTtsStreamSampleRate);
        if (strcmp(header_rate, expected_rate) != 0) {
            ESP_LOGE(TAG, "Refusing playback: unsupported sample rate '%s' (expected %s)",
                     header_rate, expected_rate);
            if (update_tts_status) {
                set_tts_status("Rate Err", "Expected 24000 Hz", header_rate, "", false, false);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
    }

    if (update_tts_status) {
        set_tts_status("Playing", "Receiving pcm_s16le", "24kHz mono stream", "");
    }

    PcmPlaybackStats playback;

    // One speech task owns this fixed pool while audio_mutex is held; avoid per-stream heap churn.
    static int16_t buffers[3][kTtsStreamBufferSamples] = {};
    int buffer_index = 0;
    size_t sample_pos = 0;
    uint8_t pending_byte = 0;
    bool has_pending_byte = false;
    size_t total_bytes = 0;
    int peak_sample = 0;
    uint32_t started_ms = M5.millis();
    uint8_t read_buffer[kHttpBufferSize];

    auto flush_samples = [&]() -> bool {
        if (sample_pos == 0) {
            return true;
        }
        for (size_t i = 0; i < sample_pos; ++i) {
            int sample = buffers[buffer_index][i];
            int abs_sample = sample < 0 ? -sample : sample;
            if (abs_sample > peak_sample) {
                peak_sample = abs_sample;
            }
        }
        update_speaking_light_level(buffers[buffer_index], sample_pos);
        bool ok = audio_service_play_pcm_24k(buffers[buffer_index], sample_pos,
                                             AudioPlayOptions{.wait = true, .drop_oldest = false});
        if (ok) {
            if (playback.queued_chunks == 0 && active_speak_turn_id[0] != '\0') {
                if (!reply_voice_audio_started(active_speak_turn_id, active_speak_generation)) {
                    ESP_LOGW(TAG, "Discarding PCM for inactive reply: turn=%s generation=%u",
                             active_speak_turn_id, static_cast<unsigned>(active_speak_generation));
                    return false;
                }
                expression_state_reply_audio_started(active_speak_turn_id, active_speak_generation);
            }
            ++playback.queued_chunks;
            playback.played_samples += sample_pos;
        }
        buffer_index = (buffer_index + 1) % 3;
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
            audio_service_abort_playback();
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
                audio_service_abort_playback();
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
                audio_service_abort_playback();
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
        audio_service_abort_playback();
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    bool playback_ok = !app2_stop_requested && audio_service_wait_playback_idle(pdMS_TO_TICKS(5000));
    if (playback_ok) {
        mark_speech_output_finished();
    }

    if (app2_stop_requested) {
        if (update_tts_status) {
            set_tts_status("Stopped", "TTS stopped", "", "", false, false);
        }
        return false;
    }
    if (!playback_ok) {
        if (update_tts_status) {
            set_tts_status("Play Fail", "PCM playback failed", "", "", false, false);
        }
        return false;
    }

    if (update_tts_status) {
        char line[64];
        snprintf(line, sizeof(line), "streamed %u bytes", static_cast<unsigned>(total_bytes));
        set_tts_status("Done", line, "Tap TTS to speak again", "", false, true);
    }
    ESP_LOGI(TAG, "PCM stream done: bytes=%u queued=%u samples=%u peak=%d elapsed_ms=%u",
             static_cast<unsigned>(total_bytes), static_cast<unsigned>(playback.queued_chunks),
             static_cast<unsigned>(playback.played_samples), peak_sample,
             static_cast<unsigned>(M5.millis() - started_ms));
    return true;
}

static bool stream_tts_pcm_for_text(const char* text, const char* voice = nullptr, int sample_rate = 0, int volume = 0,
                                    int speech_rate = 0, int pitch_rate = 0, const char* cmd_id = "")
{
    (void)sample_rate;
    std::string url = make_server_url("/v3/tts");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", mac_address().c_str());
    cJSON_AddStringToObject(root, "cmd_id", cmd_id != nullptr ? cmd_id : "");
    cJSON_AddStringToObject(root, "text", text != nullptr ? text : "");
    cJSON_AddStringToObject(root, "voice", voice != nullptr && voice[0] != '\0' ? voice : "default");
    cJSON_AddNumberToObject(root, "sample_rate", kTtsStreamSampleRate);
    if (volume != 0) cJSON_AddNumberToObject(root, "volume", volume);
    if (speech_rate != 0) cJSON_AddNumberToObject(root, "speech_rate", speech_rate);
    if (pitch_rate != 0) cJSON_AddNumberToObject(root, "pitch_rate", pitch_rate);
    char* encoded = cJSON_PrintUnformatted(root);
    std::string body = encoded != nullptr ? encoded : "{}";
    if (encoded != nullptr) cJSON_free(encoded);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Stream TTS POST: %s cmd_id=%s", url.c_str(), cmd_id != nullptr ? cmd_id : "");
    return stream_pcm_url(url, text, true, &body);
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

    if (!audio_service_start()) {
        set_tts_status("Audio Fail", "AudioService start failed", "", "", false, false);
        ESP_LOGE(TAG, "audio_service_start failed");
        return;
    }
    if (!supervisor_set_speech_playback_active(true, "stream tts demo")) {
        set_tts_status("Rejected", "Speech is gated by supervisor", "", "", false, false);
        return;
    }
    apply_speaker_volume();
    supervisor_set_app2_stop_requested(false, "stream tts demo");
    stream_tts_pcm();
    audio_service_wait_playback_idle(pdMS_TO_TICKS(1000));
    supervisor_set_speech_playback_active(false, "stream tts demo finished");
}

static bool http_get_string(const std::string& url, std::string* response, int timeout_ms, bool log_result)
{
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = timeout_ms;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        if (log_result) {
            ESP_LOGE(TAG, "HTTP GET init failed: %s", url.c_str());
        }
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        if (log_result) {
            ESP_LOGE(TAG, "HTTP GET open failed: url=%s err=%s", url.c_str(), esp_err_to_name(err));
        }
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
    const bool ok = status >= 200 && status < 300;
    if (log_result) {
        if (ok) {
            ESP_LOGI(TAG, "HTTP GET %s -> status=%d content_length=%d response_len=%u", url.c_str(), status,
                     content_length, response != nullptr ? static_cast<unsigned>(response->size()) : 0);
        } else {
            ESP_LOGW(TAG, "HTTP GET failed: url=%s status=%d content_length=%d", url.c_str(), status,
                     content_length);
        }
    }
    return ok;
}

static bool http_post_json_string(const std::string& url, const std::string& body, std::string* response,
                                  int timeout_ms, bool log_result = true)
{
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = timeout_ms;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        if (log_result) {
            ESP_LOGE(TAG, "HTTP POST init failed: %s", url.c_str());
        }
        return false;
    }

    std::string device_id = mac_address();
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-Device-Id", device_id.c_str());
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && response != nullptr) {
        char buffer[512];
        int read_len = 0;
        while (response->size() < 4096 &&
               (read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1)) > 0) {
            response->append(buffer, read_len);
        }
    }
    esp_http_client_cleanup(client);
    const bool ok = err == ESP_OK && status >= 200 && status < 300;
    if (log_result) {
        if (ok) {
            ESP_LOGI(TAG, "HTTP POST %s -> status=%d response_len=%u", url.c_str(), status,
                     response != nullptr ? static_cast<unsigned>(response->size()) : 0);
        } else {
            ESP_LOGW(TAG, "HTTP POST failed: url=%s err=%s status=%d", url.c_str(), esp_err_to_name(err), status);
        }
    }
    return ok;
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

static const CommandDedupeEntry* command_dedupe_find(const char* cmd_id)
{
    if (cmd_id == nullptr || cmd_id[0] == '\0') {
        return nullptr;
    }
    for (const auto& entry : command_dedupe_entries) {
        if (entry.cmd_id[0] != '\0' && strcmp(entry.cmd_id, cmd_id) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

static CommandDedupeEntry* command_dedupe_update(const char* cmd_id, const char* state, uint32_t attempt)
{
    if (cmd_id == nullptr || cmd_id[0] == '\0') {
        return nullptr;
    }
    CommandDedupeEntry* target = nullptr;
    bool existing = false;
    for (auto& entry : command_dedupe_entries) {
        if (entry.cmd_id[0] != '\0' && strcmp(entry.cmd_id, cmd_id) == 0) {
            target = &entry;
            existing = true;
            break;
        }
        if (target == nullptr && entry.cmd_id[0] == '\0') {
            target = &entry;
        }
    }
    if (target == nullptr) {
        target = &command_dedupe_entries[0];
        for (auto& entry : command_dedupe_entries) {
            if (entry.sequence < target->sequence) {
                target = &entry;
            }
        }
    }
    snprintf(target->cmd_id, sizeof(target->cmd_id), "%s", cmd_id);
    if (!existing) target->persist_across_boot = false;
    snprintf(target->state, sizeof(target->state), "%s", state != nullptr ? state : "received");
    target->attempt = attempt;
    target->sequence = ++command_dedupe_sequence;
    target->ack_pending = true;
    return target;
}

static void command_dedupe_mark_persistent(const char* cmd_id)
{
    for (auto& entry : command_dedupe_entries) {
        if (entry.cmd_id[0] != '\0' && strcmp(entry.cmd_id, cmd_id) == 0) {
            entry.persist_across_boot = true;
            return;
        }
    }
}

struct PersistentCommandDedupeLog {
    uint32_t version;
    uint32_t count;
    CommandDedupeEntry entries[32];
};

static bool command_state_is_terminal(const char* state)
{
    return state != nullptr &&
           (strcmp(state, "rendered") == 0 || strcmp(state, "done") == 0 ||
            strcmp(state, "failed") == 0 || strcmp(state, "cancelled") == 0 ||
            strcmp(state, "expired") == 0);
}

static void persist_terminal_command_dedupe_log()
{
    PersistentCommandDedupeLog log = {};
    log.version = 1;
    for (uint32_t sequence = command_dedupe_sequence; sequence > 0 && log.count < 32; --sequence) {
        for (const auto& entry : command_dedupe_entries) {
            if (entry.sequence == sequence && entry.persist_across_boot && command_state_is_terminal(entry.state)) {
                log.entries[log.count++] = entry;
                break;
            }
        }
    }
    nvs_handle_t handle;
    if (nvs_open("xiaopai", NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_set_blob(handle, "cmd_dedupe", &log, sizeof(log));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGW(TAG, "Persist command dedupe failed: %s", esp_err_to_name(err));
}

static void restore_terminal_command_dedupe_log()
{
    nvs_handle_t handle;
    if (nvs_open("xiaopai", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    PersistentCommandDedupeLog log = {};
    size_t size = sizeof(log);
    esp_err_t err = nvs_get_blob(handle, "cmd_dedupe", &log, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(log) || log.version != 1) {
        return;
    }
    uint32_t count = std::min<uint32_t>(log.count, 32);
    for (uint32_t i = 0; i < count; ++i) {
        command_dedupe_entries[i] = log.entries[i];
        command_dedupe_entries[i].ack_pending = false;
        command_dedupe_entries[i].sequence = ++command_dedupe_sequence;
    }
    ESP_LOGI(TAG, "Restored %u terminal command dedupe entries from NVS", count);
}

static bool send_command_ack(const char* cmd_id, const char* status, const char* message = "",
                             uint32_t attempt = 0, uint32_t started_at_tick = 0)
{
    if (xiaopai_boot_id == 0) {
        xiaopai_boot_id = esp_random();
    }
    const uint32_t ack_seq = ++command_ack_seq;
    const char* state = status != nullptr ? status : "received";
    CommandDedupeEntry* dedupe_entry = command_dedupe_update(cmd_id, state, attempt);
    if (dedupe_entry != nullptr && command_state_is_terminal(state)) {
        persist_terminal_command_dedupe_log();
    }
    std::string v3_url = make_server_url("/v3/command_ack");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "command_ack");
    cJSON_AddNumberToObject(root, "ack_seq", ack_seq);
    cJSON_AddStringToObject(root, "device_id", mac_address().c_str());
    cJSON_AddNumberToObject(root, "boot_id", xiaopai_boot_id);
    cJSON_AddStringToObject(root, "cmd_id", cmd_id != nullptr ? cmd_id : "");
    cJSON_AddNumberToObject(root, "attempt", attempt);
    cJSON_AddStringToObject(root, "state", state);
    cJSON_AddStringToObject(root, "effect", strcmp(state, "rendered") == 0 ? "speech_played" : "");
    cJSON_AddNumberToObject(root, "started_at_tick", started_at_tick);
    cJSON_AddNumberToObject(root, "finished_at_tick", static_cast<double>(esp_timer_get_time() / 1000));
    cJSON_AddStringToObject(root, "message", message != nullptr ? message : "");
    char* encoded = cJSON_PrintUnformatted(root);
    std::string body = encoded != nullptr ? encoded : "{}";
    if (encoded != nullptr) {
        cJSON_free(encoded);
    }
    cJSON_Delete(root);

    std::string v3_response;
    if (http_post_json_string(v3_url, body, &v3_response, 5000, false)) {
        ESP_LOGI(TAG, "COMMAND ack id=%s state=%s seq=%u attempt=%u",
                 cmd_id != nullptr ? cmd_id : "", state, static_cast<unsigned>(ack_seq),
                 static_cast<unsigned>(attempt));
        if (dedupe_entry != nullptr) {
            dedupe_entry->ack_pending = false;
        }
        return true;
    }

    ESP_LOGW(TAG, "COMMAND ack pending id=%s state=%s seq=%u attempt=%u",
             cmd_id != nullptr ? cmd_id : "", state, static_cast<unsigned>(ack_seq),
             static_cast<unsigned>(attempt));
    return false;
}

static void replay_pending_command_acks()
{
    for (auto& entry : command_dedupe_entries) {
        if (!entry.ack_pending || entry.cmd_id[0] == '\0') {
            continue;
        }
        ESP_LOGI(TAG, "Replaying command ACK: id=%s state=%s attempt=%u",
                 entry.cmd_id, entry.state, entry.attempt);
        send_command_ack(entry.cmd_id, entry.state, "ack replay", entry.attempt);
    }
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
    if (xiaopai_state_get().state == LocalVoiceState::DialogSleeping) {
        return;
    }
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

static uint32_t reply_voice_now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

static bool reply_voice_matches_locked(const char* turn_id, uint32_t generation)
{
    return reply_voice_session.phase != ReplyVoicePhase::None &&
           reply_voice_session.generation == generation &&
           strcmp(reply_voice_session.turn_id, turn_id != nullptr ? turn_id : "") == 0;
}

static bool reply_voice_is_active()
{
    if (reply_voice_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(reply_voice_mutex, portMAX_DELAY);
    const bool active = reply_voice_session.phase != ReplyVoicePhase::None;
    xSemaphoreGive(reply_voice_mutex);
    return active;
}

static void reply_voice_clear_locked()
{
    reply_voice_session = {};
}

static void report_reply_abort_to_server(const char* reason)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", mac_address().c_str());
    cJSON_AddStringToObject(root, "type", "local_stop");
    cJSON_AddStringToObject(root, "reason", reason != nullptr ? reason : "reply_cancelled");
    cJSON_AddNumberToObject(root, "generation", speech_command_generation);
    char* encoded = cJSON_PrintUnformatted(root);
    std::string body = encoded != nullptr ? encoded : "{}";
    if (encoded != nullptr) cJSON_free(encoded);
    cJSON_Delete(root);
    std::string response;
    if (!http_post_json_string(make_server_url("/device/event"), body, &response, 5000)) {
        ESP_LOGW(TAG, "Failed to report reply abort: reason=%s", reason != nullptr ? reason : "");
    }
}

static void reply_voice_restore_after_clear(bool speaking_hold_active, const char* reason)
{
    if (speaking_hold_active) {
        supervisor_set_speech_playback_active(false, reason);
    }
    const LocalVoiceState state = xiaopai_state_get().state;
    if (state == LocalVoiceState::Waiting || state == LocalVoiceState::Idle) {
        xiaopai_state_set(LocalVoiceState::Listening, reason);
    }
}

static void reply_voice_prepare(const char* turn_id, uint32_t generation)
{
    if (reply_voice_mutex == nullptr) {
        return;
    }
    const char* id = turn_id != nullptr ? turn_id : "";
    bool replaced_speaking_hold = false;
    bool new_session = false;
    xSemaphoreTake(reply_voice_mutex, portMAX_DELAY);
    if (!reply_voice_matches_locked(id, generation)) {
        replaced_speaking_hold = reply_voice_session.speaking_hold_active;
        reply_voice_clear_locked();
        snprintf(reply_voice_session.turn_id, sizeof(reply_voice_session.turn_id), "%s", id);
        reply_voice_session.generation = generation;
        new_session = true;
    }
    reply_voice_session.phase = ReplyVoicePhase::PendingAudio;
    reply_voice_session.last_progress_ms = reply_voice_now_ms();
    xSemaphoreGive(reply_voice_mutex);

    if (replaced_speaking_hold) {
        supervisor_set_speech_playback_active(false, "new reply replaced previous turn");
    }
    if (new_session && xiaopai_state_get().state == LocalVoiceState::Listening) {
        xiaopai_state_set(LocalVoiceState::Waiting, "reply pending audio");
    }
    if (new_session) {
        ESP_LOGI(TAG, "Reply voice pending: turn=%s generation=%u", id, static_cast<unsigned>(generation));
    }
}

static bool reply_voice_audio_started(const char* turn_id, uint32_t generation)
{
    if (reply_voice_mutex == nullptr) {
        return false;
    }
    bool begin_speaking = false;
    xSemaphoreTake(reply_voice_mutex, portMAX_DELAY);
    if (!reply_voice_matches_locked(turn_id, generation)) {
        xSemaphoreGive(reply_voice_mutex);
        return false;
    }
    begin_speaking = !reply_voice_session.speaking_hold_active;
    reply_voice_session.speaking_hold_active = true;
    reply_voice_session.phase = ReplyVoicePhase::Playing;
    reply_voice_session.last_progress_ms = reply_voice_now_ms();
    xSemaphoreGive(reply_voice_mutex);

    if (begin_speaking && !supervisor_set_speech_playback_active(true, "reply first pcm")) {
        xSemaphoreTake(reply_voice_mutex, portMAX_DELAY);
        if (reply_voice_matches_locked(turn_id, generation)) {
            reply_voice_session.speaking_hold_active = false;
        }
        xSemaphoreGive(reply_voice_mutex);
        return false;
    }
    return true;
}

static bool reply_voice_audio_has_started(const char* turn_id, uint32_t generation)
{
    if (reply_voice_mutex == nullptr) {
        return false;
    }
    xSemaphoreTake(reply_voice_mutex, portMAX_DELAY);
    const bool started = reply_voice_matches_locked(turn_id, generation) &&
                         reply_voice_session.speaking_hold_active;
    xSemaphoreGive(reply_voice_mutex);
    return started;
}

static void reply_voice_segment_finished(const char* turn_id, uint32_t generation)
{
    if (reply_voice_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(reply_voice_mutex, portMAX_DELAY);
    if (reply_voice_matches_locked(turn_id, generation) && reply_voice_session.speaking_hold_active) {
        reply_voice_session.phase = ReplyVoicePhase::BetweenSegments;
        reply_voice_session.last_progress_ms = reply_voice_now_ms();
    }
    xSemaphoreGive(reply_voice_mutex);
}

static void reply_voice_end(const char* turn_id, uint32_t generation, const char* reason)
{
    if (reply_voice_mutex == nullptr) {
        return;
    }
    bool matched = false;
    bool speaking_hold_active = false;
    xSemaphoreTake(reply_voice_mutex, portMAX_DELAY);
    if (reply_voice_matches_locked(turn_id, generation)) {
        matched = true;
        speaking_hold_active = reply_voice_session.speaking_hold_active;
        reply_voice_clear_locked();
    }
    xSemaphoreGive(reply_voice_mutex);
    if (matched) {
        reply_voice_restore_after_clear(speaking_hold_active, reason);
        ESP_LOGI(TAG, "Reply voice ended: turn=%s generation=%u reason=%s",
                 turn_id != nullptr ? turn_id : "", static_cast<unsigned>(generation),
                 reason != nullptr ? reason : "");
    }
}

static void reply_voice_cancel(const char* reason)
{
    if (reply_voice_mutex == nullptr) {
        return;
    }
    bool active = false;
    bool speaking_hold_active = false;
    xSemaphoreTake(reply_voice_mutex, portMAX_DELAY);
    active = reply_voice_session.phase != ReplyVoicePhase::None;
    speaking_hold_active = reply_voice_session.speaking_hold_active;
    reply_voice_clear_locked();
    xSemaphoreGive(reply_voice_mutex);
    if (active) {
        reply_voice_restore_after_clear(speaking_hold_active, reason);
        ESP_LOGI(TAG, "Reply voice cancelled: %s", reason != nullptr ? reason : "unspecified");
    }
}

static void run_reply_voice_watchdog()
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        bool expired = false;
        bool speaking_hold_active = false;
        char turn_id[40] = {};
        uint32_t generation = 0;
        if (reply_voice_mutex == nullptr) {
            continue;
        }
        xSemaphoreTake(reply_voice_mutex, portMAX_DELAY);
        const uint32_t now = reply_voice_now_ms();
        if ((reply_voice_session.phase == ReplyVoicePhase::PendingAudio ||
             reply_voice_session.phase == ReplyVoicePhase::BetweenSegments) &&
            static_cast<uint32_t>(now - reply_voice_session.last_progress_ms) >=
                kReplyContinuationTimeoutMs) {
            expired = true;
            speaking_hold_active = reply_voice_session.speaking_hold_active;
            snprintf(turn_id, sizeof(turn_id), "%s", reply_voice_session.turn_id);
            generation = reply_voice_session.generation;
            reply_voice_clear_locked();
        }
        xSemaphoreGive(reply_voice_mutex);
        if (!expired) {
            continue;
        }

        ESP_LOGE(TAG, "Reply voice timed out after %u ms: turn=%s generation=%u",
                 static_cast<unsigned>(kReplyContinuationTimeoutMs), turn_id,
                 static_cast<unsigned>(generation));
        supervisor_set_app2_stop_requested(true, "reply continuation timeout", SupervisorSafetyClass::LocalStop);
        audio_service_abort_playback();
        codec_audio_output_stop();
        reply_voice_restore_after_clear(speaking_hold_active, "reply continuation timeout");
        expression_state_reply_cancel("reply continuation timeout");
        advance_speech_generation(generation + 1);
        report_reply_abort_to_server("reply_continuation_timeout");
    }
}

static void request_speak_preempt(const char* reason)
{
    supervisor_set_app2_stop_requested(true, "speak preempt", SupervisorSafetyClass::LocalStop);
    stop_speaking_light_animation();
    if (realtime_tts_playback_active) {
        audio_service_abort_playback();
        finish_realtime_tts_playback();
    } else {
        codec_audio_output_stop();
    }
    reply_voice_cancel(reason);
    expression_state_reply_cancel(reason);
    ESP_LOGI(TAG, "Speak preempt requested: %s", reason != nullptr ? reason : "unspecified");
}

static void advance_speech_generation(uint32_t generation)
{
    speech_command_generation = generation > speech_command_generation
        ? generation
        : speech_command_generation + 1;
    if (speak_command_queue != nullptr) {
        xQueueReset(speak_command_queue);
    }
    reply_voice_cancel("speech generation advanced");
    expression_state_reply_cancel("speech generation advanced");
    ESP_LOGI(TAG, "Speech generation advanced: generation=%u", speech_command_generation);
}

static bool execute_speak_command_internal(const char* text, bool pause_voice_listener, const char* cache_name,
                                           const char* voice, int sample_rate, int volume, int speech_rate,
                                           int pitch_rate, const char* cmd_id, bool manage_voice_state)
{
    if (text == nullptr || text[0] == '\0') {
        return true;
    }
    if (demo_lock_enabled.load()) {
        ESP_LOGI(TAG, "Speak suppressed while demo lock is active");
        return false;
    }
    static constexpr const char* kTemporarilySuppressedFallbackSpeech =
        "\xE6\x88\x91\xE6\xB2\xA1\xE5\x90\xAC\xE6\xB8\x85\xEF\xBC\x8C\xE5\x8F\xAF\xE4\xBB\xA5"
        "\xE5\x86\x8D\xE8\xAF\xB4\xE4\xB8\x80\xE9\x81\x8D\xE5\x90\x97";
    if (strcmp(text, kTemporarilySuppressedFallbackSpeech) == 0) {
        ESP_LOGI(TAG, "Speak suppressed by temporary fallback silence guard");
        return true;
    }

    if (manage_voice_state && !supervisor_set_speech_playback_active(true, "speak command")) {
        return false;
    }
    JobHeader playback_job = xiaopai_make_job_header("", xiaopai_boot_id, 0);
    bool should_pause_listener = pause_voice_listener || realtime_task_active.load();
    VoiceListenerPauseGuard* voice_pause =
        should_pause_listener ? new VoiceListenerPauseGuard("speak command") : nullptr;

    if (audio_mutex != nullptr) {
        xSemaphoreTake(audio_mutex, portMAX_DELAY);
    }
    if (!codec_audio_output_begin(kTtsStreamSampleRate, speaker_volume_percent)) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed for command speak");
        if (audio_mutex != nullptr) {
            xSemaphoreGive(audio_mutex);
        }
        delete voice_pause;
        if (manage_voice_state) {
            supervisor_set_speech_playback_active(false, "speak begin failed");
        }
        return false;
    }
    apply_speaker_volume();
    supervisor_set_app2_stop_requested(false, "speak command");
    expression_state_start_speech_visual_feedback();
    const char* selected_cache_name = cache_name != nullptr && cache_name[0] != '\0' ? cache_name : "";
    bool ok = selected_cache_name[0] != '\0' ? stream_cached_reply_pcm(selected_cache_name, text)
                                             : stream_tts_pcm_for_text(text, voice, sample_rate, volume, speech_rate,
                                                                       pitch_rate, cmd_id);
    bool current_completion = manage_voice_state
        ? xiaopai_job_is_current(playback_job, xTaskGetTickCount())
        : active_speak_generation == speech_command_generation && !app2_stop_requested;
    if (!current_completion) {
        ESP_LOGI(TAG, "Discarding stale speak completion: generation or cancellation changed");
    }
    expression_state_stop_speech_visual_feedback();
    codec_audio_output_end();
    if (audio_mutex != nullptr) {
        xSemaphoreGive(audio_mutex);
    }
    delete voice_pause;
    if (manage_voice_state) {
        supervisor_set_speech_playback_active(false, "speak command finished");
    }
    return ok && current_completion;
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
                                  const char* voice, int sample_rate, int volume, int speech_rate, int pitch_rate,
                                  uint32_t attempt, uint32_t generation, uint16_t segment_index,
                                  uint32_t ttl_ms, const char* turn_id, const char* expression,
                                  bool reply_end, bool reply_cancelled, int speaker_volume)
{
    if (speak_command_queue == nullptr || text == nullptr || (text[0] == '\0' && !reply_end)) {
        return false;
    }
    if (demo_lock_enabled.load()) {
        ESP_LOGI(TAG, "Speak enqueue suppressed while demo lock is active");
        return false;
    }

    SpeakCommandItem item = {};
    const bool server_command = cmd_id != nullptr && cmd_id[0] != '\0';
    if (server_command && generation < speech_command_generation) {
        ESP_LOGW(TAG, "Rejecting stale speak generation=%u current=%u", generation, speech_command_generation);
        return false;
    }
    if (server_command && generation > speech_command_generation) {
        speech_command_generation = generation;
    }
    snprintf(item.cmd_id, sizeof(item.cmd_id), "%s", cmd_id != nullptr ? cmd_id : "");
    if (turn_id != nullptr && turn_id[0] != '\0') {
        snprintf(item.turn_id, sizeof(item.turn_id), "%s", turn_id);
    } else if (server_command) {
        snprintf(item.turn_id, sizeof(item.turn_id), "%s", cmd_id);
    } else {
        snprintf(item.turn_id, sizeof(item.turn_id), "local-%u", static_cast<unsigned>(M5.millis()));
    }
    snprintf(item.expression, sizeof(item.expression), "%s", expression != nullptr ? expression : "calm");
    item.attempt = attempt;
    item.generation = server_command ? generation : speech_command_generation;
    item.segment_index = segment_index;
    item.deadline_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ttl_ms);
    snprintf(item.text, sizeof(item.text), "%s", text);
    snprintf(item.cache_name, sizeof(item.cache_name), "%s", cache_name != nullptr ? cache_name : "");
    snprintf(item.voice, sizeof(item.voice), "%s", voice != nullptr ? voice : "");
    item.sample_rate = sample_rate;
    item.volume = volume;
    item.speaker_volume = speaker_volume;
    item.speech_rate = speech_rate;
    item.pitch_rate = pitch_rate;
    item.pause_voice_listener = pause_voice_listener;
    item.reply_end = reply_end;
    item.reply_cancelled = reply_cancelled;
    return xQueueSend(speak_command_queue, &item, 0) == pdPASS;
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
        if (demo_lock_enabled.load()) {
            send_command_ack(item.cmd_id, "cancelled", "demo lock active", item.attempt);
            continue;
        }
        if (static_cast<int32_t>(xTaskGetTickCount() - item.deadline_tick) >= 0) {
            send_command_ack(item.cmd_id, "expired", "speak deadline expired", item.attempt);
            continue;
        }
        supervisor_set_app2_stop_requested(false, "speak command loop");
        speak_command_active = true;
        uint32_t started_at_tick = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        send_command_ack(item.cmd_id, "running", "", item.attempt, started_at_tick);
        if (item.text[0] != '\0' && item.speaker_volume >= 0) {
            speaker_volume_percent = clamp_speaker_volume_percent(item.speaker_volume);
            apply_speaker_volume();
            ESP_LOGI(TAG, "Applied command speaker volume: percent=%d", speaker_volume_percent);
        }
        reply_voice_prepare(item.turn_id, item.generation);
        expression_state_reply_prepare(item.turn_id, item.generation, item.expression);
        bool ok = true;
        if (item.text[0] == '\0') {
            reply_voice_end(item.turn_id, item.generation,
                            item.reply_cancelled ? "reply cancelled control" : "reply end control");
            expression_state_reply_end(item.turn_id, item.generation, item.reply_cancelled);
        } else {
            snprintf(active_speak_turn_id, sizeof(active_speak_turn_id), "%s", item.turn_id);
            active_speak_generation = item.generation;
            ok = execute_speak_command_internal(item.text, item.pause_voice_listener, item.cache_name, item.voice,
                                                item.sample_rate, item.volume, item.speech_rate, item.pitch_rate,
                                                item.cmd_id, false);
            if (ok && !reply_voice_audio_has_started(item.turn_id, item.generation)) {
                ESP_LOGE(TAG, "Speak command produced no PCM: turn=%s segment=%u", item.turn_id,
                         static_cast<unsigned>(item.segment_index));
                ok = false;
            }
            active_speak_turn_id[0] = '\0';
            active_speak_generation = 0;
            if (ok) {
                reply_voice_segment_finished(item.turn_id, item.generation);
                expression_state_reply_segment_finished(item.turn_id, item.generation);
                if (item.reply_end) {
                    reply_voice_end(item.turn_id, item.generation, "reply ended on final segment");
                    expression_state_reply_end(item.turn_id, item.generation, false);
                }
            } else {
                const bool unexpected_failure = !app2_stop_requested;
                reply_voice_cancel("reply segment failed");
                expression_state_reply_failed(item.turn_id, item.generation);
                if (unexpected_failure) {
                    request_speak_preempt("reply segment failed");
                    advance_speech_generation(item.generation + 1);
                    report_reply_abort_to_server("reply_segment_failed");
                }
            }
        }
        speak_command_active = false;
        if (item.cmd_id[0] != '\0') {
            send_command_ack(item.cmd_id, ok ? "rendered" : "failed",
                             ok ? "" : "speak command interrupted or failed", item.attempt, started_at_tick);
        }
    }
}

static void start_speak_command_service()
{
    if (reply_voice_mutex == nullptr) {
        reply_voice_mutex = xSemaphoreCreateMutex();
    }
    if (reply_voice_mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create reply voice mutex");
        return;
    }
    if (reply_voice_watchdog_task_handle == nullptr) {
        BaseType_t watchdog_created = xTaskCreatePinnedToCore([](void*) {
            run_reply_voice_watchdog();
        }, "reply_voice", 4 * 1024, nullptr, 2, &reply_voice_watchdog_task_handle, 0);
        if (watchdog_created != pdPASS) {
            reply_voice_watchdog_task_handle = nullptr;
            ESP_LOGE(TAG, "Failed to create reply voice watchdog task");
            return;
        }
    }
    if (speak_command_queue == nullptr) {
        speak_command_queue = xQueueCreate(kSpeakCommandQueueCapacity, sizeof(SpeakCommandItem));
    }
    if (speak_command_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create speak command queue");
        return;
    }
    if (speak_command_task_handle == nullptr) {
        BaseType_t created = xTaskCreatePinnedToCore([](void*) {
            run_speak_command_loop();
        }, "speech_task", kSpeakCommandTaskStackBytes, nullptr, 5, &speak_command_task_handle, 1);
        if (created == pdPASS) {
            ESP_LOGI(TAG, "Speak command task created");
        } else {
            speak_command_task_handle = nullptr;
            ESP_LOGE(TAG, "Failed to create speak command task");
        }
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
    ESP_LOGI(TAG, "Speaker volume adjusted: percent=%d", speaker_volume_percent);
    if (!execute_speak_command(reply)) {
        ESP_LOGW(TAG, "Speaker volume reply failed after adjustment");
    }
    return true;
}
