namespace {

enum class AppId {
    Launcher,
    WifiConnect,
    VoiceDemo,
    StreamTtsDemo,
    CameraUpload,
    TrackingUser,
};

static constexpr const char* TAG = "Xiaopai";
static constexpr int kWifiConnectedBit = BIT0;
static constexpr int kWifiFailedBit = BIT1;
static constexpr int kHttpBufferSize = 4096;
static constexpr int kAudioUploadTimeoutMs = 60000;
static constexpr int kAudioSampleRate = 16000;
static constexpr int kOpusFrameDurationMs = 60;
static constexpr int kOpusFrameSamples = kAudioSampleRate * kOpusFrameDurationMs / 1000;
static constexpr int kRecordSampleRate = CONFIG_STACKCHAN_RECORD_SAMPLE_RATE;
static constexpr int kRecordChunkMs = CONFIG_STACKCHAN_RECORD_CHUNK_MS;
static constexpr int kVoiceProbeSamples = kRecordSampleRate * kRecordChunkMs / 1000;
static constexpr int kPreRollMs = CONFIG_STACKCHAN_PREROLL_MS;
static constexpr int kPreRollSamples = kRecordSampleRate * kPreRollMs / 1000;
static constexpr int kVoiceStartThreshold = CONFIG_STACKCHAN_VOICE_START_THRESHOLD;
static constexpr int kVoiceStopThreshold = CONFIG_STACKCHAN_VOICE_STOP_THRESHOLD;
static constexpr int kRecordMaxMs = CONFIG_STACKCHAN_RECORD_MAX_MS;
static constexpr int kSilenceStopMs = CONFIG_STACKCHAN_SILENCE_STOP_MS;
static constexpr int kRealtimeSttDrainTimeoutMs = 1800;
static constexpr int kTtsStreamSampleRate = 16000;
static constexpr size_t kTtsStreamBufferSamples = 2048;
static constexpr int kSpeakerDmaTailDrainMs = 384;
static constexpr int kPostSpeechEchoGuardMs = 0;
static constexpr int kWakeFindOwnerServerGraceMs = 1200;
static constexpr int kWakeFindOwnerDuplicateGuardMs = 5000;
static constexpr int kWakeFindOwnerPendingMaxMs = 8000;
static constexpr int kLauncherAppCount = 5;
static constexpr bool kShowAppStatusScreens = false;
static constexpr uint32_t kWifiTaskStackBytes = 8 * 1024;
static constexpr uint32_t kApp1TaskStackBytes = 40 * 1024;
static constexpr uint32_t kApp2TaskStackBytes = 16 * 1024;
static constexpr uint32_t kCommandTaskStackBytes = 16 * 1024;
static constexpr uint32_t kCameraTaskStackBytes = 16 * 1024;
static constexpr uint32_t kTrackingTaskStackBytes = 20 * 1024;
static constexpr size_t kTtsMaxBytes = CONFIG_STACKCHAN_TTS_MAX_BYTES;
static constexpr int kCameraWidth = 320;
static constexpr int kCameraHeight = 240;
static constexpr int kCameraFreshDiscardFrames = 2;
static constexpr float kTrackingCx = 160.0f;
static constexpr float kTrackingCy = 120.0f;
static constexpr float kTrackingFx = 364.0f;
static constexpr float kTrackingFy = 364.0f;
static constexpr float kTrackingYawGain = 0.75f;
static constexpr float kTrackingPitchGain = 0.90f;
static constexpr float kTrackingYawDirection = 1.0f;
static constexpr float kTrackingPitchDirection = -1.0f;
static constexpr float kFindOwnerYawGain = 0.45f;
static constexpr float kFindOwnerPitchGain = 0.55f;
static constexpr float kFindOwnerYawDirection = 1.0f;
static constexpr float kFindOwnerPitchDirection = -1.0f;
static constexpr float kFindOwnerStopPixels = 32.0f;
static constexpr int kTrackingScanStepCount = 5;
static constexpr int kTrackingRefineRounds = 3;
static constexpr float kTrackingStopPixels = 16.0f;
static constexpr float kTrackingYawMinDeg = -180.0f;
static constexpr float kTrackingYawMaxDeg = 180.0f;
static constexpr float kTrackingPitchMinDeg = 0.0f;
static constexpr float kTrackingPitchMaxDeg = 90.0f;
static constexpr float kTrackingHomePitchDeg = 45.0f;
static constexpr float kTrackingScanYawDeg = 25.0f;
static constexpr float kTrackingScanPitchDeltaDeg = 20.0f;
static constexpr int kFindOwnerMaxRounds = 3;
static constexpr int kVisionUploadTimeoutMs = 6000;
static constexpr int kCommandLongPollSeconds = 5;
static constexpr int kCommandLongPollHttpTimeoutMs = (kCommandLongPollSeconds + 3) * 1000;
static constexpr bool kUseHttpCommandLongPoll = false;
static constexpr float kPi = 3.14159265358979323846f;
static constexpr uart_port_t kServoUart = UART_NUM_1;
static constexpr int kServoTxPin = 6;
static constexpr int kServoRxPin = 7;
static constexpr int kServoBaud = 1000000;
static constexpr int kServoPanId = 1;
static constexpr int kServoTiltId = 2;
static constexpr int kServoYawZeroRaw = 460;
static constexpr int kServoPitchZeroRaw = 620;
static constexpr float kServoStepsPerDegree = 3.2f;
static constexpr uint8_t kPy32Address = 0x6f;
static constexpr uint8_t kPy32VersionReg = 0x02;
static constexpr uint8_t kPy32ServoPowerPin = 0;
static constexpr uint8_t kPy32RgbPin = 13;
static constexpr uint8_t kPy32LedConfigReg = 0x24;
static constexpr uint8_t kPy32LedRamStartReg = 0x30;
static constexpr uint8_t kLightStripLedCount = 12;
static constexpr uint8_t kLightStripSideLedCount = 6;
static constexpr bool kLightStripReverseSecondSide = true;
static constexpr uint8_t kLightStripListeningR = 0;
static constexpr uint8_t kLightStripListeningG = 32;
static constexpr uint8_t kLightStripListeningB = 0;
static constexpr uint8_t kLightStripSpeakingR = 0;
static constexpr uint8_t kLightStripSpeakingG = 0;
static constexpr uint8_t kLightStripSpeakingB = 48;
static constexpr uint32_t kListeningLightFrameMs = 60;
static constexpr int kListeningLightSilenceLevel = 120;
static constexpr int kListeningLightFullScaleLevel = 3600;
static constexpr uint32_t kIdleSleepDarkDelayMs = 500;
static constexpr uint32_t kSpeakingLightFrameMs = 60;
static constexpr int kSpeakingLightSilenceLevel = 180;
static constexpr int kSpeakingLightFullScaleLevel = 5200;
static constexpr uint32_t kPy32I2cFreq = 100000;
static constexpr uint8_t kSi12tAddress = 0x68;
static constexpr uint8_t kSi12tSensitivity1Reg = 0x02;
static constexpr uint8_t kSi12tCtrl1Reg = 0x08;
static constexpr uint8_t kSi12tCtrl2Reg = 0x09;
static constexpr uint8_t kSi12tRefRst1Reg = 0x0a;
static constexpr uint8_t kSi12tRefRst2Reg = 0x0b;
static constexpr uint8_t kSi12tChHold1Reg = 0x0c;
static constexpr uint8_t kSi12tChHold2Reg = 0x0d;
static constexpr uint8_t kSi12tCalHold1Reg = 0x0e;
static constexpr uint8_t kSi12tCalHold2Reg = 0x0f;
static constexpr uint8_t kSi12tOutput1Reg = 0x10;
static constexpr uint32_t kSi12tI2cFreq = 100000;
static constexpr uint32_t kHeadTouchPollMs = 50;
static constexpr uint32_t kHeadTouchClickMaxMs = 650;
static constexpr int16_t kHeadTouchSwipeThreshold = 40;
static constexpr uint32_t kHeadTouchTaskStackBytes = 6 * 1024;
static constexpr uint32_t kHeadTouchAudioTaskStackBytes = 16 * 1024;
static constexpr uint32_t kSpeakCommandTaskStackBytes = 16 * 1024;
static constexpr size_t kSpeakCommandMaxTextBytes = 512;
static constexpr size_t kSpeakCommandCacheNameBytes = 64;
static constexpr size_t kSpeakCommandVoiceBytes = 64;
static constexpr int kSpeakerVolumePercentMin = 10;
static constexpr int kSpeakerVolumePercentMax = 100;
static constexpr int kSpeakerVolumeDefaultStep = 10;

static void configure_speaker_for_tts()
{
    auto spk_cfg = M5.Speaker.config();
    spk_cfg.sample_rate = kTtsStreamSampleRate;
    spk_cfg.dma_buf_len = 512;
    spk_cfg.dma_buf_count = 12;
    spk_cfg.task_priority = 4;
    spk_cfg.task_pinned_core = 0;
    M5.Speaker.config(spk_cfg);
}

AppId current_app = AppId::Launcher;
int selected_menu = 0;
TaskHandle_t wifi_task_handle = nullptr;
TaskHandle_t xiaozhi_task_handle = nullptr;
TaskHandle_t stream_tts_task_handle = nullptr;
TaskHandle_t camera_upload_task_handle = nullptr;
TaskHandle_t tracking_task_handle = nullptr;
TaskHandle_t command_task_handle = nullptr;
TaskHandle_t boot_task_handle = nullptr;
TaskHandle_t speaking_light_task_handle = nullptr;
TaskHandle_t head_touch_task_handle = nullptr;
TaskHandle_t head_touch_audio_task_handle = nullptr;
TaskHandle_t speak_command_task_handle = nullptr;
EventGroupHandle_t wifi_event_group = nullptr;
SemaphoreHandle_t m5_mutex = nullptr;
SemaphoreHandle_t audio_mutex = nullptr;
QueueHandle_t head_touch_event_queue = nullptr;
QueueHandle_t speak_command_queue = nullptr;
int wifi_retry_count = 0;
bool wifi_started = false;
bool wifi_connect_requested = false;
volatile bool wifi_manual_switching = false;
volatile bool app1_stop_requested = false;
volatile bool app2_stop_requested = false;
volatile bool tracking_stop_requested = false;
volatile bool voice_listener_paused = false;
volatile bool voice_recording_active = false;
volatile int voice_listener_pause_count = 0;
volatile bool voice_status_screen_suppressed = false;
volatile bool speech_playback_active = false;
volatile bool realtime_tts_playback_active = false;
volatile uint32_t listening_light_last_update_ms = 0;
volatile bool speaking_light_running = false;
volatile uint8_t speaking_light_level = 0;
volatile uint32_t speaking_light_level_ms = 0;
volatile bool speak_command_active = false;
volatile uint32_t speech_output_finished_ms = 0;
volatile bool wake_find_owner_pending = false;
volatile uint32_t wake_find_owner_requested_ms = 0;
volatile uint32_t wake_find_owner_last_run_ms = 0;
bool light_strip_ready = false;
bool light_strip_missing = false;
bool light_strip_listening_after_speech = false;
uint32_t light_strip_last_probe_ms = 0;
int light_strip_state = 0;
bool camera_initialized = false;
volatile bool camera_owns_internal_i2c = false;
bool servo_uart_initialized = false;
bool servo_power_ready = false;
float tracking_yaw_deg = 0.0f;
float tracking_pitch_deg = kTrackingHomePitchDeg;
int speaker_volume_percent =
    std::max(kSpeakerVolumePercentMin, std::min(kSpeakerVolumePercentMax, (CONFIG_STACKCHAN_TTS_VOLUME * 100 + 127) / 255));
char client_id[37] = {};
std::string active_wifi_ssid = CONFIG_STACKCHAN_WIFI_SSID;
std::string active_server_base = "http://192.168.21.15:8091";
bool active_server_selected = false;
int active_wifi_candidate_index = -1;
static constexpr int kWifiRetryLimit = 1;
static constexpr const char* kWifiNvsNamespace = "xiaopai";
static constexpr const char* kWifiNvsSsidKey = "wifi_ssid";
static constexpr const char* kWifiNvsPasswordKey = "wifi_password";
static constexpr const char* kServerNvsBaseKey = "server_base";
static constexpr const char* kProvisioningApPassword = "12345678";
static constexpr int kProvisioningMaxScanResults = 16;
static constexpr uint32_t kProvisioningStopDelayMs = 700;
static constexpr uint32_t kProvisioningStopTaskStackBytes = 4 * 1024;
bool wifi_sta_netif_created = false;
bool wifi_ap_netif_created = false;
bool provisioning_started = false;
bool provisioning_stop_pending = false;
httpd_handle_t provisioning_httpd = nullptr;

enum class HeadTouchEvent : uint8_t {
    Press,
    Click,
    SwipeForward,
    SwipeBackward,
};

struct SpeakCommandItem {
    char cmd_id[40];
    char text[kSpeakCommandMaxTextBytes];
    char cache_name[kSpeakCommandCacheNameBytes];
    char voice[kSpeakCommandVoiceBytes];
    int sample_rate;
    int volume;
    int speech_rate;
    int pitch_rate;
    bool pause_voice_listener;
};

struct WifiCandidate {
    const char* ssid;
    const char* password;
};

static constexpr WifiCandidate kWifiCandidates[] = {
    {CONFIG_STACKCHAN_WIFI_SSID, CONFIG_STACKCHAN_WIFI_PASSWORD},
    {"MYENDLESS", "88888888"},
};
static constexpr int kWifiCandidateCount = sizeof(kWifiCandidates) / sizeof(kWifiCandidates[0]);

static constexpr const char* kServerBaseCandidates[] = {
    "http://1.14.134.217:8091/",
    "http://192.168.21.15:8091",
    "http://172.24.77.83:8091",
    "http://192.168.137.1:8091",
};

struct XiaozhiConfig {
    std::string websocket_url;
    std::string websocket_token;
    std::string session_id;
};

XiaozhiConfig xiaozhi_config;
void* realtime_opus_decoder = nullptr;
bool realtime_tts_audio_mutex_taken = false;

static bool speech_output_is_busy()
{
    if (speech_playback_active || realtime_tts_playback_active || speak_command_active ||
        speaking_animation_is_running() || M5.Speaker.isPlaying()) {
        return true;
    }
    if (audio_mutex == nullptr) {
        return false;
    }
    if (xSemaphoreTake(audio_mutex, 0) == pdTRUE) {
        xSemaphoreGive(audio_mutex);
        return false;
    }
    return true;
}

static void mark_speech_output_finished()
{
    speech_output_finished_ms = M5.millis();
}

static bool post_speech_echo_guard_active()
{
    if (kPostSpeechEchoGuardMs <= 0) {
        return false;
    }
    uint32_t finished_ms = speech_output_finished_ms;
    return finished_ms != 0 && static_cast<uint32_t>(M5.millis() - finished_ms) < kPostSpeechEchoGuardMs;
}

static void mark_wake_find_owner_pending(const char* reason)
{
    uint32_t now = M5.millis();
    if (wake_find_owner_last_run_ms != 0 &&
        static_cast<uint32_t>(now - wake_find_owner_last_run_ms) < kWakeFindOwnerDuplicateGuardMs) {
        ESP_LOGI(TAG, "Wake find-owner suppressed by duplicate guard reason=%s",
                 reason != nullptr ? reason : "");
        return;
    }

    wake_find_owner_pending = true;
    wake_find_owner_requested_ms = now;
    ESP_LOGI(TAG, "Wake find-owner pending reason=%s", reason != nullptr ? reason : "");
}

static bool should_restore_listening_light_after_speech()
{
    return light_strip_listening_after_speech;
}

static void handle_pending_wake_find_owner()
{
    if (!wake_find_owner_pending) {
        return;
    }
    uint32_t now = M5.millis();
    if (static_cast<uint32_t>(now - wake_find_owner_requested_ms) > kWakeFindOwnerPendingMaxMs) {
        ESP_LOGI(TAG, "Wake find-owner pending expired");
        wake_find_owner_pending = false;
        return;
    }
    if (static_cast<uint32_t>(now - wake_find_owner_requested_ms) < kWakeFindOwnerServerGraceMs) {
        return;
    }
    if (local_voice_current_state() != LocalVoiceState::Listening || voice_listener_paused || speech_output_is_busy()) {
        return;
    }

    wake_find_owner_pending = false;
    wake_find_owner_last_run_ms = now;
    ESP_LOGI(TAG, "Wake find-owner running locally");
    run_find_owner_command(1, "", kFindOwnerYawGain, kFindOwnerPitchGain, kFindOwnerStopPixels, true, false);
}

struct App1Status {
    char stage[32] = "Idle";
    char line1[96] = "Ready";
    char line2[96] = "";
    char line3[96] = "";
    bool running = false;
    bool success = false;
};

App1Status app1_status;
App1Status wifi_status;
App1Status tts_status;
App1Status camera_status;
App1Status tracking_status;

class M5Lock {
public:
    M5Lock()
    {
        if (m5_mutex != nullptr) {
            xSemaphoreTake(m5_mutex, portMAX_DELAY);
            locked_ = true;
        }
    }
    ~M5Lock()
    {
        if (locked_) {
            xSemaphoreGive(m5_mutex);
        }
    }

private:
    bool locked_ = false;
};

void draw_header(const char* title)
{
    mark_expression_screen_dirty();
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_left);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setFont(&fonts::Font4);
    display.setTextSize(1);
    display.drawString(title, 12, 10);
    display.drawFastHLine(0, 42, display.width(), TFT_DARKGREY);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.drawString("BtnA / top-left: back", 12, display.height() - 18);
}

