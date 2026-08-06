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
// Canonical feature anchors from the pre-eye-only face layout.
static constexpr int kCanvasWidth = 320;
static constexpr int kCanvasHeight = 240;
static constexpr Point kLeftEyeCenter = {88, 101};
static constexpr Point kRightEyeCenter = {232, 101};
static constexpr Point kMouthCenter = {160, 152};
static constexpr Point kLeftCheekCenter = {47, 141};
static constexpr Point kRightCheekCenter = {273, 141};
static constexpr int kMaxIntentionalOffsetX = 10;
static constexpr int kMaxIntentionalOffsetY = 18;
} // namespace FaceLayout

enum class FaceKind : uint8_t {
    Calm,
    CalmBlink,
    SleepDark,
    Thinking,
    Happy,
    Surprised,
    Shy,
};

enum class EyeStyle : uint8_t {
    Open,
    ClosedLine,
};

enum class MouthShape : uint8_t {
    Closed,
    Smile,
    SmileWide,
    Frown,
    OpenOval,
};

enum class CheekStyle : uint8_t {
    None,
    Shy,
};

struct FacePose {
    EyeStyle left_eye = EyeStyle::Open;
    EyeStyle right_eye = EyeStyle::Open;
    MouthShape mouth = MouthShape::Closed;
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
    FaceKind::Happy,
    FaceKind::Calm,
    FaceKind::Happy,
    FaceKind::Calm,
};

static constexpr ExpressionAnimation kExpressionAnimations[] = {
    {"happy_dynamic", kHappyDynamicFrames, sizeof(kHappyDynamicFrames) / sizeof(kHappyDynamicFrames[0]), 180},
    {"happy_squint_dynamic", kHappyDynamicFrames, sizeof(kHappyDynamicFrames) / sizeof(kHappyDynamicFrames[0]), 180},
};

static constexpr uint32_t kEyeBlinkClosedMs = 200;
static constexpr uint32_t kEyeBlinkDelayMinMs = 1000;
static constexpr uint32_t kEyeBlinkDelayMaxMs = 2000;

SemaphoreHandle_t display_mutex = nullptr;
ExpressionControllerHooks hooks = {};
TaskHandle_t expression_animation_task_handle = nullptr;
TaskHandle_t temporary_expression_task_handle = nullptr;
TaskHandle_t eye_blink_task_handle = nullptr;
volatile bool expression_animation_running = false;
volatile uint32_t temporary_expression_until_ms = 0;
volatile bool eye_blink_running = false;
uint32_t eye_blink_rng_state = 0x8f3c2d1u;
bool expression_screen_visible = false;
const ExpressionAnimation* current_expression_animation = nullptr;
FaceKind current_face_kind = FaceKind::Calm;
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
    if (strcmp(name, "calm_blink") == 0 || strcmp(name, "blink") == 0 ||
        strcmp(name, "blink_half") == 0 || strcmp(name, "blink_closed") == 0) {
        *kind = FaceKind::CalmBlink;
        return true;
    }
    if (strcmp(name, "shy") == 0 || strcmp(name, "shy_blink") == 0) {
        *kind = FaceKind::Shy;
        return true;
    }
    if (strcmp(name, "thinking") == 0 || strcmp(name, "waiting") == 0 ||
        strcmp(name, "wait") == 0 || strcmp(name, "thinking_blink") == 0 ||
        strcmp(name, "waiting_blink") == 0 || strcmp(name, "wait_blink") == 0) {
        *kind = FaceKind::Thinking;
        return true;
    }
    if (strcmp(name, "happy") == 0 || strcmp(name, "happy_squint") == 0 ||
        strcmp(name, "happy_squint_soft") == 0 || strcmp(name, "smile") == 0 ||
        strcmp(name, "smile_blink") == 0 || strcmp(name, "relaxed") == 0 ||
        strcmp(name, "wink") == 0 || strcmp(name, "wink_open") == 0 ||
        strcmp(name, "wink_half") == 0 || strcmp(name, "wink_closed") == 0 ||
        strcmp(name, "wink_blink") == 0 || strcmp(name, "grin") == 0 ||
        strcmp(name, "grin_blink") == 0 || strcmp(name, "heart") == 0 ||
        strcmp(name, "heart_action") == 0 || strcmp(name, "heart_small") == 0 ||
        strcmp(name, "nod") == 0 || strcmp(name, "nodding") == 0 ||
        strcmp(name, "nod_soft") == 0 || strcmp(name, "nod_down") == 0) {
        *kind = FaceKind::Happy;
        return true;
    }
    if (strcmp(name, "surprised") == 0) {
        *kind = FaceKind::Surprised;
        return true;
    }
    return false;
}

