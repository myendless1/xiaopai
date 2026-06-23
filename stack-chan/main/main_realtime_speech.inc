static bool configure_local_xiaozhi_websocket()
{
    std::string base = active_server_base;
    const bool tls = base.rfind("https://", 0) == 0;
    const char* prefix = tls ? "https://" : "http://";
    if (base.rfind(prefix, 0) == 0) {
        base = base.substr(strlen(prefix));
    }
    size_t slash = base.find('/');
    std::string host_port = slash == std::string::npos ? base : base.substr(0, slash);
    size_t colon = host_port.rfind(':');
    std::string host = colon == std::string::npos ? host_port : host_port.substr(0, colon);
    int port = colon == std::string::npos ? (tls ? 443 : 80) : atoi(host_port.substr(colon + 1).c_str());
    if (host.empty() || port <= 0) {
        return false;
    }
    int ws_port = port + 1;
    xiaozhi_config.websocket_url =
        std::string(tls ? "wss://" : "ws://") + host + ":" + std::to_string(ws_port) + "/xiaozhi/ws";
    xiaozhi_config.websocket_token = "";
    xiaozhi_config.session_id.clear();
    ESP_LOGI(TAG, "Local Xiaozhi WS configured: %s", xiaozhi_config.websocket_url.c_str());
    set_app1_status("Realtime", xiaozhi_config.websocket_url.c_str(), "Streaming ASR/TTS", "", false, true);
    return true;
}

static bool websocket_url_is_connectable(const std::string& url)
{
    ParsedUrl parsed;
    if (!parse_websocket_url(url, parsed)) {
        return false;
    }
    return parsed.host != "0.0.0.0" && parsed.host != "::" && parsed.host != "[::]";
}

static std::string make_system_info_json()
{
    const esp_app_desc_t* app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    char json[768];
    snprintf(json, sizeof(json),
             "{\"version\":2,\"language\":\"zh-CN\",\"flash_size\":%u,"
             "\"minimum_free_heap_size\":%u,\"mac_address\":\"%s\",\"uuid\":\"%s\","
             "\"chip_model_name\":\"%s\",\"chip_info\":{\"model\":%d,\"cores\":%d,\"revision\":%d,\"features\":%lu},"
             "\"application\":{\"name\":\"%s\",\"version\":\"%s\",\"idf_version\":\"%s\"},"
             "\"board\":{\"type\":\"xiaopai-m5unified-demo\",\"name\":\"Xiaopai Probe\"}}",
             0U, static_cast<unsigned>(esp_get_minimum_free_heap_size()), mac_address().c_str(), client_id,
             CONFIG_IDF_TARGET, chip.model, chip.cores, chip.revision, static_cast<unsigned long>(chip.features),
             app->project_name, app->version, app->idf_ver);
    return std::string(json);
}

static void summarize_ota_response(const std::string& response)
{
    ESP_LOGI(TAG, "OTA response body (%u bytes): %s", static_cast<unsigned>(response.size()), response.c_str());

    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        set_app1_status("Parse Fail", "Response is not JSON", "", "", false, false);
        ESP_LOGE(TAG, "Failed to parse OTA response JSON");
        return;
    }

    cJSON* activation = cJSON_GetObjectItem(root, "activation");
    cJSON* websocket = cJSON_GetObjectItem(root, "websocket");
    cJSON* mqtt = cJSON_GetObjectItem(root, "mqtt");

    if (cJSON_IsObject(websocket)) {
        cJSON* url = cJSON_GetObjectItem(websocket, "url");
        cJSON* token = cJSON_GetObjectItem(websocket, "token");
        xiaozhi_config.websocket_url = cJSON_IsString(url) ? url->valuestring : "";
        xiaozhi_config.websocket_token = cJSON_IsString(token) ? token->valuestring : "";
        ESP_LOGI(TAG, "WebSocket config found: url=%s token=%s", cJSON_IsString(url) ? url->valuestring : "(missing)",
                 cJSON_IsString(token) ? token->valuestring : "(missing)");
        set_app1_status("Got Token", cJSON_IsString(url) ? url->valuestring : "websocket config found",
                        cJSON_IsString(token) ? "WebSocket token present" : "WebSocket token missing",
                        "See USB serial log", false, cJSON_IsString(token));
    } else if (cJSON_IsObject(mqtt)) {
        cJSON* endpoint = cJSON_GetObjectItem(mqtt, "endpoint");
        ESP_LOGI(TAG, "MQTT config found: endpoint=%s", cJSON_IsString(endpoint) ? endpoint->valuestring : "(missing)");
        set_app1_status("Got MQTT", cJSON_IsString(endpoint) ? endpoint->valuestring : "mqtt config found",
                        "See USB serial log", "", false, true);
    } else if (cJSON_IsObject(activation)) {
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        ESP_LOGI(TAG, "Activation required: code=%s message=%s challenge=%s",
                 cJSON_IsString(code) ? code->valuestring : "(missing)",
                 cJSON_IsString(message) ? message->valuestring : "(missing)",
                 cJSON_IsString(challenge) ? challenge->valuestring : "(missing)");
        set_app1_status("Activation", cJSON_IsString(code) ? code->valuestring : "Activation required",
                        cJSON_IsString(message) ? message->valuestring : "Bind this device in console",
                        "See USB serial log", false, false);
    } else {
        set_app1_status("No Config", "No websocket/mqtt/activation", "See USB serial log", "", false, false);
        ESP_LOGW(TAG, "OTA response has no activation/websocket/mqtt object");
    }

    cJSON_Delete(root);
}