void draw_launcher()
{
    if (!kShowAppStatusScreens) {
        if (!expression_screen_is_visible()) {
            show_expression(kDefaultExpression);
        }
        return;
    }
    mark_expression_screen_dirty();
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_left);

    const int card_w = display.width() - 24;
    const int card_h = 36;
    const int first_y = 20;
    const char* titles[] = {
        "WiFi Connect",
        "Voice to Text",
        "Aliyun PCM TTS",
        "Camera Upload",
        "Tracking User",
    };
    const char* subtitles[] = {
        "Connect once before network apps",
        "Record voice and upload WAV by HTTP",
        "Stream text-to-speech as raw PCM",
        "Take one photo and upload it",
        "Face detect and turn toward user",
    };
    const bool wifi_connected = wifi_is_connected();

    for (int i = 0; i < kLauncherAppCount; ++i) {
        const int y = first_y + i * (card_h + 5);
        const uint16_t border = i == selected_menu ? TFT_CYAN : TFT_DARKGREY;
        display.drawRoundRect(12, y, card_w, card_h, 6, border);
        display.setTextColor(i == selected_menu ? TFT_CYAN : TFT_WHITE, TFT_BLACK);
        display.setFont(&fonts::Font2);
        display.drawString(titles[i], 24, y + 4);
        display.setFont(&fonts::Font2);
        if (i == 0 && wifi_connected) {
            display.setTextColor(TFT_GREEN, TFT_BLACK);
            display.drawString("Connected", 24, y + 21);
            display.setTextColor(TFT_DARKGREY, TFT_BLACK);
            display.drawString(active_wifi_ssid.c_str(), 104, y + 21);
        } else {
            display.setTextColor(TFT_DARKGREY, TFT_BLACK);
            display.drawString(subtitles[i], 24, y + 21);
        }
    }

    display.setTextDatum(bottom_center);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.setFont(&fonts::Font2);
    display.drawString("Tap an app, or BtnA select / BtnB enter", display.width() / 2, display.height() - 8);
}