static const char* face_kind_name(FaceKind kind)
{
    switch (kind) {
        case FaceKind::SleepDark:
            return "sleep_dark";
        case FaceKind::Calm:
            return "calm";
        case FaceKind::CalmBlink:
            return "calm_blink";
        case FaceKind::Thinking:
            return "thinking";
        case FaceKind::Happy:
            return "happy";
        case FaceKind::Surprised:
            return "surprised";
        case FaceKind::Shy:
            return "shy";
    }
    return kDefaultExpression;
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
    case FaceKind::CalmBlink:
        pose.left_eye = EyeStyle::ClosedLine;
        pose.right_eye = EyeStyle::ClosedLine;
        pose.mouth = MouthShape::Closed;
        pose.mouth_width = 38;
        pose.mouth_height = 7;
        pose.mouth_radius = 4;
        break;
    case FaceKind::Shy:
        pose.mouth = MouthShape::Smile;
        pose.cheeks = CheekStyle::Shy;
        pose.mouth_y_offset = -4;
        break;
    case FaceKind::Happy:
        pose.mouth = MouthShape::SmileWide;
        pose.cheeks = CheekStyle::Shy;
        pose.mouth_y_offset = -5;
        break;
    case FaceKind::Thinking:
        pose.mouth = MouthShape::Frown;
        pose.mouth_y_offset = -3;
        break;
    case FaceKind::Surprised:
        pose.mouth = MouthShape::OpenOval;
        pose.mouth_y_offset = 2;
        break;
    case FaceKind::Calm:
    default:
        pose.mouth = MouthShape::Closed;
        pose.mouth_width = 38;
        pose.mouth_height = 7;
        pose.mouth_radius = 4;
        break;
    }
    return pose;
}

static bool face_kind_is_blink_variant(FaceKind kind)
{
    switch (kind) {
    case FaceKind::CalmBlink:
        return true;
    default:
        return false;
    }
}

static FaceKind open_face_kind_for(FaceKind kind)
{
    switch (kind) {
    case FaceKind::CalmBlink:
        return FaceKind::Calm;
    default:
        return kind;
    }
}

static bool blink_face_kind_for(FaceKind kind, FaceKind* blink_kind)
{
    if (blink_kind == nullptr) {
        return false;
    }
    switch (open_face_kind_for(kind)) {
    case FaceKind::Calm:
        *blink_kind = FaceKind::CalmBlink;
        return true;
    default:
        return false;
    }
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

static void stroke_quadratic_locked(Point start, Point control, Point end, int width, uint16_t color)
{
    Point previous = start;
    static constexpr int kSteps = 24;
    for (int i = 1; i <= kSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSteps);
        const float u = 1.0f - t;
        Point current = {
            static_cast<int>(lroundf(u * u * start.x + 2.0f * u * t * control.x + t * t * end.x)),
            static_cast<int>(lroundf(u * u * start.y + 2.0f * u * t * control.y + t * t * end.y)),
        };
        stroke_segment_locked(previous, current, width, color);
        previous = current;
    }
}

static void draw_eye_locked(Point center, EyeStyle style, uint16_t color)
{
    auto& display = draw_target_locked();
    switch (style) {
    case EyeStyle::ClosedLine:
        display.fillRoundRect(center.x - 15, center.y - 4, 30, 8, 4, color);
        break;
    case EyeStyle::Open:
    default:
        display.fillCircle(center.x, center.y, 15, color);
        break;
    }
}

static void draw_eye_pair_for_pose_locked(const FacePose& pose, uint16_t color)
{
    draw_eye_locked(anchor_point_locked(FaceLayout::kLeftEyeCenter, pose), pose.left_eye, color);
    draw_eye_locked(anchor_point_locked(FaceLayout::kRightEyeCenter, pose), pose.right_eye, color);
}

static void clear_eye_regions_locked(const FacePose& pose)
{
    auto& display = draw_target_locked();
    static constexpr int kEyeRegionHalfWidth = 34;
    static constexpr int kEyeRegionHalfHeight = 28;
    const Point left = anchor_point_locked(FaceLayout::kLeftEyeCenter, pose);
    const Point right = anchor_point_locked(FaceLayout::kRightEyeCenter, pose);
    display.fillRect(left.x - kEyeRegionHalfWidth, left.y - kEyeRegionHalfHeight,
                     kEyeRegionHalfWidth * 2, kEyeRegionHalfHeight * 2, TFT_BLACK);
    display.fillRect(right.x - kEyeRegionHalfWidth, right.y - kEyeRegionHalfHeight,
                     kEyeRegionHalfWidth * 2, kEyeRegionHalfHeight * 2, TFT_BLACK);
}

static void fill_round_rect_locked(Point center, int width, int height, int radius, uint16_t color)
{
    auto& display = draw_target_locked();
    display.fillRoundRect(center.x - width / 2, center.y - height / 2, width, height, radius, color);
}

