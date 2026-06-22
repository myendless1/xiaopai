#include "expression_controller.h"

#include <M5Unified.h>

#include "freertos/task.h"

#include <algorithm>
#include <string.h>

extern const uint8_t calm_face_png_start[] asm("_binary_calm_face_png_start");
extern const uint8_t calm_face_png_end[] asm("_binary_calm_face_png_end");
extern const uint8_t sleep_dark_face_png_start[] asm("_binary_sleep_dark_face_png_start");
extern const uint8_t sleep_dark_face_png_end[] asm("_binary_sleep_dark_face_png_end");
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

enum class MouthShape : uint8_t {
    Closed,
    SmallOpen,
    BigOpen,
    Wry,
    SmallHeart,
    BigHeart,
};

static const ExpressionAsset kExpressionAssets[] = {
    {"calm", calm_face_png_start, calm_face_png_end, 320, 240},
    {"sleep_dark", sleep_dark_face_png_start, sleep_dark_face_png_end, 320, 240},
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
    {"happy_dynamic", kHappyDynamicFrames, sizeof(kHappyDynamicFrames) / sizeof(kHappyDynamicFrames[0]), 180},
    {"happy_squint_dynamic", kHappyDynamicFrames, sizeof(kHappyDynamicFrames) / sizeof(kHappyDynamicFrames[0]), 180},
};

SemaphoreHandle_t display_mutex = nullptr;
ExpressionControllerHooks hooks = {};
TaskHandle_t expression_animation_task_handle = nullptr;
TaskHandle_t temporary_expression_task_handle = nullptr;
TaskHandle_t mouth_animation_task_handle = nullptr;
volatile bool expression_animation_running = false;
volatile uint32_t temporary_expression_until_ms = 0;
volatile bool mouth_animation_running = false;
volatile bool speech_expression_overridden = false;
volatile uint32_t mouth_audio_level_ms = 0;
bool expression_screen_visible = false;
const ExpressionAsset* current_expression_asset = nullptr;
const ExpressionAnimation* current_expression_animation = nullptr;
MouthShape mouth_shape = MouthShape::Closed;
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