void draw_wifi_status()
{
    auto& display = M5.Display;
    draw_header("WiFi Connect");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(wifi_status.success ? TFT_GREEN : (wifi_status.running ? TFT_CYAN : TFT_ORANGE), TFT_BLACK);
    display.drawString(wifi_status.stage, display.width() / 2, display.height() / 2 - 46);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(wifi_status.line1, display.width() / 2, display.height() / 2 - 2);
    display.drawString(wifi_status.line2, display.width() / 2, display.height() / 2 + 24);
    display.drawString(wifi_status.line3, display.width() / 2, display.height() / 2 + 50);

    if (!wifi_status.running && !wifi_status.success) {
        display.setTextColor(TFT_CYAN, TFT_BLACK);
        display.drawString("Tap screen or BtnB", display.width() / 2, display.height() / 2 + 80);
    }
}

void draw_wifi_setup_steps(const char* ap_ssid)
{
    mark_expression_screen_dirty();
    auto& display = M5.Display;
    display.fillScreen(TFT_BLACK);
    display.setTextDatum(top_left);

    display.setFont(&fonts::Font4);
    display.setTextSize(1);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.drawString("Setup", 18, 18);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("1. Connect to AP", 18, 58);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.drawString(ap_ssid ? ap_ssid : "", 38, 84);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("Passwd", 38, 112);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.drawString(kProvisioningApPassword, 104, 112);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("2. Open Browser", 18, 150);
    display.setTextColor(TFT_GREEN, TFT_BLACK);
    display.drawString("192.168.4.1", 38, 176);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString("3. Select WiFi & Server", 18, 214);
}

