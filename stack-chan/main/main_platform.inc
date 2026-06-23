static esp_err_t init_nvs_once()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static void force_core_s3_display_board()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("M5GFX", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open M5GFX failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_u32(nvs_handle, "AUTODETECT", static_cast<uint32_t>(m5gfx::board_t::board_M5StackCoreS3));
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

static void ensure_client_id()
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("xiaopai", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(client_id, sizeof(client_id), "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
                 mac[2], mac[3], mac[4], mac[5]);
        return;
    }

    size_t length = sizeof(client_id);
    err = nvs_get_str(nvs_handle, "client_id", client_id, &length);
    if (err == ESP_OK && strlen(client_id) > 0) {
        nvs_close(nvs_handle);
        return;
    }

    uint32_t r0 = esp_random();
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uint32_t r3 = esp_random();
    snprintf(client_id, sizeof(client_id), "%08lx-%04lx-4%03lx-%04lx-%012llx", static_cast<unsigned long>(r0),
             static_cast<unsigned long>(r1 & 0xffff), static_cast<unsigned long>(r2 & 0x0fff),
             static_cast<unsigned long>((r2 & 0x3fff) | 0x8000),
             static_cast<unsigned long long>((static_cast<uint64_t>(r3) << 16) | (r0 & 0xffff)));
    nvs_set_str(nvs_handle, "client_id", client_id);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

static std::string mac_address()
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    return std::string(mac_str);
}

struct ParsedUrl {
    std::string host;
    std::string path = "/";
    int port = 443;
    bool tls = true;
};

static bool parse_websocket_url(const std::string& url, ParsedUrl& parsed)
{
    const char* wss = "wss://";
    const char* ws = "ws://";
    size_t offset = 0;
    if (url.rfind(wss, 0) == 0) {
        parsed.tls = true;
        parsed.port = 443;
        offset = strlen(wss);
    } else if (url.rfind(ws, 0) == 0) {
        parsed.tls = false;
        parsed.port = 80;
        offset = strlen(ws);
    } else {
        return false;
    }

    size_t slash = url.find('/', offset);
    std::string host_port = slash == std::string::npos ? url.substr(offset) : url.substr(offset, slash - offset);
    parsed.path = slash == std::string::npos ? "/" : url.substr(slash);
    size_t colon = host_port.rfind(':');
    if (colon != std::string::npos) {
        parsed.host = host_port.substr(0, colon);
        parsed.port = atoi(host_port.substr(colon + 1).c_str());
    } else {
        parsed.host = host_port;
    }
    return !parsed.host.empty() && parsed.port > 0;
}

static int32_t average_abs_level(const int16_t* samples, size_t count)
{
    int64_t sum = 0;
    for (size_t i = 0; i < count; ++i) {
        sum += abs(samples[i]);
    }
    return static_cast<int32_t>(sum / std::max<size_t>(count, 1));
}

static bool mic_record_blocking(int16_t* samples, size_t count, uint32_t sample_rate)
{
    if (samples == nullptr || count == 0 || sample_rate != kAudioSampleRate) {
        return false;
    }
    size_t read = audio_service_read_clean_16k(samples, count, pdMS_TO_TICKS(250));
    return read == count;
}
