#include "expression_controller.h"

#include <M5Unified.h>

#include "freertos/task.h"

#include <algorithm>
#include <math.h>
#include <string.h>

const char* const kDefaultExpression = "calm";

namespace {

struct Point {
    int x;
    int y;
};

namespace FaceLayout {
// Canonical feature anchors. New expressions must derive feature coordinates
// from these points; only explicit expression offsets may move them.
static constexpr int kCanvasWidth = 320;
static constexpr int kCanvasHeight = 240;
static constexpr Point kLeftEyeCenter = {88, 101};
static constexpr Point kRightEyeCenter = {232, 101};
static constexpr Point kLeftBrowCenter = {88, 72};
static constexpr Point kRightBrowCenter = {232, 72};
static constexpr Point kMouthCenter = {160, 152};
static constexpr Point kLeftCheekCenter = {47, 141};
static constexpr Point kRightCheekCenter = {273, 141};
static constexpr int kMaxIntentionalOffsetX = 10;
static constexpr int kMaxIntentionalOffsetY = 18;
} // namespace FaceLayout

enum class FaceKind : uint8_t {
    Calm,
    SleepDark,
    Speak1,
    Speak2,
    Thinking,
    Shy,
    Smile,
    Happy,
    Relaxed,
    Wink,
};

enum class EyeStyle : uint8_t {
    Open,
    ClosedHappy,
    ClosedRelaxed,
    WinkRight,
};

enum class BrowStyle : uint8_t {
    None,
    Thinking,
};

enum class MouthShape : uint8_t {
    Closed,
    Speak1,
    Speak2,
    Frown,
    Smile,
    SmileWide,
    HappyOpen,
};

enum class CheekStyle : uint8_t {
    None,
    Shy,
};

struct FacePose {
    EyeStyle left_eye = EyeStyle::Open;
    EyeStyle right_eye = EyeStyle::Open;
    BrowStyle left_brow = BrowStyle::None;
    BrowStyle right_brow = BrowStyle::None;
    MouthShape default_mouth = MouthShape::Closed;
    CheekStyle cheeks = CheekStyle::None;
    int offset_x = 0;
    int offset_y = 0;
    int mouth_y_offset = 0;
    int mouth_width = 42;
    int mouth_height = 8;
    int mouth_radius = 4;
    bool sleep_dark = false;
};

struct ExpressionAnimation {
    const char* name;
    const FaceKind* frames;
    size_t frame_count;
    int frame_ms;
};

static constexpr FaceKind kHappyDynamicFrames[] = {
    FaceKind::Smile,
    FaceKind::Happy,
    FaceKind::Smile,
    FaceKind::Happy,
};

static constexpr ExpressionAnimation kExpressionAnimations[] = {
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
const ExpressionAnimation* current_expression_animation = nullptr;
FaceKind current_face_kind = FaceKind::Calm;
MouthShape mouth_shape = MouthShape::Closed;
bool manual_mouth_override = false;
volatile bool sleep_dark_visible = false;
volatile uint32_t last_lit_expression_ms = 0;
lgfx::LovyanGFX* active_draw_target = nullptr;
M5Canvas* face_canvas = nullptr;
bool face_canvas_ready = false;

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

static lgfx::LovyanGFX& draw_target_locked()
{
    return active_draw_target != nullptr ? *active_draw_target : static_cast<lgfx::LovyanGFX&>(M5.Display);
}

static int canvas_origin_x_locked()
{
    return (draw_target_locked().width() - FaceLayout::kCanvasWidth) / 2;
}

static int canvas_origin_y_locked()
{
    return (draw_target_locked().height() - FaceLayout::kCanvasHeight) / 2;
}

static int clamp_intentional_offset_x(int value)
{
    return std::max(-FaceLayout::kMaxIntentionalOffsetX,
                    std::min(FaceLayout::kMaxIntentionalOffsetX, value));
}

static int clamp_intentional_offset_y(int value)
{
    return std::max(-FaceLayout::kMaxIntentionalOffsetY,
                    std::min(FaceLayout::kMaxIntentionalOffsetY, value));
}

static Point anchor_point_locked(Point anchor, const FacePose& pose, int dx = 0, int dy = 0)
{
    return {
        canvas_origin_x_locked() + anchor.x + clamp_intentional_offset_x(pose.offset_x) + dx,
        canvas_origin_y_locked() + anchor.y + clamp_intentional_offset_y(pose.offset_y) + dy,
    };
}

static uint16_t line_color_locked()
{
    return draw_target_locked().color565(245, 248, 255);
}

static uint16_t cheek_color_locked()
{
    return draw_target_locked().color565(255, 155, 185);
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
    static constexpr const char* kSpeak1Aliases[] = {
        "small", "small_open", "small_mouth", "little", "speak", "speaking", "speak1",
    };
    static constexpr const char* kSpeak2Aliases[] = {
        "big", "big_open", "big_mouth", "open", "large", "speak2",
    };
    static constexpr const char* kFrownAliases[] = {
        "frown", "thinking_mouth",
    };
    static constexpr const char* kSmileAliases[] = {
        "smile_mouth", "smile",
    };
    static constexpr const char* kHappyOpenAliases[] = {
        "happy_open", "laugh",
    };

    if (shape == nullptr || mouth == nullptr || mouth[0] == '\0') {
        return false;
    }
    if (name_equals_any(mouth, kClosedAliases, sizeof(kClosedAliases) / sizeof(kClosedAliases[0]))) {
        *shape = MouthShape::Closed;
        return true;
    }
    if (name_equals_any(mouth, kSpeak1Aliases, sizeof(kSpeak1Aliases) / sizeof(kSpeak1Aliases[0]))) {
        *shape = MouthShape::Speak1;
        return true;
    }
    if (name_equals_any(mouth, kSpeak2Aliases, sizeof(kSpeak2Aliases) / sizeof(kSpeak2Aliases[0]))) {
        *shape = MouthShape::Speak2;
        return true;
    }
    if (name_equals_any(mouth, kFrownAliases, sizeof(kFrownAliases) / sizeof(kFrownAliases[0]))) {
        *shape = MouthShape::Frown;
        return true;
    }
    if (name_equals_any(mouth, kSmileAliases, sizeof(kSmileAliases) / sizeof(kSmileAliases[0]))) {
        *shape = MouthShape::Smile;
        return true;
    }
    if (name_equals_any(mouth, kHappyOpenAliases, sizeof(kHappyOpenAliases) / sizeof(kHappyOpenAliases[0]))) {
        *shape = MouthShape::HappyOpen;
        return true;
    }
    return false;
}

static bool face_kind_from_name(const char* expression, FaceKind* kind)
{
    if (kind == nullptr) {
        return false;
    }

    const char* name = expression != nullptr && expression[0] != '\0' ? expression : kDefaultExpression;
    if (strcmp(name, "screen_off") == 0 || strcmp(name, "sleep") == 0 ||
        strcmp(name, "sleeping") == 0 || strcmp(name, "stopped") == 0 ||
        strcmp(name, "sleep_dark") == 0) {
        *kind = FaceKind::SleepDark;
        return true;
    }
    if (strcmp(name, "listening") == 0 || strcmp(name, "default") == 0 ||
        strcmp(name, "wake") == 0 || strcmp(name, "awake") == 0 ||
        strcmp(name, "calm") == 0) {
        *kind = FaceKind::Calm;
        return true;
    }
    if (strcmp(name, "speak1") == 0 || strcmp(name, "speaking") == 0 ||
        strcmp(name, "speak") == 0) {
        *kind = FaceKind::Speak1;
        return true;
    }
    if (strcmp(name, "speak2") == 0) {
        *kind = FaceKind::Speak2;
        return true;
    }
    if (strcmp(name, "shy") == 0) {
        *kind = FaceKind::Shy;
        return true;
    }
    if (strcmp(name, "thinking") == 0 || strcmp(name, "waiting") == 0 ||
        strcmp(name, "wait") == 0) {
        *kind = FaceKind::Thinking;
        return true;
    }
    if (strcmp(name, "relaxed") == 0) {
        *kind = FaceKind::Relaxed;
        return true;
    }
    if (strcmp(name, "smile") == 0 || strcmp(name, "smile_blink") == 0) {
        *kind = FaceKind::Smile;
        return true;
    }
    if (strcmp(name, "wink") == 0 || strcmp(name, "wink_half") == 0 ||
        strcmp(name, "wink_closed") == 0) {
        *kind = FaceKind::Wink;
        return true;
    }
    if (strcmp(name, "happy") == 0 || strcmp(name, "happy_squint") == 0) {
        *kind = FaceKind::Happy;
        return true;
    }
    if (strcmp(name, "happy_squint_soft") == 0) {
        *kind = FaceKind::Smile;
        return true;
    }
    if (strcmp(name, "blink") == 0 || strcmp(name, "blink_half") == 0 ||
        strcmp(name, "blink_closed") == 0) {
        *kind = FaceKind::Relaxed;
        return true;
    }
    if (strcmp(name, "heart") == 0 || strcmp(name, "heart_action") == 0 ||
        strcmp(name, "heart_small") == 0 ||
        strcmp(name, "nod") == 0 || strcmp(name, "nodding") == 0 ||
        strcmp(name, "nod_soft") == 0 || strcmp(name, "nod_down") == 0) {
        *kind = FaceKind::Smile;
        return true;
    }

    return false;
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

static FacePose pose_for_kind(FaceKind kind)
{
    FacePose pose;
    switch (kind) {
    case FaceKind::SleepDark:
        pose.sleep_dark = true;
        pose.cheeks = CheekStyle::None;
        break;
    case FaceKind::Speak1:
        pose.default_mouth = MouthShape::Speak1;
        break;
    case FaceKind::Speak2:
        pose.default_mouth = MouthShape::Speak2;
        break;
    case FaceKind::Thinking:
        pose.left_brow = BrowStyle::Thinking;
        pose.default_mouth = MouthShape::Frown;
        pose.mouth_y_offset = -3;
        break;
    case FaceKind::Shy:
        pose.default_mouth = MouthShape::Smile;
        pose.cheeks = CheekStyle::Shy;
        pose.mouth_y_offset = -4;
        break;
    case FaceKind::Smile:
        pose.default_mouth = MouthShape::SmileWide;
        pose.cheeks = CheekStyle::Shy;
        pose.mouth_y_offset = -5;
        break;
    case FaceKind::Happy:
        pose.left_eye = EyeStyle::ClosedHappy;
        pose.right_eye = EyeStyle::ClosedHappy;
        pose.default_mouth = MouthShape::HappyOpen;
        pose.cheeks = CheekStyle::Shy;
        pose.mouth_y_offset = -4;
        break;
    case FaceKind::Relaxed:
        pose.left_eye = EyeStyle::ClosedRelaxed;
        pose.right_eye = EyeStyle::ClosedRelaxed;
        pose.default_mouth = MouthShape::Smile;
        pose.cheeks = CheekStyle::Shy;
        pose.mouth_y_offset = -6;
        break;
    case FaceKind::Wink:
        pose.right_eye = EyeStyle::WinkRight;
        pose.default_mouth = MouthShape::Smile;
        pose.cheeks = CheekStyle::Shy;
        pose.mouth_y_offset = -5;
        break;
    case FaceKind::Calm:
    default:
        pose.mouth_width = 38;
        pose.mouth_height = 7;
        pose.mouth_radius = 4;
        break;
    }
    return pose;
}

static void stroke_segment_locked(Point a, Point b, int width, uint16_t color)
{
    auto& display = draw_target_locked();
    const int radius = std::max(1, width / 2);
    const float dx = static_cast<float>(b.x - a.x);
    const float dy = static_cast<float>(b.y - a.y);
    const int steps = std::max(1, static_cast<int>(sqrtf(dx * dx + dy * dy)));
    const int stride = std::max(1, radius);
    for (int i = 0; i <= steps; i += stride) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const int x = static_cast<int>(lroundf(static_cast<float>(a.x) + dx * t));
        const int y = static_cast<int>(lroundf(static_cast<float>(a.y) + dy * t));
        display.fillCircle(x, y, radius, color);
    }
    display.fillCircle(b.x, b.y, radius, color);
}

static Point oval_point(Point center, int rx, int ry, int angle_deg)
{
    const float rad = static_cast<float>(angle_deg) * 3.14159265358979323846f / 180.0f;
    return {
        static_cast<int>(lroundf(static_cast<float>(center.x) + static_cast<float>(rx) * cosf(rad))),
        static_cast<int>(lroundf(static_cast<float>(center.y) + static_cast<float>(ry) * sinf(rad))),
    };
}

static void stroke_oval_arc_locked(Point center, int rx, int ry, int start_deg, int end_deg, int width, uint16_t color)
{
    int sweep = end_deg - start_deg;
    if (sweep <= 0) {
        sweep += 360;
    }
    const int steps = std::max(12, (sweep + 3) / 4);
    Point previous = oval_point(center, rx, ry, start_deg);
    for (int i = 1; i <= steps; ++i) {
        const int angle = start_deg + (sweep * i) / steps;
        Point current = oval_point(center, rx, ry, angle);
        stroke_segment_locked(previous, current, width, color);
        previous = current;
    }
    const int cap_radius = std::max(2, (width + 1) / 2);
    auto& display = draw_target_locked();
    display.fillCircle(oval_point(center, rx, ry, start_deg).x,
                       oval_point(center, rx, ry, start_deg).y, cap_radius, color);
    display.fillCircle(previous.x, previous.y, cap_radius, color);
}

static void draw_eye_locked(Point center, EyeStyle style, uint16_t color)
{
    auto& display = draw_target_locked();
    switch (style) {
    case EyeStyle::ClosedHappy:
        stroke_oval_arc_locked({center.x, center.y - 4}, 30, 24, 205, 335, 7, color);
        break;
    case EyeStyle::ClosedRelaxed:
        stroke_oval_arc_locked({center.x, center.y - 4}, 31, 17, 35, 145, 6, color);
        break;
    case EyeStyle::WinkRight:
        stroke_segment_locked({center.x + 20, center.y - 18}, {center.x - 15, center.y}, 7, color);
        stroke_segment_locked({center.x - 15, center.y}, {center.x + 18, center.y + 15}, 7, color);
        break;
    case EyeStyle::Open:
    default:
        display.fillCircle(center.x, center.y, 15, color);
        break;
    }
}

static void draw_brow_locked(Point center, BrowStyle style, bool left, uint16_t color)
{
    if (style == BrowStyle::None) {
        return;
    }
    (void)left;
    stroke_oval_arc_locked({center.x, center.y - 5}, 23, 12, 205, 335, 4, color);
}

static void fill_round_rect_locked(Point center, int width, int height, int radius, uint16_t color)
{
    auto& display = draw_target_locked();
    display.fillRoundRect(center.x - width / 2, center.y - height / 2, width, height, radius, color);
}

static void draw_happy_mouth_locked(Point center, uint16_t color)
{
    auto& display = draw_target_locked();
    const int rx = 31;
    const int ry = 35;
    const int flat_y = center.y - 8;
    const int bottom_y = center.y + 27;
    for (int y = flat_y; y <= bottom_y; ++y) {
        const float t = static_cast<float>(y - flat_y) / static_cast<float>(ry);
        const int half = static_cast<int>(lroundf(static_cast<float>(rx) * sqrtf(std::max(0.0f, 1.0f - t * t))));
        display.drawFastHLine(center.x - half, y, half * 2 + 1, color);
    }
    display.fillRoundRect(center.x - 31, flat_y - 3, 62, 13, 9, color);
}

static MouthShape effective_mouth_for_pose(const FacePose& pose)
{
    if (mouth_animation_running || manual_mouth_override) {
        return mouth_shape;
    }
    return pose.default_mouth;
}

static void draw_mouth_locked(const FacePose& pose, uint16_t color)
{
    const MouthShape shape = effective_mouth_for_pose(pose);
    Point center = anchor_point_locked(FaceLayout::kMouthCenter, pose, 0, pose.mouth_y_offset);
    switch (shape) {
    case MouthShape::Speak1:
        fill_round_rect_locked({center.x, center.y + 2}, 34, 14, 6, color);
        break;
    case MouthShape::Speak2:
        fill_round_rect_locked({center.x, center.y + 4}, 50, 21, 9, color);
        break;
    case MouthShape::Frown:
        stroke_oval_arc_locked({center.x, center.y + 11}, 18, 14, 200, 340, 5, color);
        break;
    case MouthShape::Smile:
        stroke_oval_arc_locked({center.x, center.y - 2}, 20, 17, 35, 145, 5, color);
        break;
    case MouthShape::SmileWide:
        stroke_oval_arc_locked({center.x, center.y - 3}, 34, 23, 35, 145, 6, color);
        break;
    case MouthShape::HappyOpen:
        draw_happy_mouth_locked(center, color);
        break;
    case MouthShape::Closed:
    default:
        fill_round_rect_locked(center, pose.mouth_width, pose.mouth_height, pose.mouth_radius, color);
        break;
    }
}

static void draw_cheek_pair_locked(const FacePose& pose)
{
    if (pose.cheeks == CheekStyle::None) {
        return;
    }
    const uint16_t color = cheek_color_locked();
    Point left = anchor_point_locked(FaceLayout::kLeftCheekCenter, pose);
    Point right = anchor_point_locked(FaceLayout::kRightCheekCenter, pose);

    static constexpr int kOffsets[] = {-13, 0, 13};
    for (int xoff : kOffsets) {
        stroke_segment_locked({left.x + xoff - 3, left.y + 7}, {left.x + xoff + 3, left.y - 7}, 4, color);
        stroke_segment_locked({right.x + xoff + 4, right.y + 7}, {right.x + xoff - 4, right.y - 7}, 4, color);
    }
}

static void draw_face_locked(FaceKind kind)
{
    auto& display = draw_target_locked();
    FacePose pose = pose_for_kind(kind);

    display.fillScreen(TFT_BLACK);
    current_face_kind = kind;
    expression_screen_visible = true;
    sleep_dark_visible = pose.sleep_dark;

    if (pose.sleep_dark) {
        return;
    }

    const uint16_t line = line_color_locked();

    draw_brow_locked(anchor_point_locked(FaceLayout::kLeftBrowCenter, pose), pose.left_brow, true, line);
    draw_brow_locked(anchor_point_locked(FaceLayout::kRightBrowCenter, pose), pose.right_brow, false, line);
    draw_eye_locked(anchor_point_locked(FaceLayout::kLeftEyeCenter, pose), pose.left_eye, line);
    draw_eye_locked(anchor_point_locked(FaceLayout::kRightEyeCenter, pose), pose.right_eye, line);
    draw_cheek_pair_locked(pose);
    draw_mouth_locked(pose, line);

    last_lit_expression_ms = M5.millis();
}

static bool ensure_face_canvas_locked()
{
    if (face_canvas_ready && face_canvas != nullptr) {
        return true;
    }
    if (face_canvas == nullptr) {
        static M5Canvas canvas(&M5.Display);
        face_canvas = &canvas;
    }
    face_canvas->setColorDepth(16);
    face_canvas_ready = face_canvas->createSprite(FaceLayout::kCanvasWidth, FaceLayout::kCanvasHeight) != nullptr;
    return face_canvas_ready;
}

static void render_face_to_display_locked(FaceKind kind)
{
    const bool use_canvas = ensure_face_canvas_locked();
    if (use_canvas) {
        active_draw_target = face_canvas;
        draw_face_locked(kind);
        active_draw_target = nullptr;
        const int draw_x = (M5.Display.width() - FaceLayout::kCanvasWidth) / 2;
        const int draw_y = (M5.Display.height() - FaceLayout::kCanvasHeight) / 2;
        M5.Display.fillScreen(TFT_BLACK);
        face_canvas->pushSprite(&M5.Display, draw_x, draw_y);
        return;
    }
    active_draw_target = nullptr;
    draw_face_locked(kind);
}

static void render_expression_frame(FaceKind kind)
{
    DisplayLock lock;
    render_face_to_display_locked(kind);
}

static void redraw_current_face()
{
    if (!expression_screen_visible) {
        return;
    }
    render_expression_frame(current_face_kind);
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

static void show_expression_internal(const char* expression, bool notify_temporary_task, bool reset_manual_mouth)
{
    const ExpressionAnimation* animation = find_expression_animation(expression);
    if (animation != nullptr) {
        if (reset_manual_mouth && !mouth_animation_running) {
            manual_mouth_override = false;
        }
        start_expression_animation(animation);
        if (notify_temporary_task) {
            notify_temporary_expression_task();
        }
        return;
    }

    FaceKind kind = FaceKind::Calm;
    if (face_kind_from_name(expression, &kind)) {
        if (reset_manual_mouth && !mouth_animation_running) {
            manual_mouth_override = false;
        }
        stop_expression_animation();
        render_expression_frame(kind);
        if (notify_temporary_task) {
            notify_temporary_expression_task();
        }
        return;
    }

    MouthShape shape;
    if (mouth_shape_from_name(expression, &shape)) {
        stop_expression_animation();
        mouth_shape = shape;
        manual_mouth_override = true;
        redraw_current_face();
        if (notify_temporary_task) {
            notify_temporary_expression_task();
        }
        return;
    }

    kind = FaceKind::Calm;
    if (reset_manual_mouth && !mouth_animation_running) {
        manual_mouth_override = false;
    }
    stop_expression_animation();
    render_expression_frame(kind);
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
}

bool expression_screen_is_visible()
{
    return expression_screen_visible;
}

bool sleep_dark_is_visible()
{
    return sleep_dark_visible;
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
    show_expression_internal(expression, true, true);
}

void show_expression_with_mouth(const char* expression, const char* mouth)
{
    MouthShape shape;
    const bool has_mouth = mouth_shape_from_name(mouth, &shape);
    if (has_mouth) {
        mouth_shape = shape;
        manual_mouth_override = true;
    }
    show_expression_internal(expression, true, !has_mouth);
}

void set_mouth_shape(const char* mouth)
{
    MouthShape shape;
    if (mouth_shape_from_name(mouth, &shape)) {
        stop_expression_animation();
        mouth_shape = shape;
        manual_mouth_override = true;
        redraw_current_face();
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
        show_expression_internal("sleep_dark", false, true);
    }
}

void show_temporary_expression(const char* expression, uint32_t duration_ms)
{
    FaceKind expected_kind;
    if (!face_kind_from_name(expression, &expected_kind)) {
        expected_kind = FaceKind::Calm;
    }

    temporary_expression_until_ms = static_cast<uint32_t>(M5.millis() + duration_ms);
    show_expression_internal(expression, false, true);

    if (temporary_expression_task_handle != nullptr) {
        return;
    }

    xTaskCreatePinnedToCore([](void* arg) {
        const auto expected_kind = static_cast<FaceKind>(reinterpret_cast<uintptr_t>(arg));
        while (true) {
            if (current_face_kind != expected_kind || current_expression_animation != nullptr) {
                break;
            }

            uint32_t now = M5.millis();
            uint32_t until = temporary_expression_until_ms;
            int32_t remaining_ms = static_cast<int32_t>(until - now);
            if (remaining_ms > 0) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::min<int32_t>(remaining_ms, 250)));
                continue;
            }

            if (until == temporary_expression_until_ms && current_face_kind == expected_kind &&
                current_expression_animation == nullptr) {
                show_expression_internal(kDefaultExpression, false, true);
            }

            break;
        }
        temporary_expression_task_handle = nullptr;
        temporary_expression_until_ms = 0;
        vTaskDelete(nullptr);
    }, "temp_expr", 4 * 1024, reinterpret_cast<void*>(static_cast<uintptr_t>(expected_kind)), 2,
       &temporary_expression_task_handle, 0);
}