void draw_app1_status()
{
    if (!kShowAppStatusScreens) {
        return;
    }
    auto& display = M5.Display;
    draw_header("Voice to Text");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(app1_status.success ? TFT_GREEN : (app1_status.running ? TFT_CYAN : TFT_ORANGE), TFT_BLACK);
    display.drawString(app1_status.stage, display.width() / 2, display.height() / 2 - 46);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(app1_status.line1, display.width() / 2, display.height() / 2 - 2);
    display.drawString(app1_status.line2, display.width() / 2, display.height() / 2 + 24);
    display.drawString(app1_status.line3, display.width() / 2, display.height() / 2 + 50);
}

void draw_tts_status()
{
    if (!kShowAppStatusScreens) {
        return;
    }
    auto& display = M5.Display;
    draw_header("Aliyun PCM TTS");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(tts_status.success ? TFT_GREEN : (tts_status.running ? TFT_CYAN : TFT_ORANGE), TFT_BLACK);
    display.drawString(tts_status.stage, display.width() / 2, display.height() / 2 - 46);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(tts_status.line1, display.width() / 2, display.height() / 2 - 2);
    display.drawString(tts_status.line2, display.width() / 2, display.height() / 2 + 24);
    display.drawString(tts_status.line3, display.width() / 2, display.height() / 2 + 50);
}

