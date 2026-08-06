#!/usr/bin/env python3
"""Generate expression previews with restored mouths for Xiaopai."""

from __future__ import annotations

import math
from pathlib import Path
from typing import Callable, Iterable

from PIL import Image, ImageDraw, ImageFont


WIDTH = 320
HEIGHT = 240
SCALE = 4

BACKGROUND = (0, 0, 0)
FEATURE = (245, 248, 255)
BLUSH = (255, 155, 185)
GUIDE = (46, 56, 70)
SHEET_BACKGROUND = (18, 20, 24)
SHEET_LABEL = (215, 220, 230)

LEFT_EYE = (88, 101)
RIGHT_EYE = (232, 101)
MOUTH = (160, 152)
LEFT_CHEEK = (47, 141)
RIGHT_CHEEK = (273, 141)

OUTPUT_DIR = Path(__file__).resolve().parent / "eye_expression_preview"
COMPARISON_PATH = OUTPUT_DIR / "eye_expression_comparison.png"
GUIDES_PATH = OUTPUT_DIR / "eye_expression_guides.png"


Point = tuple[int, int]
DrawColor = int | tuple[int, int, int]
ExpressionDrawer = Callable[[ImageDraw.ImageDraw], None]


def scaled_point(point: Point) -> Point:
    return point[0] * SCALE, point[1] * SCALE


def fill_circle(
    draw: ImageDraw.ImageDraw,
    center: Point,
    radius: int,
    color: DrawColor = FEATURE,
) -> None:
    cx, cy = scaled_point(center)
    r = radius * SCALE
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)


def fill_ellipse(
    draw: ImageDraw.ImageDraw,
    center: Point,
    rx: int,
    ry: int,
    color: DrawColor = FEATURE,
) -> None:
    cx, cy = scaled_point(center)
    draw.ellipse(
        (cx - rx * SCALE, cy - ry * SCALE, cx + rx * SCALE, cy + ry * SCALE),
        fill=color,
    )


