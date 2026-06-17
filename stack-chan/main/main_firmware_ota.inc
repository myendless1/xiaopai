struct FirmwareOtaInfo {
    std::string version;
    std::string url;
    std::string sha256;
    int size = 0;
    bool force = false;
};

static bool firmware_version_is_numeric(const std::string& version)
{
    if (version.empty()) {
        return false;
    }
    bool saw_digit = false;
    for (char ch : version) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            saw_digit = true;
            continue;
        }
        if (ch != '.') {
            return false;
        }
    }
    return saw_digit;
}

static int compare_version_strings(const std::string& left, const std::string& right)
{
    size_t li = 0;
    size_t ri = 0;
    while (li < left.size() || ri < right.size()) {
        long lv = 0;
        long rv = 0;
        while (li < left.size() && left[li] == '.') {
            ++li;
        }
        while (ri < right.size() && right[ri] == '.') {
            ++ri;
        }
        while (li < left.size() && std::isdigit(static_cast<unsigned char>(left[li]))) {
            lv = lv * 10 + (left[li++] - '0');
        }
        while (ri < right.size() && std::isdigit(static_cast<unsigned char>(right[ri]))) {
            rv = rv * 10 + (right[ri++] - '0');
        }
        if (lv < rv) {
            return -1;
        }
        if (lv > rv) {
            return 1;
        }
        while (li < left.size() && left[li] != '.') {
            ++li;
        }
        while (ri < right.size() && right[ri] != '.') {
            ++ri;
        }
    }
    return 0;
}

static bool parse_firmware_ota_info(const std::string& response, FirmwareOtaInfo* out)
{
    if (out == nullptr || response.empty()) {
        return false;
    }
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "OTA config is not JSON");
        return false;
    }
    const cJSON* firmware = cJSON_GetObjectItemCaseSensitive(root, "firmware");
    if (!cJSON_IsObject(firmware)) {
        cJSON_Delete(root);
        return false;
    }
    out->version = json_string_value(firmware, "version");
    out->url = json_string_value(firmware, "url");
    out->sha256 = json_string_value(firmware, "sha256");
    out->size = json_int_value(firmware, "size", 0);
    out->force = json_bool_value(firmware, "force", false);
    cJSON_Delete(root);
    return !out->version.empty() && !out->url.empty();
}

static bool firmware_ota_is_newer(const FirmwareOtaInfo& info)
{
    const esp_app_desc_t* app = esp_app_get_description();
    const std::string current = app != nullptr ? std::string(app->version) : std::string();
    if (!firmware_version_is_numeric(current) && firmware_version_is_numeric(info.version)) {
        ESP_LOGW(TAG, "Current firmware version is not numeric (%s); accepting advertised OTA %s", current.c_str(),
                 info.version.c_str());
        return true;
    }
    const int cmp = compare_version_strings(current, info.version);
    ESP_LOGI(TAG, "Firmware OTA version check: current=%s latest=%s cmp=%d force=%d", current.c_str(),
             info.version.c_str(), cmp, info.force);
    return info.force || cmp < 0;
}

static void show_firmware_ota_progress(const FirmwareOtaInfo& info, int percent, bool success = false)
{
    percent = std::max(0, std::min(100, percent));

    char downloaded_line[32];
    snprintf(downloaded_line, sizeof(downloaded_line), "Downloaded %d%%", percent);
    set_app1_status("OTA Update", "New version detected", downloaded_line, "Will reboot automatically", true, success);

    M5Lock lock;
    mark_expression_screen_dirty();
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(middle_center);
    display.setTextSize(1);

    display.setFont(&fonts::Font4);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("OTA Update", display.width() / 2, 28);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("New version detected", display.width() / 2, 84);
    display.drawString(downloaded_line, display.width() / 2, 116);
    display.drawString("Will reboot automatically", display.width() / 2, 148);

    if (!info.version.empty()) {
        char version_line[64];
        snprintf(version_line, sizeof(version_line), "Version %s", info.version.c_str());
        display.setTextColor(TFT_DARKGREY, TFT_BLACK);
        display.drawString(version_line, display.width() / 2, 202);
    }
}