void draw_camera_status()
{
    if (!kShowAppStatusScreens) {
        return;
    }
    auto& display = M5.Display;
    draw_header("Camera Upload");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(camera_status.success ? TFT_GREEN : (camera_status.running ? TFT_CYAN : TFT_ORANGE), TFT_BLACK);
    display.drawString(camera_status.stage, display.width() / 2, display.height() / 2 - 46);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(camera_status.line1, display.width() / 2, display.height() / 2 - 2);
    display.drawString(camera_status.line2, display.width() / 2, display.height() / 2 + 24);
    display.drawString(camera_status.line3, display.width() / 2, display.height() / 2 + 50);
}

void draw_tracking_status()
{
    if (!kShowAppStatusScreens) {
        return;
    }
    auto& display = M5.Display;
    draw_header("Tracking User");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(tracking_status.success ? TFT_GREEN : (tracking_status.running ? TFT_CYAN : TFT_ORANGE), TFT_BLACK);
    display.drawString(tracking_status.stage, display.width() / 2, display.height() / 2 - 46);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.drawString(tracking_status.line1, display.width() / 2, display.height() / 2 - 2);
    display.drawString(tracking_status.line2, display.width() / 2, display.height() / 2 + 24);
    display.drawString(tracking_status.line3, display.width() / 2, display.height() / 2 + 50);
}