static bool request_xiaozhi_ota_config()
{
    set_app1_status("HTTP", "POST Xiaozhi OTA config");
    std::string body = make_system_info_json();
    ESP_LOGI(TAG, "OTA URL: %s", CONFIG_STACKCHAN_XIAOZHI_OTA_URL);
    ESP_LOGI(TAG, "Device-Id: %s", mac_address().c_str());
    ESP_LOGI(TAG, "Client-Id: %s", client_id);
    ESP_LOGI(TAG, "Request body: %s", body.c_str());

    esp_http_client_config_t config = {};
    config.url = CONFIG_STACKCHAN_XIAOZHI_OTA_URL;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_app1_status("HTTP Fail", "esp_http_client_init failed", "", "", false, false);
        return false;
    }

    std::string ua = std::string("xiaopai-m5unified/") + esp_app_get_description()->version;
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "Device-Id", mac_address().c_str());
    esp_http_client_set_header(client, "Client-Id", client_id);
    esp_http_client_set_header(client, "User-Agent", ua.c_str());
    esp_http_client_set_header(client, "Accept-Language", "zh-CN");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(client, body.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA HTTP request failed: %s", esp_err_to_name(err));
        set_app1_status("HTTP Fail", esp_err_to_name(err), "See USB serial log", "", false, false);
        esp_http_client_cleanup(client);
        return false;
    }

    int written = esp_http_client_write(client, body.c_str(), body.size());
    if (written < 0 || static_cast<size_t>(written) != body.size()) {
        ESP_LOGE(TAG, "HTTP write failed: written=%d expected=%u", written, static_cast<unsigned>(body.size()));
        set_app1_status("Write Fail", "HTTP request body failed", "", "", false, false);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGW(TAG, "HTTP content length unknown: %d", content_length);
    }

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "OTA HTTP status=%d content_length=%d", status, content_length);
    if (status != 200) {
        char line[64];
        snprintf(line, sizeof(line), "HTTP status %d", status);
        set_app1_status("HTTP Status", line, "Expected 200", "", false, false);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    std::string response;
    char buffer[512];
    while (true) {
        int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read failed");
            set_app1_status("Read Fail", "HTTP body read failed", "", "", false, false);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (read_len == 0) {
            break;
        }
        buffer[read_len] = '\0';
        response.append(buffer, read_len);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    summarize_ota_response(response);
    return !xiaozhi_config.websocket_url.empty() && !xiaozhi_config.websocket_token.empty();
}

static bool request_local_xiaozhi_ota_config()
{
    std::string url = make_server_url("/realtime/config");
    set_app1_status("Realtime", "GET realtime config", url.c_str(), "", true, false);
    ESP_LOGI(TAG, "Realtime config URL: %s", url.c_str());

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 5000;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        return false;
    }
    esp_http_client_set_header(client, "Device-Id", mac_address().c_str());
    esp_http_client_set_header(client, "Client-Id", client_id);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Realtime config open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (err != ESP_OK || status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Realtime config failed: err=%s status=%d", esp_err_to_name(err), status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    std::string response;
    if (content_length > 0) {
        response.resize(content_length);
        size_t offset = 0;
        while (offset < response.size()) {
            int read_len = esp_http_client_read(client, response.data() + offset, response.size() - offset);
            if (read_len <= 0) {
                break;
            }
            offset += read_len;
        }
        if (offset < response.size()) {
            response.resize(offset);
        }
    } else {
        char buffer[512];
        while (true) {
            int read_len = esp_http_client_read(client, buffer, sizeof(buffer) - 1);
            if (read_len <= 0) {
                break;
            }
            response.append(buffer, read_len);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (response.empty()) {
        return false;
    }
    summarize_ota_response(response);
    if (!websocket_url_is_connectable(xiaozhi_config.websocket_url)) {
        ESP_LOGW(TAG, "Realtime config returned unusable WS URL: %s", xiaozhi_config.websocket_url.c_str());
        xiaozhi_config.websocket_url.clear();
        xiaozhi_config.websocket_token.clear();
        return false;
    }
    return true;
}

static void* create_opus_encoder(int& frame_size_samples, int& outbuf_size)
{
    esp_opus_enc_config_t cfg = {};
    cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_16K;
    cfg.channel = ESP_AUDIO_MONO;
    cfg.bits_per_sample = ESP_AUDIO_BIT16;
    cfg.bitrate = ESP_OPUS_BITRATE_AUTO;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    cfg.complexity = 0;
    cfg.enable_fec = false;
    cfg.enable_dtx = true;
    cfg.enable_vbr = true;

    void* encoder = nullptr;
    esp_err_t ret = esp_opus_enc_open(&cfg, sizeof(cfg), &encoder);
    if (ret != ESP_AUDIO_ERR_OK || encoder == nullptr) {
        ESP_LOGE(TAG, "esp_opus_enc_open failed: %d", ret);
        return nullptr;
    }

    esp_opus_enc_get_frame_size(encoder, &frame_size_samples, &outbuf_size);
    frame_size_samples /= sizeof(int16_t);
    ESP_LOGI(TAG, "Opus encoder ready: frame_samples=%d outbuf=%d", frame_size_samples, outbuf_size);
    return encoder;
}

static esp_transport_handle_t open_xiaozhi_websocket(esp_transport_handle_t& parent)
{
    ParsedUrl parsed;
    if (!parse_websocket_url(xiaozhi_config.websocket_url, parsed)) {
        ESP_LOGE(TAG, "Invalid websocket URL: %s", xiaozhi_config.websocket_url.c_str());
        set_app1_status("WS Fail", "Invalid websocket URL", xiaozhi_config.websocket_url.c_str(), "", false, false);
        return nullptr;
    }

    parent = parsed.tls ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (parent == nullptr) {
        set_app1_status("WS Fail", "transport init failed", "", "", false, false);
        return nullptr;
    }
    if (parsed.tls) {
        esp_transport_ssl_crt_bundle_attach(parent, esp_crt_bundle_attach);
    }

    esp_transport_handle_t ws = esp_transport_ws_init(parent);
    if (ws == nullptr) {
        set_app1_status("WS Fail", "websocket init failed", "", "", false, false);
        esp_transport_destroy(parent);
        parent = nullptr;
        return nullptr;
    }

    std::string headers = std::string("Protocol-Version: 1\r\n") +
                          "Device-Id: " + mac_address() + "\r\n" +
                          "Client-Id: " + client_id + "\r\n";
    esp_transport_ws_set_path(ws, parsed.path.c_str());
    if (!xiaozhi_config.websocket_token.empty()) {
        std::string auth = "Bearer " + xiaozhi_config.websocket_token;
        esp_transport_ws_set_auth(ws, auth.c_str());
    }
    esp_transport_ws_set_headers(ws, headers.c_str());
    esp_transport_ws_set_user_agent(ws, "xiaopai-m5unified");

    set_app1_status("WS", "Connecting...", parsed.host.c_str(), parsed.path.c_str());
    ESP_LOGI(TAG, "Connecting WS host=%s port=%d path=%s tls=%d", parsed.host.c_str(), parsed.port, parsed.path.c_str(),
             parsed.tls);
    if (esp_transport_connect(ws, parsed.host.c_str(), parsed.port, 15000) != 0) {
        ESP_LOGE(TAG, "WS connect failed, status=%d", esp_transport_ws_get_upgrade_request_status(ws));
        set_app1_status("WS Fail", "connect/upgrade failed", "See USB serial log", "", false, false);
        esp_transport_destroy(ws);
        esp_transport_destroy(parent);
        parent = nullptr;
        return nullptr;
    }

    bool got_hello = false;
    std::string stt_text;
    std::string hello = make_ws_hello_message();
    ESP_LOGI(TAG, "WS send hello: %s", hello.c_str());
    if (!send_ws_text(ws, hello, "hello")) {
        set_app1_status("WS Fail", "hello send failed", "", "", false, false);
        esp_transport_destroy(ws);
        esp_transport_destroy(parent);
        parent = nullptr;
        return nullptr;
    }

    int64_t deadline = esp_timer_get_time() + 10000000LL;
    while (!got_hello && esp_timer_get_time() < deadline && !app1_stop_requested) {
        if (!receive_ws_once(ws, 1000, got_hello, stt_text)) {
            break;
        }
    }

    if (!got_hello) {
        set_app1_status("WS Fail", "No server hello", "See USB serial log", "", false, false);
        esp_transport_destroy(ws);
        esp_transport_destroy(parent);
        parent = nullptr;
        return nullptr;
    }

    set_app1_status("Ready", "Listening for voice", "Speak louder than threshold", "", false, true);
    return ws;
}

static bool send_opus_frame(esp_transport_handle_t ws, void* encoder, const int16_t* pcm, int samples, int outbuf_size)
{
    std::vector<uint8_t> outbuf(outbuf_size);
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t*>(const_cast<int16_t*>(pcm));
    in.len = samples * sizeof(int16_t);
    esp_audio_enc_out_frame_t out = {};
    out.buffer = outbuf.data();
    out.len = outbuf.size();
    out.encoded_bytes = 0;

    esp_err_t ret = esp_opus_enc_process(encoder, &in, &out);
    if (ret != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0) {
        ESP_LOGE(TAG, "Opus encode failed: %d encoded=%u", ret, static_cast<unsigned>(out.encoded_bytes));
        return false;
    }

    if (!send_ws_frame(ws, WS_TRANSPORT_OPCODES_BINARY, reinterpret_cast<const char*>(out.buffer), out.encoded_bytes,
                       "audio")) {
        return false;
    }
    ESP_LOGD(TAG, "Sent opus frame: pcm=%d encoded=%u", samples, static_cast<unsigned>(out.encoded_bytes));
    return true;
}

class VoiceRecordingGuard {
public:
    VoiceRecordingGuard()
    {
        voice_recording_active = true;
    }

    ~VoiceRecordingGuard()
    {
        voice_recording_active = false;
    }

    VoiceRecordingGuard(const VoiceRecordingGuard&) = delete;
    VoiceRecordingGuard& operator=(const VoiceRecordingGuard&) = delete;
};

static bool run_one_speech_recognition(esp_transport_handle_t ws, void* encoder, int encoder_frame_samples,
                                       int encoder_outbuf_size, const int16_t* first_samples, int first_count)
{
    std::string start = make_listen_message("start");
    ESP_LOGI(TAG, "WS send listen start: %s", start.c_str());
    if (!send_ws_text(ws, start, "listen start")) {
        set_app1_status("WS Fail", "listen start failed", "", "", false, false);
        return false;
    }

    VoiceRecordingGuard recording_guard;
    set_app1_status("Recording", "Voice threshold hit", "Sending Opus to Xiaozhi", "");
    update_listening_light_level(first_samples, first_count);
    std::vector<int16_t> frame(encoder_frame_samples);
    int frame_pos = 0;
    int elapsed_ms = 0;
    int silence_ms = 0;
    int32_t stop_smooth_level = average_abs_level(first_samples, first_count);
    bool got_hello = true;
    std::string stt_text;
    uint32_t state_generation = local_voice_generation();

    auto append_and_send = [&](const int16_t* samples, int count) -> bool {
        int offset = 0;
        while (offset < count && local_voice_generation() == state_generation && !local_voice_is_speaking()) {
            int n = std::min(count - offset, encoder_frame_samples - frame_pos);
            memcpy(frame.data() + frame_pos, samples + offset, n * sizeof(int16_t));
            frame_pos += n;
            offset += n;
            if (frame_pos == encoder_frame_samples) {
                if (!send_opus_frame(ws, encoder, frame.data(), encoder_frame_samples, encoder_outbuf_size)) {
                    return false;
                }
                frame_pos = 0;
                elapsed_ms += kOpusFrameDurationMs;
                if (!receive_ws_once(ws, 0, got_hello, stt_text)) {
                    return false;
                }
            }
        }
        return true;
    };

    if (!append_and_send(first_samples, first_count)) {
        return false;
    }

    std::vector<int16_t> chunk(kVoiceProbeSamples);
    while (!app1_stop_requested && !voice_listener_paused && local_voice_generation() == state_generation &&
           !local_voice_is_speaking() && elapsed_ms < kRecordMaxMs) {
        if (!mic_record_blocking(chunk.data(), chunk.size(), kAudioSampleRate)) {
            ESP_LOGW(TAG, "Mic record returned false while recording");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int32_t level = average_abs_level(chunk.data(), chunk.size());
        update_listening_light_level(chunk.data(), chunk.size());
        stop_smooth_level = (stop_smooth_level * 3 + level) / 4;
        silence_ms = stop_smooth_level < kVoiceStopThreshold ? silence_ms + (kVoiceProbeSamples * 1000 / kAudioSampleRate) : 0;
        if (!append_and_send(chunk.data(), chunk.size())) {
            return false;
        }

        if (silence_ms >= kSilenceStopMs && elapsed_ms > 600) {
            ESP_LOGI(TAG, "Stop recording due to silence: %d ms level=%ld smooth=%ld stop=%d",
                     silence_ms, static_cast<long>(level), static_cast<long>(stop_smooth_level), kVoiceStopThreshold);
            break;
        }
    }

    bool interrupted = voice_listener_paused || local_voice_generation() != state_generation || local_voice_is_speaking();

    if (!interrupted && frame_pos > 0) {
        memset(frame.data() + frame_pos, 0, (encoder_frame_samples - frame_pos) * sizeof(int16_t));
        if (!send_opus_frame(ws, encoder, frame.data(), encoder_frame_samples, encoder_outbuf_size)) {
            return false;
        }
    }

    std::string stop = make_listen_message("stop");
    ESP_LOGI(TAG, "WS send listen stop: %s", stop.c_str());
    send_ws_text(ws, stop, "listen stop");

    set_light_strip_listening();

    if (interrupted) {
        ESP_LOGI(TAG, "Recognition interrupted by local state: paused=%d state=%s gen=%u->%u",
                 voice_listener_paused, local_voice_state_name(local_voice_current_state()),
                 static_cast<unsigned>(state_generation), static_cast<unsigned>(local_voice_generation()));
        set_app1_status("Interrupted", "Local speech or command", "Listening resumes soon", "", true, false);
        return true;
    }

    set_app1_status("Recognizing", "Waiting for STT text", "See USB serial log", "");

    int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(kRealtimeSttDrainTimeoutMs) * 1000LL;
    while (stt_text.empty() && esp_timer_get_time() < deadline && !app1_stop_requested && !voice_listener_paused) {
        if (!receive_ws_once(ws, 1000, got_hello, stt_text)) {
            return false;
        }
    }

    if (stt_text.empty()) {
        set_app1_status("No STT", "No text returned yet", "Try speaking louder/longer", "", false, false);
        ESP_LOGW(TAG, "Recognition completed without STT text");
    } else {
        set_app1_status("Recognized", stt_text.c_str(), "Listening again...", "", false, true);
    }
    return true;
}

static void append_le16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
}

static void append_le32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(value & 0xff);
    out.push_back((value >> 8) & 0xff);
    out.push_back((value >> 16) & 0xff);
    out.push_back((value >> 24) & 0xff);
}

static std::vector<uint8_t> make_wav_bytes(const std::vector<int16_t>& pcm)
{
    const uint32_t data_bytes = pcm.size() * sizeof(int16_t);
    std::vector<uint8_t> wav;
    wav.reserve(44 + data_bytes);

    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    append_le32(wav, 36 + data_bytes);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    append_le32(wav, 16);
    append_le16(wav, 1);
    append_le16(wav, 1);
    append_le32(wav, kRecordSampleRate);
    append_le32(wav, kRecordSampleRate * sizeof(int16_t));
    append_le16(wav, sizeof(int16_t));
    append_le16(wav, 16);
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    append_le32(wav, data_bytes);

    const uint8_t* pcm_bytes = reinterpret_cast<const uint8_t*>(pcm.data());
    wav.insert(wav.end(), pcm_bytes, pcm_bytes + data_bytes);
    return wav;
}

static bool upload_wav_recording(const std::vector<int16_t>& pcm)
{
    std::vector<uint8_t> wav = make_wav_bytes(pcm);
    std::string upload_url = make_server_url("/upload-audio");
    ESP_LOGI(TAG, "Uploading WAV to %s, samples=%u bytes=%u", upload_url.c_str(),
             static_cast<unsigned>(pcm.size()), static_cast<unsigned>(wav.size()));
    ESP_LOGI(TAG, "APP1 stack high water=%u bytes, free heap=%u, min free heap=%u",
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()));
    set_app1_status("Uploading", upload_url.c_str(), "Sending WAV over HTTP", "");

    esp_http_client_config_t config = {};
    config.url = upload_url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kAudioUploadTimeoutMs;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_app1_status("Upload Fail", "esp_http_client_init failed", "", "", false, false);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "X-Device-Id", mac_address().c_str());
    esp_http_client_set_header(client, "X-Client-Id", client_id);

    esp_err_t err = esp_http_client_open(client, wav.size());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Upload open failed: %s", esp_err_to_name(err));
        set_app1_status("Upload Fail", esp_err_to_name(err), "Check server URL/IP", "", false, false);
        esp_http_client_cleanup(client);
        return false;
    }

    size_t offset = 0;
    while (offset < wav.size()) {
        size_t chunk = std::min<size_t>(kHttpBufferSize, wav.size() - offset);
        int written = esp_http_client_write(client, reinterpret_cast<const char*>(wav.data() + offset), chunk);
        if (written <= 0) {
            ESP_LOGE(TAG, "Upload write failed at offset=%u", static_cast<unsigned>(offset));
            set_app1_status("Upload Fail", "HTTP write failed", "See USB serial log", "", false, false);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        offset += written;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Upload HTTP status=%d content_length=%d", status, content_length);

    std::string response;
    char response_chunk[256];
    while (response.size() < 2048) {
        int read_len = esp_http_client_read(client, response_chunk, sizeof(response_chunk) - 1);
        if (read_len <= 0) {
            break;
        }
        response.append(response_chunk, read_len);
    }
    if (!response.empty()) {
        ESP_LOGI(TAG, "Upload response: %s", response.c_str());
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status >= 200 && status < 300) {
        std::string stt_text;
        if (!response.empty()) {
            cJSON* root = cJSON_Parse(response.c_str());
            if (root != nullptr) {
                std::string type = json_string_value(root, "type");
                if (type == "stt") {
                    stt_text = json_string_value(root, "text");
                } else if (type == "error") {
                    std::string message = json_string_value(root, "message");
                    set_app1_status("STT Error", message.empty() ? "server returned error" : message.c_str(),
                                    "See local server log", "", false, false);
                    cJSON_Delete(root);
                    return false;
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "Upload response is not JSON");
            }
        }

        if (!stt_text.empty()) {
            ESP_LOGI(TAG, "Background ASR text: %s", stt_text.c_str());
        } else {
            char line[96];
            snprintf(line, sizeof(line), "samples=%u wav=%u bytes", static_cast<unsigned>(pcm.size()),
                     static_cast<unsigned>(wav.size()));
            set_app1_status("No Speech", line, "Server returned empty text", "", false, true);
        }
        return true;
    }

    char line[48];
    snprintf(line, sizeof(line), "HTTP status %d", status);
    set_app1_status("Upload Fail", line, "See USB serial log", "", false, false);
    return false;
}

static void append_capped(std::vector<int16_t>& dst, const int16_t* samples, size_t count, size_t max_count)
{
    if (count >= max_count) {
        dst.assign(samples + count - max_count, samples + count);
        return;
    }
    if (dst.size() + count > max_count) {
        dst.erase(dst.begin(), dst.begin() + (dst.size() + count - max_count));
    }
    dst.insert(dst.end(), samples, samples + count);
}

static std::vector<int16_t> record_pcm_after_trigger(const std::vector<int16_t>& pre_roll, int32_t trigger_level)
{
    VoiceRecordingGuard recording_guard;
    const size_t max_samples = static_cast<size_t>(kRecordSampleRate) * kRecordMaxMs / 1000;
    std::vector<int16_t> pcm;
    pcm.reserve(max_samples + pre_roll.size() + kVoiceProbeSamples);
    pcm.insert(pcm.end(), pre_roll.begin(), pre_roll.end());

    std::vector<int16_t> chunk(kVoiceProbeSamples);
    int elapsed_ms = pre_roll.size() * 1000 / kRecordSampleRate;
    int silence_ms = 0;
    uint32_t last_draw_ms = 0;
    int32_t smooth_level = trigger_level;

    set_app1_status("Recording", "Voice threshold hit", "Pre-roll captured", "");
    while (!app1_stop_requested && !voice_listener_paused && elapsed_ms < kRecordMaxMs && pcm.size() < max_samples) {
        if (!mic_record_blocking(chunk.data(), chunk.size(), kRecordSampleRate)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int32_t level = average_abs_level(chunk.data(), chunk.size());
        update_listening_light_level(chunk.data(), chunk.size());
        smooth_level = (smooth_level * 3 + level) / 4;
        silence_ms = smooth_level < kVoiceStopThreshold ? silence_ms + kRecordChunkMs : 0;
        pcm.insert(pcm.end(), chunk.begin(), chunk.end());
        elapsed_ms += kRecordChunkMs;

        uint32_t now = M5.millis();
        if (now - last_draw_ms > 900) {
            char line1[64];
            char line2[64];
            snprintf(line1, sizeof(line1), "level=%ld smooth=%ld", static_cast<long>(level), static_cast<long>(smooth_level));
            snprintf(line2, sizeof(line2), "time=%dms silence=%dms", elapsed_ms, silence_ms);
            set_app1_status("Recording", line1, line2, "");
            last_draw_ms = now;
        }

        if (silence_ms >= kSilenceStopMs && elapsed_ms > kPreRollMs + 600) {
            ESP_LOGI(TAG, "Stop recording due to silence: elapsed=%d silence=%d", elapsed_ms, silence_ms);
            break;
        }
    }

    ESP_LOGI(TAG, "Recorded PCM samples=%u duration=%ums", static_cast<unsigned>(pcm.size()),
             static_cast<unsigned>(pcm.size() * 1000 / kRecordSampleRate));
    set_light_strip_listening();
    return pcm;
}

static void run_local_record_upload_loop()
{
    ensure_client_id();
    while (!wifi_is_connected() || !active_server_selected) {
        set_app1_status("Waiting", "Network is starting", "Listening starts soon", "", true, false);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    codec_audio_output_end();
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = kRecordSampleRate;
    mic_cfg.magnification = CONFIG_STACKCHAN_MIC_MAGNIFICATION;
    mic_cfg.noise_filter_level = 0;
    mic_cfg.task_pinned_core = 1;
    M5.Mic.config(mic_cfg);
    if (!M5.Mic.isEnabled() && !M5.Mic.begin()) {
        set_app1_status("Mic Fail", "M5.Mic.begin failed", "", "", false, false);
        ESP_LOGE(TAG, "M5.Mic.begin failed");
        return;
    }

    set_app1_status("Ready", "Listening", make_server_url("/upload-audio").c_str(), "", false, true);
    set_light_strip_listening();
    std::vector<int16_t> probe(kVoiceProbeSamples);
    std::vector<int16_t> pre_roll;
    pre_roll.reserve(kPreRollSamples + kVoiceProbeSamples);
    uint32_t last_status_ms = 0;
    int32_t smooth_level = 0;

    while (!app1_stop_requested) {
        if (voice_listener_paused) {
            if (M5.Mic.isEnabled()) {
                while (M5.Mic.isRecording()) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                M5.Mic.end();
            }
            set_app1_status("Paused", "Executing command", "Listening resumes soon", "", true, false);
            while (voice_listener_paused && !app1_stop_requested) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (app1_stop_requested) {
                break;
            }
            M5.Mic.config(mic_cfg);
            if (!M5.Mic.isEnabled() && !M5.Mic.begin()) {
                set_app1_status("Mic Fail", "M5.Mic.begin failed", "", "", false, false);
                ESP_LOGE(TAG, "M5.Mic.begin failed after pause");
                break;
            }
            pre_roll.clear();
            smooth_level = 0;
            last_status_ms = 0;
            set_app1_status("Listening", "Resumed", "Speak to upload", "", true, false);
            set_light_strip_listening();
        }
        if (!mic_record_blocking(probe.data(), probe.size(), kRecordSampleRate)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int32_t level = average_abs_level(probe.data(), probe.size());
        smooth_level = smooth_level == 0 ? level : (smooth_level * 3 + level) / 4;
        append_capped(pre_roll, probe.data(), probe.size(), kPreRollSamples);
        uint32_t now = M5.millis();
        if (now - last_status_ms > 700) {
            char line[64];
            snprintf(line, sizeof(line), "level=%ld smooth=%ld start=%d", static_cast<long>(level),
                     static_cast<long>(smooth_level), kVoiceStartThreshold);
            set_app1_status("Listening", line, "Speak to record/upload", "", true, false);
            last_status_ms = now;
        }

        if (post_speech_echo_guard_active()) {
            if (smooth_level >= kVoiceStartThreshold) {
                ESP_LOGI(TAG, "Voice threshold ignored during post-speech echo guard: level=%ld smooth=%ld",
                         static_cast<long>(level), static_cast<long>(smooth_level));
            }
            pre_roll.clear();
            smooth_level = 0;
            continue;
        }

        if (!voice_listener_paused && smooth_level >= kVoiceStartThreshold) {
            ESP_LOGI(TAG, "Voice threshold triggered: level=%ld smooth=%ld start=%d stop=%d pre_roll=%ums sample_rate=%d",
                     static_cast<long>(level), static_cast<long>(smooth_level), kVoiceStartThreshold,
                     kVoiceStopThreshold, kPreRollMs, kRecordSampleRate);
            mark_user_interaction("voice activity");
            request_speak_preempt("voice activity");
            auto pcm = record_pcm_after_trigger(pre_roll, smooth_level);
            if (!pcm.empty() && !app1_stop_requested && !voice_listener_paused) {
                upload_wav_recording(pcm);
            }
            pre_roll.clear();
            smooth_level = 0;
            last_status_ms = 0;
        }
    }

    set_app1_status("Stopped", "Voice stopped", "", "", false, false);
    set_light_strip_sleeping();
    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    M5.Mic.end();
}

static void pause_voice_listener_for_shared_peripherals(const char* reason)
{
    voice_listener_pause_count = voice_listener_pause_count + 1;
    voice_listener_paused = true;
    ESP_LOGI(TAG, "Voice listener pause requested: %s depth=%d",
             reason != nullptr ? reason : "shared peripheral use", voice_listener_pause_count);

    if (xTaskGetCurrentTaskHandle() == xiaozhi_task_handle) {
        while (M5.Mic.isRecording()) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        if (M5.Mic.isEnabled()) {
            M5.Mic.end();
        }
        return;
    }

    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (M5.Mic.isEnabled()) {
        M5.Mic.end();
    }
}

static void resume_voice_listener_after_shared_peripherals()
{
    if (voice_listener_pause_count > 0) {
        voice_listener_pause_count = voice_listener_pause_count - 1;
    }
    voice_listener_paused = voice_listener_pause_count > 0;
    ESP_LOGI(TAG, "Voice listener pause released depth=%d", voice_listener_pause_count);
}

class VoiceListenerPauseGuard {
public:
    explicit VoiceListenerPauseGuard(const char* reason)
    {
        pause_voice_listener_for_shared_peripherals(reason);
    }

    ~VoiceListenerPauseGuard()
    {
        resume_voice_listener_after_shared_peripherals();
    }

    VoiceListenerPauseGuard(const VoiceListenerPauseGuard&) = delete;
    VoiceListenerPauseGuard& operator=(const VoiceListenerPauseGuard&) = delete;
};

static void run_xiaozhi_speech_loop()
{
    if (xiaozhi_config.websocket_url.empty()) {
        set_app1_status("No WS", "OTA did not return websocket config", "Cannot recognize speech", "", false, false);
        return;
    }

    codec_audio_output_end();
    auto mic_cfg = M5.Mic.config();
    mic_cfg.sample_rate = kAudioSampleRate;
    mic_cfg.magnification = CONFIG_STACKCHAN_MIC_MAGNIFICATION;
    mic_cfg.noise_filter_level = 0;
    mic_cfg.task_pinned_core = 1;
    M5.Mic.config(mic_cfg);
    if (!M5.Mic.isEnabled() && !M5.Mic.begin()) {
        set_app1_status("Mic Fail", "M5.Mic.begin failed", "", "", false, false);
        ESP_LOGE(TAG, "M5.Mic.begin failed");
        return;
    }

    int encoder_frame_samples = 0;
    int encoder_outbuf_size = 0;
    void* encoder = create_opus_encoder(encoder_frame_samples, encoder_outbuf_size);
    if (encoder == nullptr || encoder_frame_samples != kOpusFrameSamples) {
        set_app1_status("Opus Fail", "Could not start encoder", "See USB serial log", "", false, false);
        if (encoder != nullptr) {
            esp_opus_enc_close(encoder);
        }
        return;
    }

    esp_transport_handle_t parent = nullptr;
    esp_transport_handle_t ws = nullptr;

    auto close_ws = [&]() {
        if (realtime_tts_playback_active) {
            request_speak_preempt("websocket reconnect");
            app2_stop_requested = false;
        }
        if (ws != nullptr) {
            esp_transport_close(ws);
            esp_transport_destroy(ws);
            ws = nullptr;
        }
        if (parent != nullptr) {
            esp_transport_destroy(parent);
            parent = nullptr;
        }
    };

    auto reconnect_ws = [&](const char* reason) -> bool {
        ESP_LOGW(TAG, "Realtime WebSocket reconnect requested: %s", reason != nullptr ? reason : "");
        close_ws();
        local_voice_request_state(LocalVoiceState::Waiting, reason != nullptr ? reason : "websocket reconnect");
        set_app1_status("WS Reconnect", "Waiting for server", "Voice resumes automatically", "", true, false);

        int retry_delay_ms = 1000;
        while (!app1_stop_requested) {
            while ((!wifi_is_connected() || !active_server_selected) && !app1_stop_requested) {
                set_app1_status("WS Reconnect", "Waiting for network", "Voice resumes automatically", "", true, false);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            if (app1_stop_requested) {
                break;
            }

            if (!request_local_xiaozhi_ota_config()) {
                configure_local_xiaozhi_websocket();
            }
            ws = open_xiaozhi_websocket(parent);
            if (ws != nullptr) {
                local_voice_request_state(LocalVoiceState::Listening, "websocket connected");
                set_app1_status("Listening", "WebSocket connected", "Ready for voice", "", true, false);
                set_light_strip_listening();
                return true;
            }

            set_app1_status("WS Reconnect", "Connect failed", "Retrying...", "", true, false);
            vTaskDelay(pdMS_TO_TICKS(retry_delay_ms));
            retry_delay_ms = std::min(retry_delay_ms * 2, 5000);
        }
        return false;
    };

    if (!reconnect_ws("speech loop start")) {
        close_ws();
        esp_opus_enc_close(encoder);
        local_voice_request_state(LocalVoiceState::Idle, "websocket open failed");
        return;
    }

    local_voice_request_state(LocalVoiceState::Listening, "speech loop start");
    set_app1_status("Listening", "Ready for voice", "Speak to recognize", "", true, false);
    set_light_strip_listening();
    std::vector<int16_t> probe(kVoiceProbeSamples);
    uint32_t last_status_ms = 0;
    bool got_hello = true;
    std::string stt_text;
    while (!app1_stop_requested) {
        handle_pending_wake_find_owner();

        if (voice_listener_paused || !local_voice_can_sample_mic()) {
            while (M5.Mic.isRecording()) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            if (M5.Mic.isEnabled()) {
                M5.Mic.end();
            }
            LocalVoiceState paused_state = local_voice_current_state();
            if (paused_state == LocalVoiceState::Speaking) {
                set_app1_status("Speaking", "Local speech output", "Mic is gated", "", true, false);
            } else if (paused_state == LocalVoiceState::Waiting) {
                set_app1_status("Waiting", "OpenClaw is thinking", "Mic is gated", "", true, false);
            } else {
                set_app1_status("Paused", "Shared peripheral", "Listening resumes soon", "", true, false);
            }
            while ((voice_listener_paused || !local_voice_can_sample_mic()) && !app1_stop_requested) {
                if (!receive_ws_once(ws, 50, got_hello, stt_text) &&
                    !reconnect_ws("websocket read failed while paused")) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            if (app1_stop_requested) {
                break;
            }
            if (!local_voice_can_sample_mic()) {
                continue;
            }
            M5.Mic.config(mic_cfg);
            if (!M5.Mic.begin()) {
                set_app1_status("Mic Fail", "M5.Mic.begin failed after pause", "", "", false, false);
                ESP_LOGE(TAG, "M5.Mic.begin failed after realtime pause");
                break;
            }
            last_status_ms = 0;
            stt_text.clear();
            local_voice_apply_outputs(local_voice_current_state());
            if (local_voice_current_state() == LocalVoiceState::Idle) {
                set_app1_status("Wake Listen", "Resumed", "Say 小派同学", "", true, false);
                set_light_strip_listening();
            } else {
                set_app1_status("Listening", "Resumed", "Speak to recognize", "", true, false);
            }
            continue;
        }
        if (realtime_tts_playback_active) {
            if (!receive_ws_once(ws, 100, got_hello, stt_text) &&
                !reconnect_ws("websocket read failed during tts")) {
                break;
            }
            continue;
        }
        if (!M5.Mic.isEnabled() && !realtime_tts_playback_active) {
            M5.Mic.config(mic_cfg);
            if (!M5.Mic.begin()) {
                set_app1_status("Mic Fail", "M5.Mic.begin failed", "", "", false, false);
                ESP_LOGE(TAG, "M5.Mic.begin failed while resuming realtime listener");
                break;
            }
        }
        if (!mic_record_blocking(probe.data(), probe.size(), kAudioSampleRate)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        int32_t level = average_abs_level(probe.data(), probe.size());
        update_listening_light_level(probe.data(), probe.size());
        uint32_t now = M5.millis();
        if (now - last_status_ms > 700) {
            char level_line[64];
            snprintf(level_line, sizeof(level_line), "level=%ld threshold=%d", static_cast<long>(level), kVoiceStartThreshold);
            if (local_voice_current_state() == LocalVoiceState::Idle) {
                set_app1_status("Wake Listen", level_line, "Say 小派同学", "", true, false);
            } else {
                set_app1_status("Listening", level_line, "Speak to recognize", "", true, false);
            }
            last_status_ms = now;
        }

        if (!receive_ws_once(ws, 0, got_hello, stt_text) &&
            !reconnect_ws("websocket read failed")) {
            break;
        }
        if (post_speech_echo_guard_active()) {
            if (level >= kVoiceStartThreshold) {
                ESP_LOGI(TAG, "Voice threshold ignored during post-speech echo guard: level=%ld threshold=%d",
                         static_cast<long>(level), kVoiceStartThreshold);
            }
            continue;
        }
        if (local_voice_can_sample_mic() && level >= kVoiceStartThreshold) {
            ESP_LOGI(TAG, "Voice threshold triggered: level=%ld threshold=%d state=%s",
                     static_cast<long>(level), kVoiceStartThreshold, local_voice_state_name(local_voice_current_state()));
            mark_user_interaction("voice activity");
            if (!run_one_speech_recognition(ws, encoder, encoder_frame_samples, encoder_outbuf_size, probe.data(),
                                            probe.size())) {
                ESP_LOGW(TAG, "Speech recognition round failed; reconnecting WebSocket");
                if (!reconnect_ws("speech recognition round failed")) {
                    local_voice_request_state(LocalVoiceState::Idle, "websocket reconnect failed");
                    break;
                }
                set_app1_status("Listening", "Reconnected", "Speak to recognize", "", true, false);
                set_light_strip_listening();
            }
            last_status_ms = 0;
        }
    }

    set_app1_status("Stopped", "Voice stopped", "", "", false, false);
    local_voice_request_state(LocalVoiceState::Idle, "speech loop stopped");
    close_ws();
    esp_opus_enc_close(encoder);
    while (M5.Mic.isRecording()) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    M5.Mic.end();
}

void run_xiaozhi_ota_probe()
{
    ESP_LOGI(TAG, "Starting realtime Xiaozhi speech loop over WebSocket");
    set_app1_status("Realtime", "Preparing streaming speech", "Waiting for local server", "", true, false);
    ensure_client_id();
    if (!ensure_network_ready()) {
        return;
    }
    if (!request_local_xiaozhi_ota_config() && !configure_local_xiaozhi_websocket()) {
        set_app1_status("No WS", "Could not build local WebSocket URL", active_server_base.c_str(), "", false, false);
        return;
    }
    run_xiaozhi_speech_loop();
}
