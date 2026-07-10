static vprintf_like_t previous_log_vprintf = nullptr;

static int wifi_debug_vprintf(const char* format, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    char line[240];
    int written = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);

    if (wifi_debug_log_enabled && !wifi_debug_internal_http && written > 0) {
        debug_events_push_log_line(line);
    }

    if (wifi_debug_usb_serial_enabled) {
        if (previous_log_vprintf != nullptr) {
            return previous_log_vprintf(format, args);
        }
        return vprintf(format, args);
    }
    return written > 0 ? written : 0;
}

static void install_wifi_debug_log_sink()
{
    debug_events_init();
    previous_log_vprintf = esp_log_set_vprintf(wifi_debug_vprintf);
}

static void wifi_debug_append_bool(std::string& json, const char* key, bool value)
{
    json += ",\"";
    json += key;
    json += "\":";
    json += value ? "true" : "false";
}

static bool wifi_debug_event_to_json(const DebugEventItem& item, std::string* out)
{
    if (out == nullptr) {
        return false;
    }
    if (item.kind == DebugEventKind::LogLine) {
        if (!wifi_debug_log_enabled) {
            return false;
        }
        std::string json = "{\"type\":\"log\",\"source\":\"";
        json += json_escape(item.source);
        json += "\",\"device_ms\":";
        json += std::to_string(item.device_ms);
        json += ",\"line\":\"";
        json += json_escape(item.line);
        json += "\"}";
        *out = json;
        return true;
    }
    if (item.kind == DebugEventKind::StateChange) {
        if (!wifi_debug_state_events_enabled) {
            return false;
        }
        std::string json = "{\"type\":\"state_change\",\"source\":\"";
        json += json_escape(item.source);
        json += "\",\"state_machine\":\"";
        json += json_escape(item.source);
        json += "\",\"device_ms\":";
        json += std::to_string(item.device_ms);
        json += ",\"from\":\"";
        json += json_escape(item.from);
        json += "\",\"to\":\"";
        json += json_escape(item.to);
        json += "\",\"reason\":\"";
        json += json_escape(item.reason);
        json += "\"";
        if (strcmp(item.source, "xiaopai-state") == 0) {
            json += ",\"generation\":";
            json += std::to_string(item.generation);
            wifi_debug_append_bool(json, "is_speaking", item.is_speaking);
            wifi_debug_append_bool(json, "can_sample_mic", item.can_sample_mic);
        } else if (strcmp(item.source, "expression-state") == 0) {
            wifi_debug_append_bool(json, "screen_visible", item.screen_visible);
            wifi_debug_append_bool(json, "sleep_dark", item.sleep_dark);
            wifi_debug_append_bool(json, "animation_active", item.animation_active);
        }
        json += "}";
        *out = json;
        return true;
    }
    return false;
}

static bool wifi_debug_post_json(const char* path, const std::string& body, int timeout_ms)
{
    std::string url = make_server_url(path);
    url += "?device_id=";
    url += url_encode(mac_address().c_str());

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = timeout_ms;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    wifi_debug_internal_http = true;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        wifi_debug_internal_http = false;
        return false;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-Device-Id", mac_address().c_str());

    esp_err_t err = esp_http_client_open(client, static_cast<int>(body.size()));
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        wifi_debug_internal_http = false;
        return false;
    }
    int written = esp_http_client_write(client, body.data(), static_cast<int>(body.size()));
    if (written < 0 || static_cast<size_t>(written) != body.size()) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        wifi_debug_internal_http = false;
        return false;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    wifi_debug_internal_http = false;
    return status >= 200 && status < 300;
}

static void wifi_debug_apply_config_response(const std::string& response)
{
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        return;
    }
    bool old_wifi_log = wifi_debug_log_enabled;
    bool old_usb_serial = wifi_debug_usb_serial_enabled;
    bool old_state_events = wifi_debug_state_events_enabled;
    wifi_debug_log_enabled = json_bool_value(root, "wifi_log", wifi_debug_log_enabled);
    wifi_debug_usb_serial_enabled = json_bool_value(root, "usb_serial", wifi_debug_usb_serial_enabled);
    wifi_debug_state_events_enabled = json_bool_value(root, "state_events", wifi_debug_state_events_enabled);
    wifi_debug_config_poll_ms = std::max(1000, json_int_value(root, "config_poll_ms", wifi_debug_config_poll_ms));
    wifi_debug_post_interval_ms = std::max(250, json_int_value(root, "log_post_interval_ms", wifi_debug_post_interval_ms));
    cJSON_Delete(root);

    if (old_wifi_log != wifi_debug_log_enabled || old_usb_serial != wifi_debug_usb_serial_enabled ||
        old_state_events != wifi_debug_state_events_enabled) {
        ESP_LOGI(TAG, "Device debug config: wifi_log=%d usb_serial=%d state_events=%d",
                 wifi_debug_log_enabled ? 1 : 0, wifi_debug_usb_serial_enabled ? 1 : 0,
                 wifi_debug_state_events_enabled ? 1 : 0);
    }
}