void show_recognition_text(const char* title, const char* text)
{
    if (!kShowAppStatusScreens) {
        return;
    }
    auto& display = M5.Display;
    draw_header(title);

    display.setTextDatum(top_left);
    display.setTextColor(TFT_CYAN, TFT_BLACK);
    display.setFont(&fonts::Font4);
    display.drawString("Recognized:", 12, 58);

    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setFont(&fonts::Font4);
    display.setTextSize(1);
    display.setCursor(12, 94);
    display.setTextWrap(true);
    display.print(text && text[0] ? text : "(empty)");

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    display.drawString("Listening will continue automatically", 12, display.height() - 42);
}

void set_app1_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                     bool running = true, bool success = false)
{
    snprintf(app1_status.stage, sizeof(app1_status.stage), "%s", stage);
    snprintf(app1_status.line1, sizeof(app1_status.line1), "%s", line1);
    snprintf(app1_status.line2, sizeof(app1_status.line2), "%s", line2);
    snprintf(app1_status.line3, sizeof(app1_status.line3), "%s", line3);
    app1_status.running = running;
    app1_status.success = success;
    ESP_LOGI(TAG, "APP1 status: %s | %s | %s | %s", app1_status.stage, app1_status.line1, app1_status.line2,
             app1_status.line3);
    if (voice_status_screen_suppressed || !kShowAppStatusScreens) {
        return;
    }
    M5Lock lock;
    draw_app1_status();
}

void set_wifi_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                     bool running = true, bool success = false)
{
    snprintf(wifi_status.stage, sizeof(wifi_status.stage), "%s", stage);
    snprintf(wifi_status.line1, sizeof(wifi_status.line1), "%s", line1);
    snprintf(wifi_status.line2, sizeof(wifi_status.line2), "%s", line2);
    snprintf(wifi_status.line3, sizeof(wifi_status.line3), "%s", line3);
    wifi_status.running = running;
    wifi_status.success = success;
    ESP_LOGI(TAG, "APP0 status: %s | %s | %s | %s", wifi_status.stage, wifi_status.line1, wifi_status.line2,
             wifi_status.line3);
    M5Lock lock;
    draw_wifi_status();
}

void set_tts_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                    bool running = true, bool success = false)
{
    snprintf(tts_status.stage, sizeof(tts_status.stage), "%s", stage);
    snprintf(tts_status.line1, sizeof(tts_status.line1), "%s", line1);
    snprintf(tts_status.line2, sizeof(tts_status.line2), "%s", line2);
    snprintf(tts_status.line3, sizeof(tts_status.line3), "%s", line3);
    tts_status.running = running;
    tts_status.success = success;
    ESP_LOGI(TAG, "TTS status: %s | %s | %s | %s", tts_status.stage, tts_status.line1, tts_status.line2,
             tts_status.line3);
    if (voice_status_screen_suppressed || !kShowAppStatusScreens) {
        return;
    }
    M5Lock lock;
    draw_tts_status();
}

void set_camera_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                       bool running = true, bool success = false)
{
    snprintf(camera_status.stage, sizeof(camera_status.stage), "%s", stage);
    snprintf(camera_status.line1, sizeof(camera_status.line1), "%s", line1);
    snprintf(camera_status.line2, sizeof(camera_status.line2), "%s", line2);
    snprintf(camera_status.line3, sizeof(camera_status.line3), "%s", line3);
    camera_status.running = running;
    camera_status.success = success;
    ESP_LOGI(TAG, "Camera status: %s | %s | %s | %s", camera_status.stage, camera_status.line1, camera_status.line2,
             camera_status.line3);
    if (!kShowAppStatusScreens) {
        return;
    }
    M5Lock lock;
    draw_camera_status();
}