void start_mouth_animation()
{
    if (hooks.start_speaking_light_animation != nullptr) {
        hooks.start_speaking_light_animation();
    }
    mouth_animation_running = true;
    mouth_audio_level_ms = 0;
    mouth_shape = MouthShape::Closed;
    manual_mouth_override = false;
    redraw_current_face();
    if (mouth_animation_task_handle != nullptr) {
        return;
    }
    xTaskCreatePinnedToCore([](void*) {
        bool big = false;
        while (mouth_animation_running) {
            uint32_t last_level_ms = mouth_audio_level_ms;
            if (last_level_ms == 0 ||
                static_cast<uint32_t>(M5.millis() - last_level_ms) > 220) {
                mouth_shape = big ? MouthShape::Speak2 : MouthShape::Speak1;
                redraw_current_face();
                big = !big;
            }
            vTaskDelay(pdMS_TO_TICKS(120));
        }
        mouth_animation_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "mouth_anim", 4 * 1024, nullptr, 2, &mouth_animation_task_handle, 0);
}

void start_speech_visual_feedback(bool animate_mouth)
{
    if (animate_mouth) {
        start_mouth_animation();
        return;
    }
    if (hooks.start_speaking_light_animation != nullptr) {
        hooks.start_speaking_light_animation();
    }
}

void stop_mouth_animation()
{
    mouth_animation_running = false;
    for (int i = 0; i < 30 && mouth_animation_task_handle != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    mouth_audio_level_ms = 0;
    mouth_shape = MouthShape::Closed;
    manual_mouth_override = false;
    redraw_current_face();
    if (hooks.stop_speaking_light_animation != nullptr) {
        hooks.stop_speaking_light_animation();
    }
    speech_expression_overridden = false;
    restore_light_after_speaking();
}

void stop_speech_visual_feedback(bool animate_mouth)
{
    if (animate_mouth) {
        stop_mouth_animation();
        return;
    }
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
        mouth_shape = MouthShape::Closed;
    } else if (level == 1) {
        mouth_shape = MouthShape::Speak1;
    } else {
        mouth_shape = MouthShape::Speak2;
    }
    redraw_current_face();
}

void cancel_mouth_animation_now()
{
    mouth_animation_running = false;
    mouth_audio_level_ms = 0;
    mouth_shape = MouthShape::Closed;
    manual_mouth_override = false;
    redraw_current_face();
}
