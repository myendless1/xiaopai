#include "expression_controller.h"

#include <M5Unified.h>

#include "freertos/task.h"

#include <algorithm>
#include <string.h>

extern const uint8_t calm_face_png_start[] asm("_binary_calm_face_png_start");
extern const uint8_t calm_face_png_end[] asm("_binary_calm_face_png_end");
extern const uint8_t sleep_dark_face_png_start[] asm("_binary_sleep_dark_face_png_start");
extern const uint8_t sleep_dark_face_png_end[] asm("_binary_sleep_dark_face_png_end");
extern const uint8_t speak1_face_png_start[] asm("_binary_speak1_face_png_start");
extern const uint8_t speak1_face_png_end[] asm("_binary_speak1_face_png_end");
extern const uint8_t speak2_face_png_start[] asm("_binary_speak2_face_png_start");
extern const uint8_t speak2_face_png_end[] asm("_binary_speak2_face_png_end");
extern const uint8_t shy_face_png_start[] asm("_binary_shy_face_png_start");
extern const uint8_t shy_face_png_end[] asm("_binary_shy_face_png_end");
extern const uint8_t thinking_face_png_start[] asm("_binary_thinking_face_png_start");
extern const uint8_t thinking_face_png_end[] asm("_binary_thinking_face_png_end");
extern const uint8_t relaxed_face_png_start[] asm("_binary_relaxed_face_png_start");
extern const uint8_t relaxed_face_png_end[] asm("_binary_relaxed_face_png_end");
extern const uint8_t smile_blink_face_png_start[] asm("_binary_smile_blink_face_png_start");
extern const uint8_t smile_blink_face_png_end[] asm("_binary_smile_blink_face_png_end");
extern const uint8_t blink_half_face_png_start[] asm("_binary_blink_half_face_png_start");
extern const uint8_t blink_half_face_png_end[] asm("_binary_blink_half_face_png_end");
extern const uint8_t blink_closed_face_png_start[] asm("_binary_blink_closed_face_png_start");
extern const uint8_t blink_closed_face_png_end[] asm("_binary_blink_closed_face_png_end");
extern const uint8_t wink_half_face_png_start[] asm("_binary_wink_half_face_png_start");
extern const uint8_t wink_half_face_png_end[] asm("_binary_wink_half_face_png_end");
extern const uint8_t wink_closed_face_png_start[] asm("_binary_wink_closed_face_png_start");
extern const uint8_t wink_closed_face_png_end[] asm("_binary_wink_closed_face_png_end");
extern const uint8_t heart_small_face_png_start[] asm("_binary_heart_small_face_png_start");
extern const uint8_t heart_small_face_png_end[] asm("_binary_heart_small_face_png_end");
extern const uint8_t heart_face_png_start[] asm("_binary_heart_face_png_start");
extern const uint8_t heart_face_png_end[] asm("_binary_heart_face_png_end");
extern const uint8_t nod_soft_face_png_start[] asm("_binary_nod_soft_face_png_start");
extern const uint8_t nod_soft_face_png_end[] asm("_binary_nod_soft_face_png_end");
extern const uint8_t nod_down_face_png_start[] asm("_binary_nod_down_face_png_start");
extern const uint8_t nod_down_face_png_end[] asm("_binary_nod_down_face_png_end");
extern const uint8_t happy_squint_face_png_start[] asm("_binary_happy_squint_face_png_start");
extern const uint8_t happy_squint_face_png_end[] asm("_binary_happy_squint_face_png_end");
extern const uint8_t happy_squint_soft_face_png_start[] asm("_binary_happy_squint_soft_face_png_start");
extern const uint8_t happy_squint_soft_face_png_end[] asm("_binary_happy_squint_soft_face_png_end");

const char* const kDefaultExpression = "calm";