void set_tracking_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                       bool running = true, bool success = false)
{
    snprintf(tracking_status.stage, sizeof(tracking_status.stage), "%s", stage);
    snprintf(tracking_status.line1, sizeof(tracking_status.line1), "%s", line1);
    snprintf(tracking_status.line2, sizeof(tracking_status.line2), "%s", line2);
    snprintf(tracking_status.line3, sizeof(tracking_status.line3), "%s", line3);
    tracking_status.running = running;
    tracking_status.success = success;
    ESP_LOGI(TAG, "Tracking status: %s | %s | %s | %s", tracking_status.stage, tracking_status.line1, tracking_status.line2,
             tracking_status.line3);
    if (!kShowAppStatusScreens) {
        return;
    }
    M5Lock lock;
    draw_tracking_status();
}

void set_current_network_status(const char* stage, const char* line1, const char* line2 = "", const char* line3 = "",
                                bool running = true, bool success = false)
{
    if (current_app == AppId::WifiConnect) {
        set_wifi_status(stage, line1, line2, line3, running, success);
    } else if (current_app == AppId::StreamTtsDemo) {
        set_tts_status(stage, line1, line2, line3, running, success);
    } else if (current_app == AppId::CameraUpload) {
        set_camera_status(stage, line1, line2, line3, running, success);
    } else if (current_app == AppId::TrackingUser) {
        set_tracking_status(stage, line1, line2, line3, running, success);
    } else {
        set_app1_status(stage, line1, line2, line3, running, success);
    }
}

void start_wifi_connect_task()
{
    if (wifi_task_handle == nullptr) {
        xTaskCreatePinnedToCore([](void*) {
            ::run_wifi_connect_app();
            wifi_task_handle = nullptr;
            vTaskDelete(nullptr);
        }, "app0_wifi", kWifiTaskStackBytes, nullptr, 3, &wifi_task_handle, 1);
    } else {
        ESP_LOGI(TAG, "WiFi setup task is already running");
    }
}

const char* touch_state_name(const m5::Touch_Class::touch_detail_t& touch)
{
    if (touch.wasPressed()) {
        return "Pressed";
    }
    if (touch.wasClicked()) {
        return "Clicked / Released";
    }
    if (touch.wasHold()) {
        return "Hold Start";
    }
    if (touch.isHolding()) {
        return "Holding";
    }
    if (touch.wasFlickStart()) {
        return "Flick Start";
    }
    if (touch.isFlicking()) {
        return "Flicking";
    }
    if (touch.wasFlicked()) {
        return "Flick End";
    }
    if (touch.wasDragStart()) {
        return "Drag Start";
    }
    if (touch.isDragging()) {
        return "Dragging";
    }
    if (touch.wasDragged()) {
        return "Drag End";
    }
    if (touch.isPressed()) {
        return "Touching";
    }
    if (touch.wasReleased()) {
        return "Released";
    }
    return "Idle";
}

void draw_touch_status(const char* event_name, const m5::Touch_Class::touch_detail_t& touch)
{
    if (!kShowAppStatusScreens) {
        if (!expression_screen_is_visible()) {
            show_expression(kDefaultExpression);
        }
        return;
    }
    auto& display = M5.Display;
    draw_header("APP2 Touch Events");

    display.setTextDatum(middle_center);
    display.setFont(&fonts::FreeSansBoldOblique24pt7b);
    display.setTextSize(1);
    display.setTextColor(strcmp(event_name, "Idle") == 0 ? TFT_DARKGREY : TFT_ORANGE, TFT_BLACK);
    display.drawString(event_name, display.width() / 2, display.height() / 2 - 24);

    char line[96];
    display.setFont(&fonts::Font4);
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    snprintf(line, sizeof(line), "x:%d y:%d", touch.x, touch.y);
    display.drawString(line, display.width() / 2, display.height() / 2 + 22);

    display.setFont(&fonts::Font2);
    display.setTextColor(TFT_DARKGREY, TFT_BLACK);
    snprintf(line, sizeof(line), "dx:%d dy:%d clicks:%u", touch.deltaX(), touch.deltaY(), touch.getClickCount());
    display.drawString(line, display.width() / 2, display.height() / 2 + 48);
}