static bool is_sleep_dark_asset(const ExpressionAsset* asset)
{
    return asset != nullptr && strcmp(asset->name, "sleep_dark") == 0;
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

static bool name_equals_any(const char* name, const char* const* aliases, size_t alias_count)
{
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    for (size_t i = 0; i < alias_count; ++i) {
        if (strcmp(name, aliases[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool mouth_shape_from_name(const char* mouth, MouthShape* shape)
{
    static constexpr const char* kClosedAliases[] = {
        "closed", "close", "closed_mouth", "shut", "none",
    };
    static constexpr const char* kSmallOpenAliases[] = {
        "small", "small_open", "small_mouth", "little", "speak1",
    };
    static constexpr const char* kBigOpenAliases[] = {
        "big", "big_open", "big_mouth", "open", "large", "speak2",
    };
    static constexpr const char* kWryAliases[] = {
        "wry", "skew", "crooked", "crooked_mouth", "thinking_mouth",
    };
    static constexpr const char* kSmallHeartAliases[] = {
        "small_heart", "heart_small", "small-heart", "kiss",
    };
    static constexpr const char* kBigHeartAliases[] = {
        "big_heart", "heart_big", "big-heart",
    };

    if (shape == nullptr || mouth == nullptr || mouth[0] == '\0') {
        return false;
    }
    if (name_equals_any(mouth, kClosedAliases, sizeof(kClosedAliases) / sizeof(kClosedAliases[0]))) {
        *shape = MouthShape::Closed;
        return true;
    }
    if (name_equals_any(mouth, kSmallOpenAliases, sizeof(kSmallOpenAliases) / sizeof(kSmallOpenAliases[0]))) {
        *shape = MouthShape::SmallOpen;
        return true;
    }
    if (name_equals_any(mouth, kBigOpenAliases, sizeof(kBigOpenAliases) / sizeof(kBigOpenAliases[0]))) {
        *shape = MouthShape::BigOpen;
        return true;
    }
    if (name_equals_any(mouth, kWryAliases, sizeof(kWryAliases) / sizeof(kWryAliases[0]))) {
        *shape = MouthShape::Wry;
        return true;
    }
    if (name_equals_any(mouth, kSmallHeartAliases, sizeof(kSmallHeartAliases) / sizeof(kSmallHeartAliases[0]))) {
        *shape = MouthShape::SmallHeart;
        return true;
    }
    if (name_equals_any(mouth, kBigHeartAliases, sizeof(kBigHeartAliases) / sizeof(kBigHeartAliases[0]))) {
        *shape = MouthShape::BigHeart;
        return true;
    }
    return false;
}

static void draw_open_mouth_locked(int rx, int ry)
{
    auto& display = M5.Display;
    display.fillEllipse(160, 190, rx, ry, display.color565(10, 61, 135));
    display.fillEllipse(160, 190, std::max(1, rx - 7), std::max(1, ry - 7),
                        display.color565(55, 145, 255));
    display.fillEllipse(153, 183, 5, 4, display.color565(160, 211, 255));
}

static void draw_wry_mouth_locked()
{
    auto& display = M5.Display;
    for (int i = 0; i < 10; ++i) {
        display.drawLine(142, 191 + i, 178, 182 + i, display.color565(10, 61, 135));
    }
    for (int i = 0; i < 6; ++i) {
        display.drawLine(145, 191 + i, 175, 184 + i, display.color565(102, 173, 255));
    }
}

static void draw_heart_mouth_locked(int cx, int cy, int r)
{
    auto& display = M5.Display;
    display.fillCircle(cx - r, cy - r / 2, r, display.color565(132, 28, 52));
    display.fillCircle(cx + r, cy - r / 2, r, display.color565(132, 28, 52));
    display.fillTriangle(cx - r * 2, cy, cx + r * 2, cy, cx, cy + r * 2,
                         display.color565(132, 28, 52));
    const int inner = std::max(2, r - 3);
    display.fillCircle(cx - inner, cy - inner / 2, inner, display.color565(238, 66, 102));
    display.fillCircle(cx + inner, cy - inner / 2, inner, display.color565(238, 66, 102));
    display.fillTriangle(cx - inner * 2, cy, cx + inner * 2, cy, cx, cy + inner * 2,
                         display.color565(238, 66, 102));
    display.fillCircle(cx - inner, cy - inner, std::max(1, inner / 3), display.color565(255, 147, 170));
}

static void render_mouth_locked(MouthShape shape)
{
    if (!expression_screen_visible || current_expression_asset == nullptr ||
        is_sleep_dark_asset(current_expression_asset)) {
        return;
    }

    auto& display = M5.Display;
    display.fillRect(120, 160, 80, 66, TFT_BLACK);
    if (shape == MouthShape::SmallOpen) {
        draw_open_mouth_locked(20, 15);
    } else if (shape == MouthShape::BigOpen) {
        draw_open_mouth_locked(27, 22);
    } else if (shape == MouthShape::Wry) {
        draw_wry_mouth_locked();
    } else if (shape == MouthShape::SmallHeart) {
        draw_heart_mouth_locked(160, 184, 8);
    } else if (shape == MouthShape::BigHeart) {
        draw_heart_mouth_locked(160, 181, 12);
    } else {
        display.fillRoundRect(144, 184, 32, 10, 5, display.color565(10, 61, 135));
        display.fillRoundRect(147, 185, 26, 6, 3, display.color565(102, 173, 255));
    }
}

static void redraw_mouth()
{
    DisplayLock lock;
    render_mouth_locked(mouth_shape);
}

static void set_mouth_shape_internal(MouthShape shape)
{
    if (mouth_shape == shape) {
        return;
    }
    mouth_shape = shape;
    redraw_mouth();
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
                        render_mouth_locked(mouth_shape);
                    }
                    return;
                }
            } else if (current_expression_asset != asset) {
                if (display.drawPng(asset->start, image_len, draw_x, draw_y)) {
                    current_expression_asset = asset;
                    sleep_dark_visible = sleep_dark_requested;
                    if (!sleep_dark_requested) {
                        last_lit_expression_ms = M5.millis();
                        render_mouth_locked(mouth_shape);
                    }
                    return;
                }
            } else {
                render_mouth_locked(mouth_shape);
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
    MouthShape shape;
    if (mouth_shape_from_name(expression, &shape)) {
        stop_expression_animation();
        set_mouth_shape_internal(shape);
        render_expression_frame(current_expression_asset != nullptr ? current_expression_asset->name
                                                                    : kDefaultExpression);
        if (notify_temporary_task) {
            notify_temporary_expression_task();
        }
        return;
    }

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
    return mouth_animation_running;
}

void expression_set_speech_overridden(bool overridden)
{
    speech_expression_overridden = overridden;
}

void show_expression(const char* expression)
{
    show_expression_internal(expression, true);
}

void show_expression_with_mouth(const char* expression, const char* mouth)
{
    MouthShape shape;
    if (mouth_shape_from_name(mouth, &shape)) {
        set_mouth_shape_internal(shape);
    }
    show_expression_internal(expression, true);
}

void set_mouth_shape(const char* mouth)
{
    MouthShape shape;
    if (mouth_shape_from_name(mouth, &shape)) {
        set_mouth_shape_internal(shape);
    }
}

void show_idle_sleep_dark_if_due(uint32_t idle_ms)
{
    if (sleep_dark_visible || current_expression_animation != nullptr || mouth_animation_running) {
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
                current_expression_animation != nullptr) {
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
                current_expression_animation == nullptr) {
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

void start_mouth_animation()
{
    if (hooks.start_speaking_light_animation != nullptr) {
        hooks.start_speaking_light_animation();
    }
    mouth_animation_running = true;
    mouth_audio_level_ms = 0;
    set_mouth_shape_internal(MouthShape::Closed);
    if (mouth_animation_task_handle != nullptr) {
        return;
    }
    xTaskCreatePinnedToCore([](void*) {
        bool big = false;
        while (mouth_animation_running) {
            uint32_t last_level_ms = mouth_audio_level_ms;
            if (last_level_ms == 0 ||
                static_cast<uint32_t>(M5.millis() - last_level_ms) > 220) {
                set_mouth_shape_internal(big ? MouthShape::BigOpen : MouthShape::SmallOpen);
                big = !big;
            }
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        mouth_animation_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "mouth_anim", 4 * 1024, nullptr, 2, &mouth_animation_task_handle, 0);
}

void stop_mouth_animation()
{
    mouth_animation_running = false;
    for (int i = 0; i < 30 && mouth_animation_task_handle != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    mouth_audio_level_ms = 0;
    set_mouth_shape_internal(MouthShape::Closed);
    if (hooks.stop_speaking_light_animation != nullptr) {
        hooks.stop_speaking_light_animation();
    }
    speech_expression_overridden = false;
    restore_light_after_speaking();
}

void update_mouth_from_speaking_level(uint8_t level)
{
    if (!mouth_animation_running) {
        return;
    }
    mouth_audio_level_ms = M5.millis();
    if (level == 0) {
        set_mouth_shape_internal(MouthShape::Closed);
    } else if (level == 1) {
        set_mouth_shape_internal(MouthShape::SmallOpen);
    } else {
        set_mouth_shape_internal(MouthShape::BigOpen);
    }
}

void cancel_mouth_animation_now()
{
    mouth_animation_running = false;
    mouth_audio_level_ms = 0;
    set_mouth_shape_internal(MouthShape::Closed);
}
