static std::string url_encode(const char* value)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
        unsigned char c = *p;
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

static std::string make_tts_url()
{
    std::string url = CONFIG_STACKCHAN_TTS_URL;
    url += (url.find('?') == std::string::npos) ? "?text=" : "&text=";
    url += url_encode(CONFIG_STACKCHAN_TTS_TEXT);
    return url;
}

static void append_query_param(std::string& url, const char* key, const char* value)
{
    if (value == nullptr || value[0] == '\0') {
        return;
    }
    url += (url.find('?') == std::string::npos) ? "?" : "&";
    url += key;
    url += "=";
    url += url_encode(value);
}

static void append_query_param_int(std::string& url, const char* key, int value)
{
    if (value == 0) {
        return;
    }
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%d", value);
    append_query_param(url, key, buffer);
}

static std::string make_stream_tts_url_for_text(const char* text, const char* voice = nullptr, int sample_rate = 0,
                                                int volume = 0, int speech_rate = 0, int pitch_rate = 0)
{
    std::string url = make_server_url("/stream-speak");
    append_query_param(url, "voice", voice);
    append_query_param_int(url, "sample_rate", sample_rate);
    append_query_param_int(url, "volume", volume);
    append_query_param_int(url, "speech_rate", speech_rate);
    append_query_param_int(url, "pitch_rate", pitch_rate);
    append_query_param(url, "text", text != nullptr ? text : "");
    return url;
}

static std::string make_event_audio_url(const char* name)
{
    std::string url = make_server_url("/event-audio/");
    url += url_encode(name != nullptr ? name : "");
    url += ".pcm";
    return url;
}

static std::string make_stream_tts_url()
{
    return make_stream_tts_url_for_text(CONFIG_STACKCHAN_TTS_TEXT);
}

static bool init_camera_once()
{
    if (camera_initialized) {
        return true;
    }

    camera_config_t config = {};
    config.pin_pwdn = -1;
    config.pin_reset = -1;
    config.pin_xclk = -1;
    config.pin_sccb_sda = 12;
    config.pin_sccb_scl = 11;
    config.pin_d7 = 47;
    config.pin_d6 = 48;
    config.pin_d5 = 16;
    config.pin_d4 = 15;
    config.pin_d3 = 42;
    config.pin_d2 = 41;
    config.pin_d1 = 40;
    config.pin_d0 = 39;
    config.pin_vsync = 46;
    config.pin_href = 38;
    config.pin_pclk = 45;
    config.xclk_freq_hz = 20000000;
    config.ledc_timer = LEDC_TIMER_0;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 0;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.sccb_i2c_port = -1;

    set_tracking_status("Camera", "Initializing GC0308", "QVGA RGB565", "");
    M5.In_I2C.release();
    camera_owns_internal_i2c = true;
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_camera_init failed: %s", esp_err_to_name(err));
        esp_camera_deinit();
        M5.In_I2C.begin();
        camera_owns_internal_i2c = false;
        set_tracking_status("Cam Fail", esp_err_to_name(err), "Check CoreS3 camera", "", false, false);
        return false;
    }

    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor != nullptr) {
        sensor->set_framesize(sensor, FRAMESIZE_QVGA);
        sensor->set_pixformat(sensor, PIXFORMAT_RGB565);
    }

    camera_initialized = true;
    return true;
}

static void release_camera_driver()
{
    if (camera_initialized) {
        esp_camera_deinit();
        camera_initialized = false;
    }
    M5.In_I2C.begin();
    camera_owns_internal_i2c = false;
    local_voice_apply_outputs(local_voice_current_state());
}