namespace {

struct ExpressionAsset {
    const char* name;
    const uint8_t* start;
    const uint8_t* end;
    int width;
    int height;
};

struct ExpressionAnimation {
    const char* name;
    const char* const* frames;
    size_t frame_count;
    int frame_ms;
};

static const ExpressionAsset kExpressionAssets[] = {
    {"calm", calm_face_png_start, calm_face_png_end, 320, 240},
    {"sleep_dark", sleep_dark_face_png_start, sleep_dark_face_png_end, 320, 240},
    {"speak1", speak1_face_png_start, speak1_face_png_end, 320, 240},
    {"speak2", speak2_face_png_start, speak2_face_png_end, 320, 240},
    {"shy", shy_face_png_start, shy_face_png_end, 320, 240},
    {"thinking", thinking_face_png_start, thinking_face_png_end, 320, 240},
    {"relaxed", relaxed_face_png_start, relaxed_face_png_end, 320, 240},
    {"smile_blink", smile_blink_face_png_start, smile_blink_face_png_end, 320, 240},
    {"blink_half", blink_half_face_png_start, blink_half_face_png_end, 320, 240},
    {"blink_closed", blink_closed_face_png_start, blink_closed_face_png_end, 320, 240},
    {"wink_half", wink_half_face_png_start, wink_half_face_png_end, 320, 240},
    {"wink_closed", wink_closed_face_png_start, wink_closed_face_png_end, 320, 240},
    {"heart_small", heart_small_face_png_start, heart_small_face_png_end, 320, 240},
    {"heart", heart_face_png_start, heart_face_png_end, 320, 240},
    {"nod_soft", nod_soft_face_png_start, nod_soft_face_png_end, 320, 240},
    {"nod_down", nod_down_face_png_start, nod_down_face_png_end, 320, 240},
    {"happy_squint", happy_squint_face_png_start, happy_squint_face_png_end, 320, 240},
    {"happy_squint_soft", happy_squint_soft_face_png_start, happy_squint_soft_face_png_end, 320, 240},
};

static constexpr const char* kBlinkFrames[] = {
    "calm",
    "blink_half",
    "blink_closed",
    "blink_half",
    "calm",
    "calm",
    "calm",
};
static constexpr const char* kWinkFrames[] = {
    "calm",
    "wink_half",
    "wink_closed",
    "wink_half",
    "calm",
    "wink_closed",
    "calm",
};
static constexpr const char* kHeartFrames[] = {
    "calm",
    "heart_small",
    "heart",
    "heart",
    "heart_small",
    "heart",
};
static constexpr const char* kNodFrames[] = {
    "calm",
    "nod_soft",
    "nod_down",
    "nod_soft",
    "calm",
};
static constexpr const char* kSpeakFrames[] = {
    "speak1",
    "speak2",
};
static constexpr const char* kHappyDynamicFrames[] = {
    "happy_squint_soft",
    "happy_squint",
    "happy_squint_soft",
    "happy_squint",
};
static constexpr ExpressionAnimation kExpressionAnimations[] = {
    {"blink", kBlinkFrames, sizeof(kBlinkFrames) / sizeof(kBlinkFrames[0]), 140},
    {"wink", kWinkFrames, sizeof(kWinkFrames) / sizeof(kWinkFrames[0]), 140},
    {"heart_action", kHeartFrames, sizeof(kHeartFrames) / sizeof(kHeartFrames[0]), 170},
    {"hearting", kHeartFrames, sizeof(kHeartFrames) / sizeof(kHeartFrames[0]), 170},
    {"nod", kNodFrames, sizeof(kNodFrames) / sizeof(kNodFrames[0]), 180},
    {"nodding", kNodFrames, sizeof(kNodFrames) / sizeof(kNodFrames[0]), 180},
    {"speak", kSpeakFrames, sizeof(kSpeakFrames) / sizeof(kSpeakFrames[0]), 160},
    {"speaking", kSpeakFrames, sizeof(kSpeakFrames) / sizeof(kSpeakFrames[0]), 160},
    {"happy_dynamic", kHappyDynamicFrames, sizeof(kHappyDynamicFrames) / sizeof(kHappyDynamicFrames[0]), 180},
    {"happy_squint_dynamic", kHappyDynamicFrames, sizeof(kHappyDynamicFrames) / sizeof(kHappyDynamicFrames[0]), 180},
};

SemaphoreHandle_t display_mutex = nullptr;
ExpressionControllerHooks hooks = {};
TaskHandle_t expression_animation_task_handle = nullptr;
TaskHandle_t temporary_expression_task_handle = nullptr;
TaskHandle_t speak_animation_task_handle = nullptr;
volatile bool expression_animation_running = false;
volatile uint32_t temporary_expression_until_ms = 0;
volatile bool speak_animation_running = false;
volatile bool speech_expression_overridden = false;
bool expression_screen_visible = false;
const ExpressionAsset* current_expression_asset = nullptr;
const ExpressionAnimation* current_expression_animation = nullptr;
volatile bool sleep_dark_visible = false;
volatile uint32_t last_lit_expression_ms = 0;

class DisplayLock {
public:
    DisplayLock()
    {
        if (display_mutex != nullptr) {
            xSemaphoreTake(display_mutex, portMAX_DELAY);
            locked_ = true;
        }
    }