static void draw_mouth_locked(const FacePose& pose, uint16_t color)
{
    Point center = anchor_point_locked(FaceLayout::kMouthCenter, pose, 0, pose.mouth_y_offset);
    auto& display = draw_target_locked();
    switch (pose.mouth) {
    case MouthShape::Smile:
        stroke_oval_arc_locked({center.x, center.y - 2}, 20, 17, 35, 145, 5, color);
        break;
    case MouthShape::SmileWide:
        stroke_oval_arc_locked({center.x, center.y - 3}, 34, 23, 35, 145, 6, color);
        break;
    case MouthShape::Frown:
        stroke_oval_arc_locked({center.x, center.y + 11}, 18, 14, 200, 340, 5, color);
        break;
    case MouthShape::OpenOval:
        display.fillEllipse(center.x, center.y + 4, 14, 18, color);
        display.fillEllipse(center.x, center.y + 4, 8, 11, TFT_BLACK);
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

// New-style eye drawings remapped from eye centers (92,122)/(228,122)
// onto the old anchors (88,101)/(232,101).
static void draw_styled_eyes_locked(FaceKind kind, uint16_t color)
{
    auto& display = draw_target_locked();
    const int ox = canvas_origin_x_locked();
    const int oy = canvas_origin_y_locked();
    auto point = [ox, oy](int x, int y) -> Point { return {ox + x, oy + y}; };

    switch (kind) {
    case FaceKind::Happy:
        stroke_quadratic_locked(point(64, 107), point(88, 80), point(112, 107), 7, color);
        stroke_quadratic_locked(point(208, 107), point(232, 80), point(256, 107), 7, color);
        return;
    case FaceKind::Thinking:
        display.fillCircle(ox + 88, oy + 103, 12, color);
        display.fillCircle(ox + 232, oy + 98, 18, color);
        stroke_segment_locked(point(72, 70), point(104, 70), 7, color);
        stroke_segment_locked(point(216, 57), point(248, 57), 7, color);
        return;
    case FaceKind::Surprised:
        for (Point center : {point(88, 102), point(232, 102)}) {
            display.fillCircle(center.x, center.y, 21, color);
            display.fillCircle(center.x, center.y, 13, TFT_BLACK);
        }
        stroke_quadratic_locked(point(67, 70), point(88, 55), point(109, 70), 7, color);
        stroke_quadratic_locked(point(211, 70), point(232, 55), point(253, 70), 7, color);
        return;
    case FaceKind::CalmBlink:
        draw_eye_pair_for_pose_locked(pose_for_kind(kind), color);
        return;
    case FaceKind::SleepDark:
        return;
    case FaceKind::Shy:
    case FaceKind::Calm:
    default:
        draw_eye_pair_for_pose_locked(pose_for_kind(kind), color);
        return;
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
    draw_styled_eyes_locked(kind, line);
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

static void draw_partial_eye_frame_to_display_locked(FaceKind kind)
{
    if (open_face_kind_for(current_face_kind) != open_face_kind_for(kind)) {
        return;
    }

    const FacePose pose = pose_for_kind(kind);
    const uint16_t line = line_color_locked();
    const bool has_canvas = face_canvas_ready && face_canvas != nullptr;
    if (has_canvas) {
        active_draw_target = face_canvas;
        clear_eye_regions_locked(pose);
        draw_eye_pair_for_pose_locked(pose, line);
        active_draw_target = nullptr;
    }

    active_draw_target = nullptr;
    clear_eye_regions_locked(pose);
    draw_eye_pair_for_pose_locked(pose, line);

    current_face_kind = kind;
    expression_screen_visible = true;
    sleep_dark_visible = false;
}

static void render_partial_eye_frame(FaceKind kind)
{
    DisplayLock lock;
    draw_partial_eye_frame_to_display_locked(kind);
}

static uint32_t next_eye_blink_delay_ms()
{
    eye_blink_rng_state = eye_blink_rng_state * 1664525u + 1013904223u;
    return kEyeBlinkDelayMinMs + (eye_blink_rng_state % (kEyeBlinkDelayMaxMs - kEyeBlinkDelayMinMs + 1));
}

static void notify_eye_blink_task()
{
    TaskHandle_t task = eye_blink_task_handle;
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
}

static void stop_eye_blink_loop(bool restore_open_eye = true)
{
    eye_blink_running = false;
    notify_eye_blink_task();
    for (int i = 0; i < 30 && eye_blink_task_handle != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (restore_open_eye && expression_screen_visible && !sleep_dark_visible &&
        face_kind_is_blink_variant(current_face_kind)) {
        render_partial_eye_frame(open_face_kind_for(current_face_kind));
    }
}

static void start_eye_blink_loop(FaceKind kind)
{
    FaceKind blink_kind;
    if (face_kind_is_blink_variant(kind) || !blink_face_kind_for(kind, &blink_kind)) {
        stop_eye_blink_loop(!face_kind_is_blink_variant(kind));
        return;
    }

    eye_blink_rng_state ^= static_cast<uint32_t>(M5.millis()) | 1u;
    eye_blink_running = true;
    if (eye_blink_task_handle != nullptr) {
        notify_eye_blink_task();
        return;
    }

    xTaskCreatePinnedToCore([](void*) {
        while (eye_blink_running) {
            const uint32_t delay_ms = next_eye_blink_delay_ms();
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms)) > 0) {
                continue;
            }
            if (!eye_blink_running) {
                break;
            }

            const FaceKind open_kind = open_face_kind_for(current_face_kind);
            FaceKind blink_kind;
            if (!expression_screen_visible || sleep_dark_visible || current_expression_animation != nullptr ||
                face_kind_is_blink_variant(current_face_kind) || !blink_face_kind_for(open_kind, &blink_kind)) {
                continue;
            }

            render_partial_eye_frame(blink_kind);
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kEyeBlinkClosedMs)) > 0) {
                if (eye_blink_running && open_face_kind_for(current_face_kind) == open_kind) {
                    render_partial_eye_frame(open_kind);
                }
                continue;
            }
            if (!eye_blink_running) {
                break;
            }
            if (open_face_kind_for(current_face_kind) == open_kind) {
                render_partial_eye_frame(open_kind);
            }
        }
        eye_blink_task_handle = nullptr;
        vTaskDelete(nullptr);
    }, "eye_blink", 4 * 1024, nullptr, 2, &eye_blink_task_handle, 0);
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
        stop_eye_blink_loop();
        start_expression_animation(animation);
        if (notify_temporary_task) {
            notify_temporary_expression_task();
        }
        return;
    }

    FaceKind kind = FaceKind::Calm;
    if (face_kind_from_name(expression, &kind)) {
        stop_expression_animation();
        render_expression_frame(kind);
        start_eye_blink_loop(kind);
        if (notify_temporary_task) {
            notify_temporary_expression_task();
        }
        return;
    }

    kind = FaceKind::Calm;
    stop_expression_animation();
    render_expression_frame(kind);
    start_eye_blink_loop(kind);
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