static bool firmware_ota_download_and_apply(const FirmwareOtaInfo& info)
{
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    if (target == nullptr) {
        ESP_LOGE(TAG, "No OTA update partition available");
        set_app1_status("OTA Fail", "No OTA partition", "Flash manually once", "", false, false);
        return false;
    }
    ESP_LOGI(TAG, "Firmware OTA target partition: %s offset=0x%lx size=%lu",
             target->label, static_cast<unsigned long>(target->address), static_cast<unsigned long>(target->size));
    if (running != nullptr) {
        ESP_LOGI(TAG, "Firmware OTA running partition: %s", running->label);
    }

    esp_http_client_config_t config = {};
    config.url = info.url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = CONFIG_STACKCHAN_FIRMWARE_OTA_TIMEOUT_MS;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;
    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_app1_status("OTA Fail", "HTTP init failed", "", "", false, false);
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Firmware OTA open failed: %s", esp_err_to_name(err));
        set_app1_status("OTA Fail", esp_err_to_name(err), info.url.c_str(), "", false, false);
        esp_http_client_cleanup(client);
        return false;
    }

    const int content_length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Firmware OTA HTTP status=%d content_length=%d", status, content_length);
    if (status < 200 || status >= 300) {
        char line[64];
        snprintf(line, sizeof(line), "HTTP status %d", status);
        set_app1_status("OTA Fail", line, "Firmware download rejected", "", false, false);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    if (content_length <= 0) {
        set_app1_status("OTA Fail", "No content length", "Firmware download", "", false, false);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    if (static_cast<size_t>(content_length) > target->size) {
        set_app1_status("OTA Fail", "Firmware too large", target->label, "", false, false);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    esp_ota_handle_t ota_handle = 0;
    err = esp_ota_begin(target, content_length, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        set_app1_status("OTA Fail", esp_err_to_name(err), "esp_ota_begin", "", false, false);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    char buffer[kHttpBufferSize];
    int total = 0;
    uint32_t last_status_ms = 0;
    bool ok = true;
    while (total < content_length) {
        const int read_len = esp_http_client_read(client, buffer, sizeof(buffer));
        if (read_len < 0) {
            ESP_LOGE(TAG, "Firmware OTA read failed");
            ok = false;
            break;
        }
        if (read_len == 0) {
            break;
        }
        err = esp_ota_write(ota_handle, buffer, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            ok = false;
            break;
        }
        total += read_len;
        uint32_t now = M5.millis();
        if (last_status_ms == 0 || static_cast<uint32_t>(now - last_status_ms) > 800) {
            show_firmware_ota_progress(info, total * 100 / content_length);
            last_status_ms = now;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!ok || total != content_length) {
        ESP_LOGE(TAG, "Firmware OTA incomplete: total=%d expected=%d", total, content_length);
        esp_ota_abort(ota_handle);
        set_app1_status("OTA Fail", "Download incomplete", "Will keep current firmware", "", false, false);
        return false;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        set_app1_status("OTA Fail", esp_err_to_name(err), "Image validation failed", "", false, false);
        return false;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        set_app1_status("OTA Fail", esp_err_to_name(err), "Boot partition", "", false, false);
        return false;
    }

    show_firmware_ota_progress(info, 100, true);
    ESP_LOGI(TAG, "Firmware OTA completed; restarting into version %s", info.version.c_str());
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
    return true;
}

bool check_and_apply_firmware_ota_once()
{
#if CONFIG_STACKCHAN_FIRMWARE_OTA_ENABLED
    std::string response;
    const std::string url = make_server_url("/realtime/config");
    set_app1_status("OTA", "Checking firmware", url.c_str(), "", true, false);
    if (!http_get_string(url, &response, CONFIG_STACKCHAN_FIRMWARE_OTA_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "Firmware OTA config check failed");
        return false;
    }

    FirmwareOtaInfo info;
    if (!parse_firmware_ota_info(response, &info)) {
        ESP_LOGI(TAG, "No firmware OTA advertised");
        return false;
    }
    if (!firmware_ota_is_newer(info)) {
        set_app1_status("OTA", "Firmware is current", esp_app_get_description()->version, "", false, true);
        vTaskDelay(pdMS_TO_TICKS(500));
        return false;
    }

    char line[96];
    snprintf(line, sizeof(line), "New version %s", info.version.c_str());
    set_app1_status("OTA", line, "Downloading firmware", "", true, false);
    show_firmware_ota_progress(info, 0);
    return firmware_ota_download_and_apply(info);
#else
    return false;
#endif
}