    ~DisplayLock()
    {
        if (locked_) {
            xSemaphoreGive(display_mutex);
        }
    }

private:
    bool locked_ = false;
};

static const ExpressionAsset* find_expression_asset(const char* expression)
{
    const char* name = expression != nullptr && expression[0] != '\0' ? expression : kDefaultExpression;
    if (strcmp(name, "screen_off") == 0 || strcmp(name, "sleep") == 0 || strcmp(name, "stopped") == 0) {
        name = "sleep_dark";
    } else if (strcmp(name, "listening") == 0 || strcmp(name, "default") == 0) {
        name = kDefaultExpression;
    }

    for (const auto& asset : kExpressionAssets) {
        if (strcmp(asset.name, name) == 0) {
            return &asset;
        }
    }

    for (const auto& asset : kExpressionAssets) {
        if (strcmp(asset.name, kDefaultExpression) == 0) {
            return &asset;
        }
    }
    return nullptr;
}

static bool is_sleep_dark_expression(const char* expression)
{
    const char* name = expression != nullptr && expression[0] != '\0' ? expression : "";
    return strcmp(name, "sleep_dark") == 0 || strcmp(name, "screen_off") == 0 ||
           strcmp(name, "sleep") == 0 || strcmp(name, "stopped") == 0;
}

static const ExpressionAnimation* find_expression_animation(const char* expression)
{
    const char* name = expression != nullptr && expression[0] != '\0' ? expression : "";
    for (const auto& animation : kExpressionAnimations) {
        if (strcmp(animation.name, name) == 0) {
            return &animation;
        }
    }
    return nullptr;
}

static void render_expression_frame(const char* expression)
{
    const bool sleep_dark_requested = is_sleep_dark_expression(expression);
    {
        DisplayLock lock;
        auto& display = M5.Display;
        const ExpressionAsset* asset = find_expression_asset(expression);
        if (asset != nullptr) {
            const uint32_t image_len = static_cast<uint32_t>(asset->end - asset->start);
            const int draw_x = (display.width() - asset->width) / 2;
            const int draw_y = (display.height() - asset->height) / 2;
            if (!expression_screen_visible || current_expression_asset == nullptr ||
                current_expression_asset->width != asset->width || current_expression_asset->height != asset->height) {
                if (display.drawPng(asset->start, image_len, draw_x, draw_y)) {
                    expression_screen_visible = true;
                    current_expression_asset = asset;
                    sleep_dark_visible = sleep_dark_requested;
                    if (!sleep_dark_requested) {
                        last_lit_expression_ms = M5.millis();
                    }
                    return;
                }
            } else if (current_expression_asset != asset) {
                if (display.drawPng(asset->start, image_len, draw_x, draw_y)) {
                    current_expression_asset = asset;
                    sleep_dark_visible = sleep_dark_requested;
                    if (!sleep_dark_requested) {
                        last_lit_expression_ms = M5.millis();
                    }
                    return;
                }
            } else {
                return;
            }
        }
        display.fillScreen(TFT_BLACK);
        mark_expression_screen_dirty();
        sleep_dark_visible = true;
    }
}

static void stop_expression_animation()
{
    expression_animation_running = false;
    for (int i = 0; i < 30 && expression_animation_task_handle != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    current_expression_animation = nullptr;
}

static void start_expression_animation(const ExpressionAnimation* animation)
{
    if (animation == nullptr || animation->frame_count == 0) {
        return;
    }
    if (expression_animation_task_handle != nullptr && current_expression_animation == animation) {
        return;
    }
    stop_expression_animation();
    expression_animation_running = true;
    current_expression_animation = animation;
    xTaskCreatePinnedToCore([](void* arg) {
        const auto* animation = static_cast<const ExpressionAnimation*>(arg);
        size_t frame = 0;
        while (expression_animation_running && animation != nullptr && animation->frame_count > 0) {
            render_expression_frame(animation->frames[frame]);
            frame = (frame + 1) % animation->frame_count;
            vTaskDelay(pdMS_TO_TICKS(animation->frame_ms));
        }
        expression_animation_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "expr_anim", 4 * 1024, const_cast<ExpressionAnimation*>(animation), 2, &expression_animation_task_handle, 0);
}

static void notify_temporary_expression_task()
{
    TaskHandle_t task = temporary_expression_task_handle;
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

static void show_expression_internal(const char* expression, bool notify_temporary_task)
{
    const ExpressionAnimation* animation = find_expression_animation(expression);
    if (animation != nullptr) {
        start_expression_animation(animation);
        if (notify_temporary_task) {
            notify_temporary_expression_task();
        }
        return;
    }
    stop_expression_animation();
    render_expression_frame(expression);
    if (notify_temporary_task) {
        notify_temporary_expression_task();
    }
}

static bool should_restore_listening_light()
{
    return hooks.should_restore_listening_light != nullptr && hooks.should_restore_listening_light();
}

static void restore_light_after_speaking()
{
    if (should_restore_listening_light()) {
        if (hooks.set_light_strip_listening != nullptr) {
            hooks.set_light_strip_listening();
        }
    } else if (hooks.set_light_strip_sleeping != nullptr) {
        hooks.set_light_strip_sleeping();
    }
}

} // namespace

void expression_controller_init(SemaphoreHandle_t m5_mutex, const ExpressionControllerHooks& new_hooks)
{
    display_mutex = m5_mutex;
    hooks = new_hooks;
}

void mark_expression_screen_dirty()
{
    expression_screen_visible = false;
    current_expression_asset = nullptr;
}

bool expression_screen_is_visible()
{
    return expression_screen_visible;
}

bool speaking_animation_is_running()
{
    return speak_animation_running;
}

void expression_set_speech_overridden(bool overridden)
{
    speech_expression_overridden = overridden;
}

void show_expression(const char* expression)
{
    show_expression_internal(expression, true);
}

void show_idle_sleep_dark_if_due(uint32_t idle_ms)
{
    if (sleep_dark_visible || current_expression_animation != nullptr || speak_animation_running) {
        return;
    }
    uint32_t last = last_lit_expression_ms;
    if (last == 0) {
        last_lit_expression_ms = M5.millis();
        return;
    }
    if (static_cast<uint32_t>(M5.millis() - last) >= idle_ms) {
        show_expression_internal("sleep_dark", false);
    }
}

void show_temporary_expression(const char* expression, uint32_t duration_ms)
{
    const ExpressionAsset* expected_asset = find_expression_asset(expression);
    if (expected_asset == nullptr) {
        return;
    }

    temporary_expression_until_ms = static_cast<uint32_t>(M5.millis() + duration_ms);
    show_expression_internal(expression, false);

    if (temporary_expression_task_handle != nullptr) {
        return;
    }

    xTaskCreatePinnedToCore([](void* arg) {
        const auto* expected_asset = static_cast<const ExpressionAsset*>(arg);
        while (true) {
            if (current_expression_asset != expected_asset ||
                current_expression_animation != nullptr ||
                speak_animation_running) {
                break;
            }

            uint32_t now = M5.millis();
            uint32_t until = temporary_expression_until_ms;
            int32_t remaining_ms = static_cast<int32_t>(until - now);
            if (remaining_ms > 0) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::min<int32_t>(remaining_ms, 250)));
                continue;
            }

            if (until == temporary_expression_until_ms &&
                current_expression_asset == expected_asset &&
                current_expression_animation == nullptr &&
                !speak_animation_running) {
                show_expression_internal(kDefaultExpression, false);
            }

            break;
        }
        temporary_expression_task_handle = nullptr;
        temporary_expression_until_ms = 0;
        vTaskDelete(nullptr);
    }, "temp_expr", 4 * 1024, const_cast<ExpressionAsset*>(expected_asset), 2,
       &temporary_expression_task_handle, 0);
}

void start_speaking_animation()
{
    if (hooks.start_speaking_light_animation != nullptr) {
        hooks.start_speaking_light_animation();
    }
    speak_animation_running = true;
    if (speak_animation_task_handle != nullptr) {
        return;
    }
    xTaskCreatePinnedToCore([](void*) {
        bool open = false;
        while (speak_animation_running) {
            show_expression(open ? "speak2" : "speak1");
            open = !open;
            vTaskDelay(pdMS_TO_TICKS(160));
        }
        speak_animation_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "speak_anim", 4 * 1024, nullptr, 2, &speak_animation_task_handle, 0);
}

void stop_speaking_animation()
{
    speak_animation_running = false;
    for (int i = 0; i < 30 && speak_animation_task_handle != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (hooks.stop_speaking_light_animation != nullptr) {
        hooks.stop_speaking_light_animation();
    }
    if (speech_expression_overridden) {
        speech_expression_overridden = false;
        restore_light_after_speaking();
        return;
    }
    show_expression(kDefaultExpression);
    restore_light_after_speaking();
}

void cancel_speaking_animation_now()
{
    speak_animation_running = false;
}