void enter_app(AppId app)
{
    if (current_app == AppId::VoiceDemo && app != AppId::VoiceDemo) {
        while (M5.Mic.isRecording()) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        M5.Mic.end();
    }
    if (current_app == AppId::StreamTtsDemo && app != AppId::StreamTtsDemo) {
        app2_stop_requested = true;
        M5.Speaker.stop();
    }
    if (current_app == AppId::TrackingUser && app != AppId::TrackingUser) {
        tracking_stop_requested = true;
    }

    current_app = app;

    if (app == AppId::Launcher) {
        draw_launcher();
        return;
    }

    if (app == AppId::WifiConnect) {
        start_wifi_connect_task();
        return;
    }

    if (app == AppId::VoiceDemo) {
        app1_stop_requested = false;
        set_app1_status("Starting", "Local recording upload");
        if (xiaozhi_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                ::run_xiaozhi_ota_probe();
                xiaozhi_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "app1_voice", kApp1TaskStackBytes, nullptr, 3, &xiaozhi_task_handle, 1);
        } else {
            set_app1_status("Running", "Probe task is already running");
        }
        return;
    }

    if (app == AppId::StreamTtsDemo) {
        app2_stop_requested = false;
        set_tts_status("Starting", "Aliyun PCM streaming TTS");
        if (stream_tts_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                ::run_stream_tts_demo();
                stream_tts_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "app2_pcm_tts", kApp2TaskStackBytes, nullptr, 3, &stream_tts_task_handle, 1);
        } else {
            set_tts_status("Running", "Stream TTS task is already running");
        }
        return;
    }

    if (app == AppId::CameraUpload) {
        set_camera_status("Starting", "Taking one photo");
        if (camera_upload_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                ::run_camera_upload_app();
                camera_upload_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "app3_camera", kCameraTaskStackBytes, nullptr, 3, &camera_upload_task_handle, 1);
        } else {
            set_camera_status("Running", "Camera upload already running");
        }
        return;
    }

    if (app == AppId::TrackingUser) {
        tracking_stop_requested = false;
        set_tracking_status("Starting", "Taking photo");
        if (tracking_task_handle == nullptr) {
            xTaskCreatePinnedToCore([](void*) {
                ::run_tracking_user_demo();
                tracking_task_handle = nullptr;
                vTaskDelete(nullptr);
            }, "app3_tracking", kTrackingTaskStackBytes, nullptr, 3, &tracking_task_handle, 1);
        } else {
            set_tracking_status("Running", "Camera task is already running");
        }
        return;
    }

    draw_touch_status("Idle", M5.Touch.getDetail());
}

bool back_requested()
{
    if (M5.BtnA.wasClicked()) {
        return true;
    }

    auto touch = M5.Touch.getDetail();
    return touch.wasClicked() && touch.x < 72 && touch.y < 52;
}

void update_launcher()
{
    if (M5.BtnA.wasClicked()) {
        selected_menu = (selected_menu + 1) % kLauncherAppCount;
        draw_launcher();
        return;
    }

    if (M5.BtnB.wasClicked()) {
        const AppId apps[] = {AppId::WifiConnect, AppId::VoiceDemo, AppId::StreamTtsDemo, AppId::CameraUpload,
                              AppId::TrackingUser};
        enter_app(apps[selected_menu]);
        return;
    }

    auto touch = M5.Touch.getDetail();
    if (!touch.wasClicked()) {
        return;
    }

    const int first_y = 20;
    const int card_h = 36;
    const int gap = 5;
    const AppId apps[] = {AppId::WifiConnect, AppId::VoiceDemo, AppId::StreamTtsDemo, AppId::CameraUpload,
                          AppId::TrackingUser};
    for (int i = 0; i < kLauncherAppCount; ++i) {
        const int y = first_y + i * (card_h + gap);
        if (touch.y >= y && touch.y < y + card_h) {
            selected_menu = i;
            enter_app(apps[i]);
            return;
        }
    }
}

void update_wifi_connect()
{
    if (back_requested()) {
        enter_app(AppId::Launcher);
        return;
    }

    if (wifi_task_handle != nullptr || wifi_status.running || wifi_status.success) {
        return;
    }

    auto touch = M5.Touch.getDetail();
    if (M5.BtnB.wasClicked() || touch.wasClicked()) {
        start_wifi_connect_task();
    }
}

void update_voice_demo()
{
    if (back_requested()) {
        app1_stop_requested = true;
        enter_app(AppId::Launcher);
        return;
    }
}

void update_stream_tts_demo()
{
    if (back_requested()) {
        app2_stop_requested = true;
        M5.Speaker.stop();
        enter_app(AppId::Launcher);
        return;
    }
}

void update_camera_upload()
{
    if (back_requested()) {
        enter_app(AppId::Launcher);
        return;
    }
}

void update_tracking_user()
{
    if (back_requested()) {
        tracking_stop_requested = true;
        enter_app(AppId::Launcher);
        return;
    }
}

}  // namespace