static bool wifi_debug_fetch_config()
{
    std::string url = make_server_url("/device/config");
    url += "?device_id=";
    url += url_encode(mac_address().c_str());
    std::string response;
    wifi_debug_internal_http = true;
    bool ok = http_get_string(url, &response, 5000);
    wifi_debug_internal_http = false;
    if (ok && !response.empty()) {
        wifi_debug_apply_config_response(response);
    }
    return ok;
}

static void wifi_debug_queue_state_snapshot(const char* reason)
{
    XiaopaiStateSnapshot xiaopai = xiaopai_state_get();
    debug_events_push_xiaopai_state("", xiaopai.name, xiaopai.generation, xiaopai.is_speaking,
                                    xiaopai.can_sample_mic, reason);
    ExpressionStateSnapshot expression = expression_state_get();
    debug_events_push_expression_state("", expression.name, expression.screen_visible, expression.sleep_dark,
                                       expression.animation_active, reason);
}

static bool wifi_debug_post_batch(std::vector<std::string>& events)
{
    if (events.empty()) {
        return true;
    }
    std::string body = "{\"device_id\":\"";
    body += json_escape(mac_address());
    body += "\",\"events\":[";
    for (size_t i = 0; i < events.size(); ++i) {
        if (i > 0) {
            body += ",";
        }
        body += events[i];
    }
    body += "]}";
    return wifi_debug_post_json("/device/logs", body, 5000);
}

static void run_wifi_debug_loop()
{
    ensure_client_id();
    while (!wifi_is_connected() || !active_server_selected) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    wifi_debug_fetch_config();
    wifi_debug_queue_state_snapshot("boot");
    ESP_LOGI(TAG, "WiFi debug log task started");

    uint32_t last_config_ms = M5.millis();
    while (true) {
        if (!wifi_is_connected() || !active_server_selected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t now = M5.millis();
        if (static_cast<uint32_t>(now - last_config_ms) >= static_cast<uint32_t>(wifi_debug_config_poll_ms)) {
            wifi_debug_fetch_config();
            last_config_ms = now;
        }

        std::vector<std::string> events;
        events.reserve(8);
        for (int i = 0; i < 8; ++i) {
            DebugEventItem item;
            TickType_t timeout = events.empty() ? pdMS_TO_TICKS(wifi_debug_post_interval_ms) : 0;
            if (!debug_events_pop(&item, timeout)) {
                break;
            }
            std::string event_json;
            if (wifi_debug_event_to_json(item, &event_json)) {
                events.push_back(event_json);
            }
        }
        if (!events.empty()) {
            wifi_debug_post_batch(events);
        }
    }
}

static void start_wifi_debug_service()
{
#if !CONFIG_STACKCHAN_WIFI_DEBUG
    ESP_LOGI(TAG, "WiFi debug log task is disabled by Kconfig");
    return;
#endif
    if (wifi_debug_task_handle != nullptr) {
        return;
    }
    BaseType_t created = xTaskCreatePinnedToCore([](void*) {
        run_wifi_debug_loop();
        wifi_debug_task_handle = nullptr;
        audio_service_register_wifi_debug_task(nullptr);
        vTaskDelete(nullptr);
    }, "wifi_debug", kWifiDebugTaskStackBytes, nullptr, 3, &wifi_debug_task_handle, 0);
    if (created == pdPASS) {
        audio_service_register_wifi_debug_task(wifi_debug_task_handle);
        ESP_LOGI(TAG, "WiFi debug log task created");
    } else {
        wifi_debug_task_handle = nullptr;
        ESP_LOGE(TAG, "Failed to create WiFi debug log task");
    }
}