static camera_fb_t* get_fresh_camera_frame(const char* context)
{
    for (int i = 0; i < kCameraFreshDiscardFrames; ++i) {
        camera_fb_t* stale = esp_camera_fb_get();
        if (stale == nullptr) {
            ESP_LOGW(TAG, "Camera stale-frame flush failed context=%s index=%d", context != nullptr ? context : "",
                     i + 1);
            break;
        }
        ESP_LOGI(TAG, "Camera stale-frame flushed context=%s index=%d len=%u", context != nullptr ? context : "",
                 i + 1, static_cast<unsigned>(stale->len));
        esp_camera_fb_return(stale);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    vTaskDelay(pdMS_TO_TICKS(80));
    return esp_camera_fb_get();
}

struct FaceTarget {
    bool found = false;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float area = 0.0f;
};

struct CapturedFrame {
    std::vector<uint8_t> data;
    uint32_t width = 0;
    uint32_t height = 0;
    pixformat_t format = PIXFORMAT_RGB565;
};

static bool capture_frame_copy(const char* context, CapturedFrame* captured)
{
    if (captured == nullptr) {
        return false;
    }
    captured->data.clear();

    VoiceListenerPauseGuard voice_pause("camera capture");
    if (!init_camera_once()) {
        return false;
    }

    camera_fb_t* frame = get_fresh_camera_frame(context);
    if (frame == nullptr) {
        ESP_LOGE(TAG, "esp_camera_fb_get failed");
        release_camera_driver();
        return false;
    }

    captured->width = frame->width;
    captured->height = frame->height;
    captured->format = frame->format;
    captured->data.assign(frame->buf, frame->buf + frame->len);
    esp_camera_fb_return(frame);
    release_camera_driver();
    return true;
}

struct ScanPose {
    const char* label;
    float yaw;
    float pitch;
};

struct ScanCandidate {
    bool found = false;
    FaceTarget face;
    float pose_yaw = 0.0f;
    float pose_pitch = kTrackingHomePitchDeg;
    float pixel_error = 0.0f;
};

static float clamp_float(float value, float min_value, float max_value)
{
    return std::max(min_value, std::min(max_value, value));
}

static int speaker_volume_raw_from_percent(int percent)
{
    int clamped = std::max(kSpeakerVolumePercentMin, std::min(kSpeakerVolumePercentMax, percent));
    return std::max(0, std::min(255, (clamped * 255 + 50) / 100));
}

static void apply_speaker_volume()
{
    M5.Speaker.setVolume(speaker_volume_raw_from_percent(speaker_volume_percent));
}

static uint8_t scs_checksum(const uint8_t* data, size_t length_without_checksum)
{
    uint32_t sum = 0;
    for (size_t i = 2; i < length_without_checksum; ++i) {
        sum += data[i];
    }
    return static_cast<uint8_t>(~(sum & 0xff));
}

static bool init_servo_uart()
{
    if (servo_uart_initialized) {
        return true;
    }

    uart_config_t uart_config = {};
    uart_config.baud_rate = kServoBaud;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    esp_err_t err = uart_driver_install(kServoUart, 1024, 0, 0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_param_config(kServoUart, &uart_config));
    ESP_ERROR_CHECK_WITHOUT_ABORT(uart_set_pin(kServoUart, kServoTxPin, kServoRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_flush(kServoUart);
    servo_uart_initialized = true;
    ESP_LOGI(TAG, "Servo UART ready: uart=%d tx=%d rx=%d baud=%d", static_cast<int>(kServoUart), kServoTxPin,
             kServoRxPin, kServoBaud);
    return true;
}

static bool scs_write(uint8_t id, uint8_t address, const uint8_t* values, size_t value_count)
{
    if (!init_servo_uart()) {
        return false;
    }
    uint8_t packet[16] = {};
    const size_t length = value_count + 3;
    const size_t packet_len = value_count + 7;
    if (packet_len > sizeof(packet)) {
        return false;
    }
    packet[0] = 0xff;
    packet[1] = 0xff;
    packet[2] = id;
    packet[3] = static_cast<uint8_t>(length);
    packet[4] = 0x03;
    packet[5] = address;
    for (size_t i = 0; i < value_count; ++i) {
        packet[6 + i] = values[i];
    }
    packet[packet_len - 1] = scs_checksum(packet, packet_len - 1);
    int written = uart_write_bytes(kServoUart, packet, packet_len);
    uart_wait_tx_done(kServoUart, pdMS_TO_TICKS(40));
    return written == static_cast<int>(packet_len);
}

static bool set_servo_torque(uint8_t id, bool enabled)
{
    uint8_t value = enabled ? 1 : 0;
    return scs_write(id, 40, &value, 1);
}

static int angle_deg_to_raw(float angle_deg, int zero_raw)
{
    int raw = static_cast<int>(std::lround(zero_raw + angle_deg * kServoStepsPerDegree));
    return std::max(0, std::min(1000, raw));
}

static bool set_servo_raw_in_time(uint8_t id, int raw, uint16_t time_ms)
{
    uint8_t values[4] = {
        static_cast<uint8_t>((raw >> 8) & 0xff),
        static_cast<uint8_t>(raw & 0xff),
        static_cast<uint8_t>((time_ms >> 8) & 0xff),
        static_cast<uint8_t>(time_ms & 0xff),
    };
    return scs_write(id, 42, values, sizeof(values));
}

static bool py32_write_bit(uint8_t low_reg, uint8_t high_reg, uint8_t pin, bool enabled)
{
    uint8_t reg = pin < 8 ? low_reg : high_reg;
    uint8_t mask = 1 << (pin < 8 ? pin : pin - 8);
    uint8_t value = M5.In_I2C.readRegister8(kPy32Address, reg, kPy32I2cFreq);
    value = enabled ? (value | mask) : (value & ~mask);
    return M5.In_I2C.writeRegister8(kPy32Address, reg, value, kPy32I2cFreq);
}

static bool py32_wait_ready_locked(uint8_t* version_out)
{
    uint8_t version = 0;
    const uint32_t start_ms = M5.millis();
    while (true) {
        version = M5.In_I2C.readRegister8(kPy32Address, kPy32VersionReg, kPy32I2cFreq);
        if (version != 0 && version != 0xff) {
            if (version_out != nullptr) {
                *version_out = version;
            }
            return true;
        }
        if (static_cast<uint32_t>(M5.millis() - start_ms) > 1200) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    uint32_t now = M5.millis();
    if (light_strip_last_probe_ms == 0 ||
        static_cast<uint32_t>(now - light_strip_last_probe_ms) > 5000) {
        ESP_LOGW(TAG, "PY32 IO expander not ready; will retry");
        light_strip_last_probe_ms = now;
    }
    light_strip_missing = true;
    return false;
}

static void py32_enable_vm_power_locked()
{
    if (servo_power_ready) {
        return;
    }

    py32_write_bit(0x03, 0x04, kPy32ServoPowerPin, true);  // direction output
    py32_write_bit(0x0b, 0x0c, kPy32ServoPowerPin, false); // pull-down off
    py32_write_bit(0x09, 0x0a, kPy32ServoPowerPin, true);  // pull-up on
    py32_write_bit(0x05, 0x06, kPy32ServoPowerPin, true);  // VM power on
    servo_power_ready = true;
    vTaskDelay(pdMS_TO_TICKS(200));
}

static uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

static bool py32_write_light_strip_color_locked(uint8_t r, uint8_t g, uint8_t b)
{
    const uint16_t color565 = rgb888_to_rgb565(r, g, b);
    const uint8_t low = static_cast<uint8_t>(color565 & 0xff);
    const uint8_t high = static_cast<uint8_t>((color565 >> 8) & 0xff);
    uint8_t data[kLightStripLedCount * 2] = {};

    for (uint8_t i = 0; i < kLightStripLedCount; ++i) {
        data[i * 2] = low;
        data[i * 2 + 1] = high;
    }

    if (!M5.In_I2C.writeRegister8(kPy32Address, kPy32LedConfigReg, kLightStripLedCount & 0x3f, kPy32I2cFreq)) {
        return false;
    }
    if (!M5.In_I2C.writeRegister(kPy32Address, kPy32LedRamStartReg, data, sizeof(data), kPy32I2cFreq)) {
        return false;
    }

    return M5.In_I2C.writeRegister8(kPy32Address, kPy32LedConfigReg,
                                    (kLightStripLedCount & 0x3f) | (1 << 6), kPy32I2cFreq);
}

static void set_rgb565_bytes(uint8_t* data, uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t color565 = rgb888_to_rgb565(r, g, b);
    data[index * 2] = static_cast<uint8_t>(color565 & 0xff);
    data[index * 2 + 1] = static_cast<uint8_t>((color565 >> 8) & 0xff);
}

static uint8_t light_strip_index_for_side_pos(uint8_t side, uint8_t pos)
{
    uint8_t physical_pos = pos;
    if (kLightStripReverseSecondSide && side == 1) {
        physical_pos = static_cast<uint8_t>(kLightStripSideLedCount - 1 - pos);
    }
    return static_cast<uint8_t>(side * kLightStripSideLedCount + physical_pos);
}

static bool py32_write_light_strip_bar_locked(uint8_t level, uint8_t base_r, uint8_t base_g, uint8_t base_b,
                                              uint8_t peak_r, uint8_t peak_g, uint8_t peak_b)
{
    uint8_t active = std::min<uint8_t>(level, kLightStripSideLedCount);
    uint8_t data[kLightStripLedCount * 2] = {};

    for (uint8_t side = 0; side < 2; ++side) {
        for (uint8_t pos = 0; pos < kLightStripSideLedCount; ++pos) {
            if (pos >= active) {
                continue;
            }
            uint8_t index = light_strip_index_for_side_pos(side, pos);
            uint8_t ramp = static_cast<uint8_t>(std::min<int>(255, 70 + pos * 18 + active * 8));
            uint8_t r = static_cast<uint8_t>(std::min<int>(255, base_r * ramp / 100));
            uint8_t g = static_cast<uint8_t>(std::min<int>(255, base_g * ramp / 100));
            uint8_t b = static_cast<uint8_t>(std::min<int>(255, base_b * ramp / 100));
            if (active >= kLightStripSideLedCount && pos == kLightStripSideLedCount - 1) {
                r = peak_r;
                g = peak_g;
                b = peak_b;
            }
            set_rgb565_bytes(data, index, r, g, b);
        }
    }

    if (!M5.In_I2C.writeRegister8(kPy32Address, kPy32LedConfigReg, kLightStripLedCount & 0x3f, kPy32I2cFreq)) {
        return false;
    }
    if (!M5.In_I2C.writeRegister(kPy32Address, kPy32LedRamStartReg, data, sizeof(data), kPy32I2cFreq)) {
        return false;
    }
    return M5.In_I2C.writeRegister8(kPy32Address, kPy32LedConfigReg,
                                    (kLightStripLedCount & 0x3f) | (1 << 6), kPy32I2cFreq);
}

static bool ensure_light_strip_ready_locked()
{
    if (light_strip_ready) {
        return true;
    }
    if (camera_owns_internal_i2c) {
        return false;
    }

    uint8_t version = 0;
    if (!py32_wait_ready_locked(&version)) {
        return false;
    }

    light_strip_missing = false;
    py32_enable_vm_power_locked();
    py32_write_bit(0x03, 0x04, kPy32RgbPin, true);   // direction output
    py32_write_bit(0x0b, 0x0c, kPy32RgbPin, false);  // pull-down off
    py32_write_bit(0x09, 0x0a, kPy32RgbPin, true);   // pull-up on
    py32_write_bit(0x13, 0x14, kPy32RgbPin, false);  // push-pull
    M5.In_I2C.writeRegister8(kPy32Address, kPy32LedConfigReg, kLightStripLedCount & 0x3f, kPy32I2cFreq);
    vTaskDelay(pdMS_TO_TICKS(200));
    py32_write_light_strip_color_locked(0, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    py32_write_light_strip_color_locked(0, 0, 0);
    light_strip_ready = true;
    ESP_LOGI(TAG, "RGB light strip ready via PY32 version=0x%02x", version);
    return true;
}

static void set_light_strip_color(uint8_t r, uint8_t g, uint8_t b, int state)
{
    if (light_strip_state == state && light_strip_ready) {
        return;
    }
    if (camera_owns_internal_i2c) {
        return;
    }

    M5Lock lock;
    if (!ensure_light_strip_ready_locked()) {
        return;
    }
    if (py32_write_light_strip_color_locked(r, g, b)) {
        light_strip_state = state;
    }
}

static void set_light_strip_listening()
{
    light_strip_listening_after_speech = true;
    listening_light_last_update_ms = 0;
    set_light_strip_listening_bar(1);
}

static void set_light_strip_speaking()
{
    set_light_strip_color(kLightStripSpeakingR, kLightStripSpeakingG, kLightStripSpeakingB, 2);
}

static void set_light_strip_speaking_bar(uint8_t level)
{
    if (camera_owns_internal_i2c) {
        return;
    }

    M5Lock lock;
    if (!ensure_light_strip_ready_locked()) {
        return;
    }
    if (py32_write_light_strip_bar_locked(level, 0, 0, 48, 28, 38, 90)) {
        light_strip_state = 20 + std::min<uint8_t>(level, kLightStripSideLedCount);
    }
}

static void set_light_strip_listening_bar(uint8_t level)
{
    if (camera_owns_internal_i2c) {
        return;
    }

    M5Lock lock;
    if (!ensure_light_strip_ready_locked()) {
        return;
    }
    if (py32_write_light_strip_bar_locked(level, 0, 36, 0, 22, 90, 28)) {
        light_strip_state = 30 + std::min<uint8_t>(level, kLightStripSideLedCount);
    }
}

static void set_light_strip_sleeping()
{
    light_strip_listening_after_speech = false;
    listening_light_last_update_ms = 0;
    set_light_strip_color(0, 0, 0, 3);
}

static void set_waiting_outputs()
{
    light_strip_listening_after_speech = false;
    listening_light_last_update_ms = 0;
    set_light_strip_color(kLightStripWaitingR, kLightStripWaitingG, kLightStripWaitingB, 5);
    show_expression("thinking");
}

static void run_light_strip_boot_probe()
{
    set_light_strip_color(0, 0, 32, 4);
    vTaskDelay(pdMS_TO_TICKS(150));
    local_voice_apply_outputs(local_voice_current_state());
}

static void enable_servo_power()
{
    if (servo_power_ready) {
        return;
    }
    if (camera_owns_internal_i2c) {
        ESP_LOGW(TAG, "Cannot enable servo power while camera owns internal I2C");
        return;
    }
    M5Lock lock;
    uint8_t version = 0;
    if (!py32_wait_ready_locked(&version)) {
        ESP_LOGW(TAG, "PY32 IO expander not ready; servo power not enabled yet");
        return;
    }
    py32_enable_vm_power_locked();
    ESP_LOGI(TAG, "Servo power enabled via PY32 version=0x%02x", version);
}

static bool move_head_to_tracking_angles(float yaw_deg, float pitch_deg, uint16_t time_ms)
{
    enable_servo_power();
    tracking_yaw_deg = clamp_float(yaw_deg, kTrackingYawMinDeg, kTrackingYawMaxDeg);
    tracking_pitch_deg = clamp_float(pitch_deg, kTrackingPitchMinDeg, kTrackingPitchMaxDeg);
    int yaw_raw = angle_deg_to_raw(tracking_yaw_deg, kServoYawZeroRaw);
    int pitch_raw = angle_deg_to_raw(tracking_pitch_deg, kServoPitchZeroRaw);
    set_servo_torque(kServoPanId, true);
    vTaskDelay(pdMS_TO_TICKS(30));
    set_servo_torque(kServoTiltId, true);
    vTaskDelay(pdMS_TO_TICKS(30));
    bool ok_yaw = set_servo_raw_in_time(kServoPanId, yaw_raw, time_ms);
    vTaskDelay(pdMS_TO_TICKS(90));
    bool ok_pitch = set_servo_raw_in_time(kServoTiltId, pitch_raw, time_ms);
    vTaskDelay(pdMS_TO_TICKS(90));
    ESP_LOGI(TAG, "Head move yaw=%.1f raw=%d pitch=%.1f raw=%d ok=%d/%d", tracking_yaw_deg, yaw_raw,
             tracking_pitch_deg, pitch_raw, ok_yaw, ok_pitch);
    return ok_yaw && ok_pitch;
}

static bool run_node_head_command()
{
    static constexpr float kNodeHeadPitchDeltaDeg = 10.0f;
    static constexpr uint16_t kNodeHeadStepMs = 500;
    static constexpr uint16_t kNodeHeadCommandOverheadMs = 240;
    static constexpr uint16_t kNodeHeadStepGapMs = kNodeHeadStepMs > kNodeHeadCommandOverheadMs
        ? kNodeHeadStepMs - kNodeHeadCommandOverheadMs
        : 1;
    const float start_pitch = tracking_pitch_deg;
    const float up_pitch = clamp_float(start_pitch + kNodeHeadPitchDeltaDeg, kTrackingPitchMinDeg, kTrackingPitchMaxDeg);
    const float down_pitch = clamp_float(start_pitch - kNodeHeadPitchDeltaDeg, kTrackingPitchMinDeg, kTrackingPitchMaxDeg);
    const bool too_low = start_pitch <= kTrackingPitchMinDeg + kNodeHeadPitchDeltaDeg;
    const bool too_high = start_pitch >= kTrackingPitchMaxDeg - kNodeHeadPitchDeltaDeg;
    const bool start_up = too_low || (!too_high && start_pitch < kTrackingHomePitchDeg - kNodeHeadPitchDeltaDeg);
    const float sequence[4] = {
        start_up ? up_pitch : down_pitch,
        start_up ? down_pitch : up_pitch,
        start_up ? up_pitch : down_pitch,
        start_pitch,
    };

    bool ok = true;
    for (float pitch : sequence) {
        ok = move_head_to_tracking_angles(tracking_yaw_deg, pitch, kNodeHeadStepMs) && ok;
        vTaskDelay(pdMS_TO_TICKS(kNodeHeadStepGapMs));
    }
    return ok;
}

static bool move_tilt_only_for_test(float pitch_deg, uint16_t time_ms)
{
    enable_servo_power();
    float clamped_pitch = clamp_float(pitch_deg, kTrackingPitchMinDeg, kTrackingPitchMaxDeg);
    int pitch_raw = angle_deg_to_raw(clamped_pitch, kServoPitchZeroRaw);
    set_servo_torque(kServoTiltId, true);
    bool ok_pitch = set_servo_raw_in_time(kServoTiltId, pitch_raw, time_ms);
    ESP_LOGI(TAG, "Tilt-only test pitch=%.1f raw=%d ok=%d", clamped_pitch, pitch_raw, ok_pitch);
    return ok_pitch;
}

static bool move_pan_only_for_test(float yaw_deg, uint16_t time_ms)
{
    enable_servo_power();
    float clamped_yaw = clamp_float(yaw_deg, kTrackingYawMinDeg, kTrackingYawMaxDeg);
    int yaw_raw = angle_deg_to_raw(clamped_yaw, kServoYawZeroRaw);
    set_servo_torque(kServoPanId, true);
    bool ok_yaw = set_servo_raw_in_time(kServoPanId, yaw_raw, time_ms);
    ESP_LOGI(TAG, "Pan-only test yaw=%.1f raw=%d ok=%d", clamped_yaw, yaw_raw, ok_yaw);
    return ok_yaw;
}

static bool move_center_for_test()
{
    bool ok_pan = move_pan_only_for_test(0.0f, 450);
    vTaskDelay(pdMS_TO_TICKS(220));
    bool ok_tilt = move_tilt_only_for_test(kTrackingHomePitchDeg, 550);
    if (ok_pan) {
        tracking_yaw_deg = 0.0f;
    }
    if (ok_tilt) {
        tracking_pitch_deg = kTrackingHomePitchDeg;
    }
    ESP_LOGI(TAG, "Center-only test home pitch=%.1f ok=%d/%d", kTrackingHomePitchDeg, ok_pan, ok_tilt);
    return ok_pan && ok_tilt;
}

static bool parse_face_target(const std::string& response, FaceTarget* target)
{
    if (target == nullptr) {
        return false;
    }
    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        return false;
    }

    cJSON* face_detection = cJSON_GetObjectItemCaseSensitive(root, "face_detection");
    cJSON* face = cJSON_GetObjectItemCaseSensitive(face_detection, "best_face");
    if (!cJSON_IsObject(face)) {
        cJSON* faces = cJSON_GetObjectItemCaseSensitive(face_detection, "faces");
        if (cJSON_IsArray(faces) && cJSON_GetArraySize(faces) > 0) {
            face = cJSON_GetArrayItem(faces, 0);
        }
    }
    if (!cJSON_IsObject(face)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* center = cJSON_GetObjectItemCaseSensitive(face, "center");
    cJSON* x = cJSON_IsObject(center) ? cJSON_GetObjectItemCaseSensitive(center, "x") : nullptr;
    cJSON* y = cJSON_IsObject(center) ? cJSON_GetObjectItemCaseSensitive(center, "y") : nullptr;
    if (cJSON_IsNumber(x) && cJSON_IsNumber(y)) {
        target->center_x = static_cast<float>(x->valuedouble);
        target->center_y = static_cast<float>(y->valuedouble);
    } else {
        cJSON* left = cJSON_GetObjectItemCaseSensitive(face, "left");
        cJSON* right = cJSON_GetObjectItemCaseSensitive(face, "right");
        cJSON* top = cJSON_GetObjectItemCaseSensitive(face, "top");
        cJSON* bottom = cJSON_GetObjectItemCaseSensitive(face, "bottom");
        if (!cJSON_IsNumber(left) || !cJSON_IsNumber(right) || !cJSON_IsNumber(top) || !cJSON_IsNumber(bottom)) {
            cJSON_Delete(root);
            return false;
        }
        target->center_x = static_cast<float>((left->valuedouble + right->valuedouble) * 0.5);
        target->center_y = static_cast<float>((top->valuedouble + bottom->valuedouble) * 0.5);
    }
    cJSON* area = cJSON_GetObjectItemCaseSensitive(face, "area");
    target->area = cJSON_IsNumber(area) ? static_cast<float>(area->valuedouble) : 0.0f;
    target->found = true;
    cJSON_Delete(root);
    return true;
}

static bool upload_tracking_frame(const uint8_t* frame_data, size_t frame_len, uint32_t frame_width,
                                  uint32_t frame_height, pixformat_t frame_format, FaceTarget* target)
{
    if (frame_data == nullptr || frame_len == 0) {
        set_tracking_status("Capture Fail", "Empty camera frame", "", "", false, false);
        return false;
    }

    std::string upload_url = make_server_url("/upload-image");
    ESP_LOGI(TAG, "Uploading camera frame to %s, len=%u size=%ux%u format=%d", upload_url.c_str(),
             static_cast<unsigned>(frame_len), static_cast<unsigned>(frame_width), static_cast<unsigned>(frame_height),
             static_cast<int>(frame_format));
    set_tracking_status("Uploading", upload_url.c_str(), "Sending RGB565 frame", "");

    esp_http_client_config_t config = {};
    config.url = upload_url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kVisionUploadTimeoutMs;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_tracking_status("Upload Fail", "esp_http_client_init failed", "", "", false, false);
        return false;
    }

    char width[16];
    char height[16];
    snprintf(width, sizeof(width), "%u", static_cast<unsigned>(frame_width));
    snprintf(height, sizeof(height), "%u", static_cast<unsigned>(frame_height));
    esp_http_client_set_header(client, "Content-Type", "image/rgb565");
    esp_http_client_set_header(client, "X-Image-Format", "rgb565");
    esp_http_client_set_header(client, "X-Image-Width", width);
    esp_http_client_set_header(client, "X-Image-Height", height);
    esp_http_client_set_header(client, "X-Device-Id", mac_address().c_str());
    esp_http_client_set_header(client, "X-Client-Id", client_id);
    esp_http_client_set_header(client, "X-Visual-Tracking", "false");

    esp_err_t err = esp_http_client_open(client, frame_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera upload open failed: %s", esp_err_to_name(err));
        set_tracking_status("Upload Fail", esp_err_to_name(err), "Check server URL/IP", "", false, false);
        esp_http_client_cleanup(client);
        return false;
    }

    size_t offset = 0;
    while (offset < frame_len && !tracking_stop_requested) {
        size_t chunk = std::min<size_t>(kHttpBufferSize, frame_len - offset);
        int written = esp_http_client_write(client, reinterpret_cast<const char*>(frame_data + offset), chunk);
        if (written <= 0) {
            ESP_LOGE(TAG, "Camera upload write failed at offset=%u", static_cast<unsigned>(offset));
            set_tracking_status("Upload Fail", "HTTP write failed", "See USB serial log", "", false, false);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        offset += written;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Camera upload HTTP status=%d content_length=%d", status, content_length);

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
        ESP_LOGI(TAG, "Camera upload response: %s", response.c_str());
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (tracking_stop_requested) {
        set_tracking_status("Stopped", "Tracking stopped", "", "", false, false);
        return false;
    }

    if (status >= 200 && status < 300) {
        if (parse_face_target(response, target)) {
            char line1[64];
            snprintf(line1, sizeof(line1), "face %.0f, %.0f", target->center_x, target->center_y);
            set_tracking_status("Detected", line1, "Calculating head angle", "");
            return true;
        }
        set_tracking_status("No Face", "No face detected", "Try facing the camera", "", false, false);
        return false;
    }

    char line[48];
    snprintf(line, sizeof(line), "HTTP status %d", status);
    set_tracking_status("Upload Fail", line, "See USB serial log", "", false, false);
    return false;
}

static bool upload_camera_frame_only(const uint8_t* frame_data, size_t frame_len, uint32_t frame_width,
                                     uint32_t frame_height, pixformat_t frame_format)
{
    if (frame_data == nullptr || frame_len == 0) {
        set_camera_status("Capture Fail", "Empty camera frame", "", "", false, false);
        return false;
    }

    std::string upload_url = make_server_url("/upload-image");
    ESP_LOGI(TAG, "Uploading one camera frame to %s, len=%u size=%ux%u format=%d", upload_url.c_str(),
             static_cast<unsigned>(frame_len), static_cast<unsigned>(frame_width),
             static_cast<unsigned>(frame_height), static_cast<int>(frame_format));
    set_camera_status("Uploading", upload_url.c_str(), "Sending RGB565 frame", "");

    esp_http_client_config_t config = {};
    config.url = upload_url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kVisionUploadTimeoutMs;
    config.buffer_size = kHttpBufferSize;
    config.buffer_size_tx = kHttpBufferSize;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        set_camera_status("Upload Fail", "esp_http_client_init failed", "", "", false, false);
        return false;
    }

    char width[16];
    char height[16];
    snprintf(width, sizeof(width), "%u", static_cast<unsigned>(frame_width));
    snprintf(height, sizeof(height), "%u", static_cast<unsigned>(frame_height));
    esp_http_client_set_header(client, "Content-Type", "image/rgb565");
    esp_http_client_set_header(client, "X-Image-Format", "rgb565");
    esp_http_client_set_header(client, "X-Image-Width", width);
    esp_http_client_set_header(client, "X-Image-Height", height);
    esp_http_client_set_header(client, "X-Device-Id", mac_address().c_str());
    esp_http_client_set_header(client, "X-Client-Id", client_id);

    esp_err_t err = esp_http_client_open(client, frame_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Single camera upload open failed: %s", esp_err_to_name(err));
        set_camera_status("Upload Fail", esp_err_to_name(err), "Check server URL/IP", "", false, false);
        esp_http_client_cleanup(client);
        return false;
    }

    size_t offset = 0;
    while (offset < frame_len) {
        size_t chunk = std::min<size_t>(kHttpBufferSize, frame_len - offset);
        int written = esp_http_client_write(client, reinterpret_cast<const char*>(frame_data + offset), chunk);
        if (written <= 0) {
            ESP_LOGE(TAG, "Single camera upload write failed at offset=%u", static_cast<unsigned>(offset));
            set_camera_status("Upload Fail", "HTTP write failed", "See USB serial log", "", false, false);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        offset += written;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Single camera upload HTTP status=%d content_length=%d", status, content_length);

    std::string response;
    char response_chunk[256];
    while (response.size() < 1024) {
        int read_len = esp_http_client_read(client, response_chunk, sizeof(response_chunk) - 1);
        if (read_len <= 0) {
            break;
        }
        response.append(response_chunk, read_len);
    }
    if (!response.empty()) {
        ESP_LOGI(TAG, "Single camera upload response: %s", response.c_str());
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status >= 200 && status < 300) {
        char line1[64];
        snprintf(line1, sizeof(line1), "uploaded %u bytes", static_cast<unsigned>(frame_len));
        set_camera_status("Done", line1, "Saved on local server", "", false, true);
        return true;
    }

    char line[48];
    snprintf(line, sizeof(line), "HTTP status %d", status);
    set_camera_status("Upload Fail", line, "See USB serial log", "", false, false);
    return false;
}

static bool capture_face_at_pose(const ScanPose& pose, int step_index, int step_count, ScanCandidate* candidate)
{
    if (candidate == nullptr) {
        return false;
    }

    char line1[64];
    char line2[64];
    snprintf(line1, sizeof(line1), "scan %d/%d", step_index, step_count);
    snprintf(line2, sizeof(line2), "yaw %.0f pitch %.0f", pose.yaw, pose.pitch);
    set_tracking_status("Scanning", line1, line2, "");

    if (!move_head_to_tracking_angles(pose.yaw, pose.pitch, 700)) {
        set_tracking_status("Servo Fail", "Scan pose write failed", "Check ID/wiring", "", false, false);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(950));

    set_tracking_status("Capturing", pose.label, "Detecting face via server", "");
    CapturedFrame frame;
    if (!capture_frame_copy("scan", &frame)) {
        set_tracking_status("Capture Fail", "esp_camera_fb_get failed", "", "", false, false);
        return false;
    }

    FaceTarget target;
    bool detected = upload_tracking_frame(frame.data.data(), frame.data.size(), frame.width, frame.height,
                                          frame.format, &target);

    if (detected) {
        float dx = target.center_x - kTrackingCx;
        float dy = target.center_y - kTrackingCy;
        candidate->found = true;
        candidate->face = target;
        candidate->pose_yaw = tracking_yaw_deg;
        candidate->pose_pitch = tracking_pitch_deg;
        candidate->pixel_error = std::sqrt(dx * dx + dy * dy);
        ESP_LOGI(TAG, "Scan candidate %s pose=(%.1f,%.1f) face=(%.1f,%.1f) err=%.1f area=%.1f",
                 pose.label, candidate->pose_yaw, candidate->pose_pitch, target.center_x, target.center_y,
                 candidate->pixel_error, target.area);
    } else {
        ESP_LOGI(TAG, "Scan pose %s found no face", pose.label);
    }

    if (!tracking_stop_requested) {
        set_tracking_status("Center", "Returning home", "Next scan/photo follows", "");
        if (!move_center_for_test()) {
            set_tracking_status("Servo Fail", "Center write failed", "Check ID/wiring", "", false, false);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(800));
    }

    return detected;
}

static bool capture_face_at_current_pose(const char* stage, const char* line1, const char* line2, FaceTarget* target)
{
    if (target == nullptr) {
        return false;
    }
    set_tracking_status(stage, line1, line2, "");
    CapturedFrame frame;
    if (!capture_frame_copy("find_owner", &frame)) {
        set_tracking_status("Capture Fail", "esp_camera_fb_get failed", "", "", false, false);
        return false;
    }

    return upload_tracking_frame(frame.data.data(), frame.data.size(), frame.width, frame.height, frame.format, target);
}

static bool refine_head_toward_face(const FaceTarget& target, uint16_t duration_ms, float gain_x, float gain_y)
{
    float dx = target.center_x - kTrackingCx;
    float dy = target.center_y - kTrackingCy;
    float yaw_delta_deg = std::atan(dx / kTrackingFx) * 180.0f / kPi;
    float pitch_delta_deg = std::atan(dy / kTrackingFy) * 180.0f / kPi;
    float yaw_step = yaw_delta_deg * gain_x * kFindOwnerYawDirection;
    float pitch_step = pitch_delta_deg * gain_y * kFindOwnerPitchDirection;
    float next_yaw = tracking_yaw_deg + yaw_step;
    float next_pitch = tracking_pitch_deg + pitch_step;
    ESP_LOGI(TAG, "Find owner adjust face=(%.1f,%.1f) dx=%.1f dy=%.1f step=(%.1f,%.1f) next=(%.1f,%.1f)",
             target.center_x, target.center_y, dx, dy, yaw_step, pitch_step, next_yaw, next_pitch);
    return move_head_to_tracking_angles(next_yaw, next_pitch, duration_ms);
}

static bool run_find_owner_command(int rounds, const char* reply, float gain_x = kFindOwnerYawGain,
                                   float gain_y = kFindOwnerPitchGain, float stop_pixels = kFindOwnerStopPixels,
                                   bool preserve_speech_playback = false, bool wait_for_speech = false)
{
    ensure_client_id();
    if (!ensure_wifi_connected()) {
        return false;
    }
    if (!ensure_server_selected()) {
        return false;
    }

    if (wait_for_speech) {
        const int max_wait_ms = 3000;
        int waited_ms = 0;
        while ((speech_output_is_busy() ||
                (speak_command_queue != nullptr && uxQueueMessagesWaiting(speak_command_queue) > 0)) &&
               waited_ms < max_wait_ms) {
            vTaskDelay(pdMS_TO_TICKS(20));
            waited_ms += 20;
        }
        ESP_LOGI(TAG, "Find owner waited for speech: %dms", waited_ms);
    }

    if (preserve_speech_playback) {
        ESP_LOGI(TAG, "Find owner preserving speech playback");
    } else {
        request_speak_preempt("find owner");
        M5.Speaker.end();
    }

    int capped_rounds = std::max(1, std::min(rounds, kFindOwnerMaxRounds));
    gain_x = std::max(0.0f, gain_x);
    gain_y = std::max(0.0f, gain_y);
    stop_pixels = std::max(0.0f, stop_pixels);
    bool saw_face = false;
    bool aligned = false;
    for (int round = 1; round <= capped_rounds; ++round) {
        FaceTarget target;
        char line1[48];
        snprintf(line1, sizeof(line1), "round %d/%d", round, capped_rounds);
        if (!capture_face_at_current_pose("Find Owner", line1, "Detecting face", &target)) {
            vTaskDelay(pdMS_TO_TICKS(220));
            continue;
        }

        saw_face = true;
        float dx = target.center_x - kTrackingCx;
        float dy = target.center_y - kTrackingCy;
        float pixel_error = std::sqrt(dx * dx + dy * dy);
        if (pixel_error <= stop_pixels) {
            aligned = true;
            ESP_LOGI(TAG, "Find owner aligned: err=%.1f", pixel_error);
            break;
        }

        if (round == capped_rounds && capped_rounds > 1) {
            ESP_LOGI(TAG, "Find owner reached final capture: err=%.1f, skip final adjustment", pixel_error);
            break;
        }

        if (!refine_head_toward_face(target, 520, gain_x, gain_y)) {
            set_tracking_status("Servo Fail", "Find-owner move failed", "Check ID/wiring", "", false, false);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(650));
    }

    ESP_LOGI(TAG, "Find owner finished: saw_face=%d aligned=%d", saw_face, aligned);
    if (reply != nullptr && reply[0] != '\0') {
        return execute_speak_command_internal(reply, false);
    }
    return saw_face;
}

void run_camera_upload_app()
{
    ensure_client_id();
    if (!ensure_wifi_connected()) {
        return;
    }
    if (!ensure_server_selected()) {
        return;
    }

    M5.Speaker.stop();
    M5.Speaker.end();

    set_camera_status("Camera", "Initializing camera", "Taking one photo");
    set_camera_status("Capturing", "Taking photo", "Uploading follows");
    CapturedFrame frame;
    if (!capture_frame_copy("capture_image", &frame)) {
        set_camera_status("Capture Fail", "esp_camera_fb_get failed", "", "", false, false);
        return;
    }

    upload_camera_frame_only(frame.data.data(), frame.data.size(), frame.width, frame.height, frame.format);
}

void run_tracking_user_demo()
{
    ensure_client_id();
    if (!ensure_wifi_connected()) {
        return;
    }
    if (!ensure_server_selected()) {
        return;
    }

    M5.Speaker.stop();
    M5.Speaker.end();

    set_tracking_status("Servo", "Centering head", "Scan starts next");
    move_center_for_test();
    vTaskDelay(pdMS_TO_TICKS(650));

    const ScanPose scan_poses[kTrackingScanStepCount] = {
        {"Home", 0.0f, kTrackingHomePitchDeg},
        {"Yaw A", kTrackingScanYawDeg, kTrackingHomePitchDeg},
        {"Yaw B", -kTrackingScanYawDeg, kTrackingHomePitchDeg},
        {"Pitch Up", 0.0f, kTrackingHomePitchDeg + kTrackingScanPitchDeltaDeg},
        {"Pitch Down", 0.0f, kTrackingHomePitchDeg - kTrackingScanPitchDeltaDeg},
    };

    ScanCandidate best;
    for (int i = 0; i < kTrackingScanStepCount && !tracking_stop_requested; ++i) {
        ScanCandidate candidate;
        capture_face_at_pose(scan_poses[i], i + 1, kTrackingScanStepCount, &candidate);
        if (!candidate.found) {
            continue;
        }
        if (!best.found || candidate.pixel_error < best.pixel_error ||
            (candidate.pixel_error == best.pixel_error && candidate.face.area > best.face.area)) {
            best = candidate;
        }
    }

    if (tracking_stop_requested) {
        set_tracking_status("Stopped", "Tracking stopped", "", "", false, false);
        return;
    }

    if (!best.found) {
        set_tracking_status("No Face", "No face in scan photos", "Try standing closer", "", false, false);
        move_center_for_test();
        return;
    }

    float dx = best.face.center_x - kTrackingCx;
    float dy = best.face.center_y - kTrackingCy;
    float yaw_delta_deg = std::atan(dx / kTrackingFx) * 180.0f / kPi;
    float pitch_delta_deg = std::atan(dy / kTrackingFy) * 180.0f / kPi;
    float final_yaw = best.pose_yaw + yaw_delta_deg * kTrackingYawGain * kTrackingYawDirection;
    float final_pitch = best.pose_pitch + pitch_delta_deg * kTrackingPitchGain * kTrackingPitchDirection;

    char line1[80];
    char line2[80];
    snprintf(line1, sizeof(line1), "best %.0f,%.0f err %.0f", best.face.center_x, best.face.center_y, best.pixel_error);
    snprintf(line2, sizeof(line2), "move %.1f / %.1f", final_yaw, final_pitch);
    set_tracking_status("Facing", line1, line2, "");
    ESP_LOGI(TAG, "Scan best pose=(%.1f,%.1f) face=(%.1f,%.1f) dx=%.1f dy=%.1f final=(%.1f,%.1f)",
             best.pose_yaw, best.pose_pitch, best.face.center_x, best.face.center_y, dx, dy, final_yaw, final_pitch);
    if (!move_head_to_tracking_angles(final_yaw, final_pitch, 800)) {
        set_tracking_status("Servo Fail", "Final face move failed", "Check ID/wiring", "", false, false);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(900));

    FaceTarget verify_target;
    float verify_error = best.pixel_error;
    for (int round = 1; round <= kTrackingRefineRounds && !tracking_stop_requested; ++round) {
        char verify_line[48];
        snprintf(verify_line, sizeof(verify_line), "refine %d/%d", round, kTrackingRefineRounds);
        if (!capture_face_at_current_pose("Verify", verify_line, "Checking face center", &verify_target)) {
            set_tracking_status("Done", "Moved to best scan pose", "No final face detected", "", false, false);
            return;
        }

        float verify_dx = verify_target.center_x - kTrackingCx;
        float verify_dy = verify_target.center_y - kTrackingCy;
        verify_error = std::sqrt(verify_dx * verify_dx + verify_dy * verify_dy);
        ESP_LOGI(TAG, "Refine %d face=(%.1f,%.1f) dx=%.1f dy=%.1f err=%.1f pose=(%.1f,%.1f)", round,
                 verify_target.center_x, verify_target.center_y, verify_dx, verify_dy, verify_error,
                 tracking_yaw_deg, tracking_pitch_deg);
        if (verify_error <= kTrackingStopPixels || round == kTrackingRefineRounds) {
            snprintf(line1, sizeof(line1), "final err %.0fpx", verify_error);
            snprintf(line2, sizeof(line2), "face %.0f, %.0f", verify_target.center_x, verify_target.center_y);
            set_tracking_status(verify_error <= kTrackingStopPixels ? "Aligned" : "Done", line1, line2, "", false,
                                verify_error <= kTrackingStopPixels);
            return;
        }

        yaw_delta_deg = std::atan(verify_dx / kTrackingFx) * 180.0f / kPi;
        pitch_delta_deg = std::atan(verify_dy / kTrackingFy) * 180.0f / kPi;
        final_yaw = tracking_yaw_deg + yaw_delta_deg * kTrackingYawGain * kTrackingYawDirection;
        final_pitch = tracking_pitch_deg + pitch_delta_deg * kTrackingPitchGain * kTrackingPitchDirection;
        snprintf(line1, sizeof(line1), "face %.0f,%.0f err %.0f", verify_target.center_x, verify_target.center_y,
                 verify_error);
        snprintf(line2, sizeof(line2), "move %.1f / %.1f", final_yaw, final_pitch);
        set_tracking_status("Refining", line1, line2, "");
        if (!move_head_to_tracking_angles(final_yaw, final_pitch, 700)) {
            set_tracking_status("Servo Fail", "Refine move failed", "Check ID/wiring", "", false, false);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(850));
    }
    return;

#if 0
    for (int round = 1; round <= kTrackingMaxRounds && !tracking_stop_requested; ++round) {
        if (!init_camera_once()) {
            return;
        }

        char round_line[48];
        snprintf(round_line, sizeof(round_line), "round %d/%d", round, kTrackingMaxRounds);
        set_tracking_status("Capturing", round_line, "Detecting face via server", "");
        camera_fb_t* frame = get_fresh_camera_frame("tracking_loop");
        if (frame == nullptr) {
            set_tracking_status("Capture Fail", "esp_camera_fb_get failed", "", "", false, false);
            ESP_LOGE(TAG, "esp_camera_fb_get failed");
            release_camera_driver();
            return;
        }

        if (frame->format != PIXFORMAT_RGB565) {
            ESP_LOGW(TAG, "Unexpected camera pixel format: %d", static_cast<int>(frame->format));
        }

        FaceTarget target;
        bool detected = upload_tracking_frame(frame, &target);
        esp_camera_fb_return(frame);
        release_camera_driver();
        if (!detected) {
            return;
        }

        float dx = target.center_x - kTrackingCx;
        float dy = target.center_y - kTrackingCy;
        float pixel_error = std::sqrt(dx * dx + dy * dy);
        float yaw_delta_deg = std::atan(dx / kTrackingFx) * 180.0f / kPi;
        float pitch_delta_deg = std::atan(dy / kTrackingFy) * 180.0f / kPi;
        float new_yaw = tracking_yaw_deg + yaw_delta_deg * kTrackingGain * kTrackingYawDirection;
        float new_pitch = tracking_pitch_deg + pitch_delta_deg * kTrackingGain * kTrackingPitchDirection;

        char line1[80];
        char line2[80];
        snprintf(line1, sizeof(line1), "err %.0fpx yaw %+0.1f", pixel_error, yaw_delta_deg);
        snprintf(line2, sizeof(line2), "pitch %+0.1f -> %.1f/%.1f", pitch_delta_deg, new_yaw, new_pitch);
        set_tracking_status("Moving", line1, line2, "");
        ESP_LOGI(TAG, "Tracking round=%d face=(%.1f,%.1f) dx=%.1f dy=%.1f yaw_delta=%.2f pitch_delta=%.2f",
                 round, target.center_x, target.center_y, dx, dy, yaw_delta_deg, pitch_delta_deg);

        if (pixel_error <= kTrackingStopPixels) {
            set_tracking_status("Aligned", line1, "Face is near center", "", false, true);
            return;
        }

        if (!move_head_to_tracking_angles(new_yaw, new_pitch, 500)) {
            set_tracking_status("Servo Fail", "SCS0009 write failed", "Check servo power/wiring", "", false, false);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(750));
    }

    char final_line[64];
    snprintf(final_line, sizeof(final_line), "yaw %.1f pitch %.1f", tracking_yaw_deg, tracking_pitch_deg);
    set_tracking_status("Done", final_line, "Tap again to refine", "", false, true);
#endif
}