def fill_round_rect(
    draw: ImageDraw.ImageDraw,
    center: Point,
    width: int,
    height: int,
    radius: int,
    color: DrawColor = FEATURE,
) -> None:
    cx, cy = center
    box = (
        (cx - width // 2) * SCALE,
        (cy - height // 2) * SCALE,
        (cx + width // 2) * SCALE,
        (cy + height // 2) * SCALE,
    )
    draw.rounded_rectangle(box, radius=radius * SCALE, fill=color)


def round_line(
    draw: ImageDraw.ImageDraw,
    points: Iterable[Point],
    width: int = 7,
    color: DrawColor = FEATURE,
) -> None:
    scaled = [scaled_point(point) for point in points]
    if len(scaled) < 2:
        raise ValueError("round_line requires at least two points")

    scaled_width = width * SCALE
    draw.line(scaled, fill=color, width=scaled_width, joint="curve")
    radius = scaled_width // 2
    for x, y in (scaled[0], scaled[-1]):
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=color)


def quadratic_curve(
    draw: ImageDraw.ImageDraw,
    start: Point,
    control: Point,
    end: Point,
    *,
    width: int = 7,
    steps: int = 24,
    color: DrawColor = FEATURE,
) -> None:
    points: list[Point] = []
    for index in range(steps + 1):
        t = index / steps
        one_minus_t = 1.0 - t
        x = one_minus_t * one_minus_t * start[0] + 2.0 * one_minus_t * t * control[0] + t * t * end[0]
        y = one_minus_t * one_minus_t * start[1] + 2.0 * one_minus_t * t * control[1] + t * t * end[1]
        points.append((round(x), round(y)))
    round_line(draw, points, width=width, color=color)


def oval_point(center: Point, rx: int, ry: int, angle_deg: float) -> Point:
    rad = math.radians(angle_deg)
    return (
        round(center[0] + rx * math.cos(rad)),
        round(center[1] + ry * math.sin(rad)),
    )


def oval_arc(
    draw: ImageDraw.ImageDraw,
    center: Point,
    rx: int,
    ry: int,
    start_deg: float,
    end_deg: float,
    *,
    width: int = 5,
    color: DrawColor = FEATURE,
) -> None:
    sweep = end_deg - start_deg
    if sweep <= 0:
        sweep += 360
    steps = max(12, math.ceil(sweep / 4))
    points = [
        oval_point(center, rx, ry, start_deg + sweep * index / steps)
        for index in range(steps + 1)
    ]
    round_line(draw, points, width=width, color=color)


def draw_cheeks(draw: ImageDraw.ImageDraw) -> None:
    for xoff in (-13, 0, 13):
        round_line(
            draw,
            ((LEFT_CHEEK[0] + xoff - 3, LEFT_CHEEK[1] + 7), (LEFT_CHEEK[0] + xoff + 3, LEFT_CHEEK[1] - 7)),
            width=4,
            color=BLUSH,
        )
        round_line(
            draw,
            ((RIGHT_CHEEK[0] + xoff + 4, RIGHT_CHEEK[1] + 7), (RIGHT_CHEEK[0] + xoff - 4, RIGHT_CHEEK[1] - 7)),
            width=4,
            color=BLUSH,
        )


def draw_calm(draw: ImageDraw.ImageDraw) -> None:
    fill_circle(draw, LEFT_EYE, 15)
    fill_circle(draw, RIGHT_EYE, 15)
    fill_round_rect(draw, MOUTH, 38, 7, 4)


def draw_shy(draw: ImageDraw.ImageDraw) -> None:
    fill_circle(draw, LEFT_EYE, 15)
    fill_circle(draw, RIGHT_EYE, 15)
    draw_cheeks(draw)
    oval_arc(draw, (MOUTH[0], MOUTH[1] - 6), 20, 17, 35, 145, width=5)


def draw_happy(draw: ImageDraw.ImageDraw) -> None:
    quadratic_curve(draw, (64, 107), (88, 80), (112, 107), width=7)
    quadratic_curve(draw, (208, 107), (232, 80), (256, 107), width=7)
    draw_cheeks(draw)
    oval_arc(draw, (MOUTH[0], MOUTH[1] - 8), 34, 23, 35, 145, width=6)


def draw_thinking(draw: ImageDraw.ImageDraw) -> None:
    fill_circle(draw, (88, 103), 12)
    fill_circle(draw, (232, 98), 18)
    round_line(draw, ((72, 70), (104, 70)), width=7)
    round_line(draw, ((216, 57), (248, 57)), width=7)
    oval_arc(draw, (MOUTH[0], MOUTH[1] + 8), 18, 14, 200, 340, width=5)


def draw_surprised(draw: ImageDraw.ImageDraw) -> None:
    for center in ((88, 102), (232, 102)):
        fill_circle(draw, center, 21)
        fill_circle(draw, center, 13, color=BACKGROUND)
    quadratic_curve(draw, (67, 70), (88, 55), (109, 70), width=7)
    quadratic_curve(draw, (211, 70), (232, 55), (253, 70), width=7)
    fill_ellipse(draw, (MOUTH[0], MOUTH[1] + 6), 14, 18)
    fill_ellipse(draw, (MOUTH[0], MOUTH[1] + 6), 8, 11, color=BACKGROUND)


EXPRESSIONS: tuple[tuple[str, ExpressionDrawer], ...] = (
    ("calm", draw_calm),
    ("shy", draw_shy),
    ("happy", draw_happy),
    ("thinking", draw_thinking),
    ("surprised", draw_surprised),
)


def draw_guides(draw: ImageDraw.ImageDraw) -> None:
    def guide_line(points: Iterable[Point], width: int = 1) -> None:
        round_line(draw, points, width=width, color=255)

    guide_line(((0, LEFT_EYE[1]), (WIDTH - 1, LEFT_EYE[1])))
    guide_line(((LEFT_EYE[0], 48), (LEFT_EYE[0], 180)))
    guide_line(((RIGHT_EYE[0], 48), (RIGHT_EYE[0], 180)))
    guide_line(((0, MOUTH[1]), (WIDTH - 1, MOUTH[1])))
    guide_line(((WIDTH // 2, 48), (WIDTH // 2, 180)))

    for anchor in (LEFT_EYE, RIGHT_EYE, MOUTH, LEFT_CHEEK, RIGHT_CHEEK):
        fill_circle(draw, anchor, 2, color=255)


def render_rgb(draw_expression: ExpressionDrawer, *, guides: bool = False) -> Image.Image:
    """Render expression at high scale then downsample."""
    size = (WIDTH * SCALE, HEIGHT * SCALE)
    image = Image.new("RGB", size, BACKGROUND)
    draw_expression(ImageDraw.Draw(image))
    image = image.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)

    if guides:
        guide_mask = Image.new("L", size, 0)
        draw_guides(ImageDraw.Draw(guide_mask))
        guide_mask = guide_mask.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        image = Image.composite(Image.new("RGB", image.size, GUIDE), image, guide_mask)
    return image


def load_font(size: int) -> ImageFont.ImageFont:
    for path in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    ):
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def compose_sheet(
    images: list[tuple[str, Image.Image]],
    output_path: Path,
    *,
    columns: int = 3,
) -> None:
    padding = 14
    label_height = 28
    cell_width = WIDTH
    cell_height = HEIGHT + label_height
    rows = math.ceil(len(images) / columns)
    sheet_width = columns * cell_width + (columns + 1) * padding
    sheet_height = rows * cell_height + (rows + 1) * padding
    sheet = Image.new("RGB", (sheet_width, sheet_height), SHEET_BACKGROUND)
    draw = ImageDraw.Draw(sheet)
    font = load_font(16)

    for index, (name, image) in enumerate(images):
        column = index % columns
        row = index // columns
        x = padding + column * (cell_width + padding)
        y = padding + row * (cell_height + padding)
        draw.text((x + 4, y + 3), name, fill=SHEET_LABEL, font=font)
        sheet.paste(image, (x, y + label_height))
        draw.rectangle(
            (x, y + label_height, x + WIDTH - 1, y + label_height + HEIGHT - 1),
            outline=(48, 54, 64),
            width=1,
        )

    sheet.save(output_path)


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

    clean_images: list[tuple[str, Image.Image]] = []
    guide_images: list[tuple[str, Image.Image]] = []
    for name, drawer in EXPRESSIONS:
        clean = render_rgb(drawer)
        clean.save(OUTPUT_DIR / f"{name}.png")
        clean_images.append((name, clean))
        guide_images.append((name, render_rgb(drawer, guides=True)))

    compose_sheet(clean_images, COMPARISON_PATH)
    compose_sheet(guide_images, GUIDES_PATH)

    print(f"resolution: {WIDTH}x{HEIGHT}")
    for name, _ in EXPRESSIONS:
        print(f"wrote: {OUTPUT_DIR / f'{name}.png'}")
    print(f"wrote: {COMPARISON_PATH}")
    print(f"wrote: {GUIDES_PATH}")


if __name__ == "__main__":
    main()
