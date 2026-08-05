#!/usr/bin/env python3
"""Generate mouth-free eye-and-eyebrow expression previews for Xiaopai."""

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
GUIDE = (46, 56, 70)
SHEET_BACKGROUND = (18, 20, 24)
SHEET_LABEL = (215, 220, 230)

LEFT_EYE = (92, 122)
RIGHT_EYE = (228, 122)
LEFT_BROW = (92, 84)
RIGHT_BROW = (228, 84)

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
    color: DrawColor = 255,
) -> None:
    cx, cy = scaled_point(center)
    r = radius * SCALE
    draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=color)


def round_line(
    draw: ImageDraw.ImageDraw,
    points: Iterable[Point],
    width: int = 7,
    color: DrawColor = 255,
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
) -> None:
    points: list[Point] = []
    for index in range(steps + 1):
        t = index / steps
        one_minus_t = 1.0 - t
        x = one_minus_t * one_minus_t * start[0] + 2.0 * one_minus_t * t * control[0] + t * t * end[0]
        y = one_minus_t * one_minus_t * start[1] + 2.0 * one_minus_t * t * control[1] + t * t * end[1]
        points.append((round(x * SCALE), round(y * SCALE)))

    scaled_width = width * SCALE
    draw.line(points, fill=255, width=scaled_width, joint="curve")
    radius = scaled_width // 2
    for x, y in (points[0], points[-1]):
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=255)


def draw_calm(draw: ImageDraw.ImageDraw) -> None:
    fill_circle(draw, LEFT_EYE, 16)
    fill_circle(draw, RIGHT_EYE, 16)


def draw_happy(draw: ImageDraw.ImageDraw) -> None:
    quadratic_curve(draw, (68, 128), (92, 101), (116, 128), width=7)
    quadratic_curve(draw, (204, 128), (228, 101), (252, 128), width=7)


def draw_thinking(draw: ImageDraw.ImageDraw) -> None:
    fill_circle(draw, (92, 124), 12)
    fill_circle(draw, (228, 119), 18)
    round_line(draw, ((76, 91), (108, 91)), width=7)
    round_line(draw, ((212, 78), (244, 78)), width=7)


def draw_surprised(draw: ImageDraw.ImageDraw) -> None:
    for center in ((92, 123), (228, 123)):
        fill_circle(draw, center, 21)
        fill_circle(draw, center, 13, color=0)
    quadratic_curve(draw, (71, 91), (92, 76), (113, 91), width=7)
    quadratic_curve(draw, (207, 91), (228, 76), (249, 91), width=7)


EXPRESSIONS: tuple[tuple[str, ExpressionDrawer], ...] = (
    ("calm", draw_calm),
    ("happy", draw_happy),
    ("thinking", draw_thinking),
    ("surprised", draw_surprised),
)


def draw_guides(draw: ImageDraw.ImageDraw) -> None:
    def guide_line(points: Iterable[Point], width: int = 1) -> None:
        round_line(draw, points, width=width)

    guide_line(((0, LEFT_EYE[1]), (WIDTH - 1, LEFT_EYE[1])))
    guide_line(((LEFT_EYE[0], 48), (LEFT_EYE[0], 154)))
    guide_line(((RIGHT_EYE[0], 48), (RIGHT_EYE[0], 154)))
    guide_line(((0, LEFT_BROW[1]), (WIDTH - 1, LEFT_BROW[1])))
    guide_line(((WIDTH // 2, 48), (WIDTH // 2, 154)))

    for anchor in (LEFT_EYE, RIGHT_EYE, LEFT_BROW, RIGHT_BROW):
        fill_circle(draw, anchor, 2)


def render(draw_expression: ExpressionDrawer, *, guides: bool = False) -> Image.Image:
    mask_size = (WIDTH * SCALE, HEIGHT * SCALE)
    expression_mask = Image.new("L", mask_size, 0)
    draw = ImageDraw.Draw(expression_mask)
    draw_expression(draw)
    expression_mask = expression_mask.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)

    image = Image.new("RGB", (WIDTH, HEIGHT), BACKGROUND)
    if guides:
        guide_mask = Image.new("L", mask_size, 0)
        draw_guides(ImageDraw.Draw(guide_mask))
        guide_mask = guide_mask.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
        image = Image.composite(Image.new("RGB", image.size, GUIDE), image, guide_mask)

    return Image.composite(Image.new("RGB", image.size, FEATURE), image, expression_mask)


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
    columns: int = 2,
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
        clean = render(drawer)
        clean.save(OUTPUT_DIR / f"{name}.png")
        clean_images.append((name, clean))
        guide_images.append((name, render(drawer, guides=True)))

    compose_sheet(clean_images, COMPARISON_PATH)
    compose_sheet(guide_images, GUIDES_PATH)

    print(f"resolution: {WIDTH}x{HEIGHT}")
    for name, _ in EXPRESSIONS:
        print(f"wrote: {OUTPUT_DIR / f'{name}.png'}")
    print(f"wrote: {COMPARISON_PATH}")
    print(f"wrote: {GUIDES_PATH}")


if __name__ == "__main__":
    main()