ExpressionControllerState expression_controller_current_state()
{
    ExpressionControllerState state = {};
    state.name = current_expression_animation != nullptr
        ? current_expression_animation->name
        : face_kind_name(current_face_kind);
    state.screen_visible = expression_screen_visible;
    state.sleep_dark_visible = sleep_dark_visible;
    state.animation_active = current_expression_animation != nullptr;
    return state;
}

bool expression_screen_is_visible()
{
    return expression_screen_visible;
}

bool sleep_dark_is_visible()
{
    return sleep_dark_visible;
}

void show_expression(const char* expression)
{
    show_expression_internal(expression, true);
}

void show_idle_sleep_dark_if_due(uint32_t idle_ms)
{
    if (sleep_dark_visible || current_expression_animation != nullptr) {
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
    FaceKind expected_kind;
    if (!face_kind_from_name(expression, &expected_kind)) {
        expected_kind = FaceKind::Calm;
    }
    expected_kind = open_face_kind_for(expected_kind);

    temporary_expression_until_ms = static_cast<uint32_t>(M5.millis() + duration_ms);
    show_expression_internal(expression, false);

    if (temporary_expression_task_handle != nullptr) {
        return;
    }

    xTaskCreatePinnedToCore([](void* arg) {
        const auto expected_kind = static_cast<FaceKind>(reinterpret_cast<uintptr_t>(arg));
        while (true) {
            if (open_face_kind_for(current_face_kind) != expected_kind || current_expression_animation != nullptr) {
                break;
            }

            uint32_t now = M5.millis();
            uint32_t until = temporary_expression_until_ms;
            int32_t remaining_ms = static_cast<int32_t>(until - now);
            if (remaining_ms > 0) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::min<int32_t>(remaining_ms, 250)));
                continue;
            }

            if (until == temporary_expression_until_ms && open_face_kind_for(current_face_kind) == expected_kind &&
                current_expression_animation == nullptr) {
                show_expression_internal(kDefaultExpression, false);
            }

            break;
        }
        temporary_expression_task_handle = nullptr;
        temporary_expression_until_ms = 0;
        vTaskDelete(nullptr);
    }, "temp_expr", 4 * 1024, reinterpret_cast<void*>(static_cast<uintptr_t>(expected_kind)), 2,
       &temporary_expression_task_handle, 0);
}

void start_speech_visual_feedback()
{
    if (hooks.start_speaking_light_animation != nullptr) {
        hooks.start_speaking_light_animation();
    }
}

void stop_speech_visual_feedback()
{
    if (hooks.stop_speaking_light_animation != nullptr) {
        hooks.stop_speaking_light_animation();
    }
    restore_light_after_speaking();
}
