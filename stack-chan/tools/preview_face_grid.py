#!/usr/bin/env python3
from __future__ import annotations

import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw, ImageFont


W = 320
H = 240
SCALE = 3
BG = (0, 0, 0)
LINE = (245, 248, 255)
DIM = (118, 128, 148)
BLUSH = (255, 155, 185)
TOOTH_LINE = (216, 221, 230)
GRID_BG = (18, 20, 24)
LABEL = (215, 220, 230)
GUIDE = (46, 56, 70)

LEFT_EYE = (88, 101)
RIGHT_EYE = (232, 101)
LEFT_BROW = (88, 72)
RIGHT_BROW = (232, 72)
MOUTH = (160, 152)
LEFT_CHEEK = (47, 141)
RIGHT_CHEEK = (273, 141)

OUT = Path(__file__).resolve().parent / "face_grid_preview.png"
OUT_GUIDES = Path(__file__).resolve().parent / "face_grid_preview_guides.png"
OUT_VARIANTS = Path(__file__).resolve().parent / "face_variant_preview.png"


@dataclass(frozen=True)
class Pose:
    left_eye: str = "open"
    right_eye: str = "open"
    left_brow: str = "none"
    right_brow: str = "none"
    mouth: str = "closed"
    offset_x: int = 0
    offset_y: int = 0
    mouth_y: int = 0
    mouth_width: int = 42
    mouth_height: int = 8
    mouth_radius: int = 4
    cheek: str = "none"
    label: str = "calm"


def p(point: tuple[int, int], pose: Pose = Pose(), dx: int = 0, dy: int = 0) -> tuple[int, int]:
    return (point[0] + pose.offset_x + dx, point[1] + pose.offset_y + dy)


def canvas() -> Image.Image:
    return Image.new("RGB", (W * SCALE, H * SCALE), BG)


def d(img: Image.Image) -> ImageDraw.ImageDraw:
    return ImageDraw.Draw(img)


def sc_point(point: tuple[int, int]) -> tuple[int, int]:
    return (point[0] * SCALE, point[1] * SCALE)


def sc_box(cx: int, cy: int, rx: int, ry: int) -> tuple[int, int, int, int]:
    return ((cx - rx) * SCALE, (cy - ry) * SCALE, (cx + rx) * SCALE, (cy + ry) * SCALE)


def line(draw: ImageDraw.ImageDraw, points: Iterable[tuple[int, int]], color=LINE, width=4) -> None:
    pts = [sc_point(pt) for pt in points]
    draw.line(pts, fill=color, width=width * SCALE, joint="curve")


def arc(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], start: int, end: int, color=LINE, width=4) -> None:
    draw.arc(tuple(v * SCALE for v in box), start=start, end=end, fill=color, width=width * SCALE)


def round_arc(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], start: int, end: int, color=LINE, width=4) -> None:
    left, top, right, bottom = box
    cx = (left + right) / 2
    cy = (top + bottom) / 2
    rx = (right - left) / 2
    ry = (bottom - top) / 2
    sweep = end - start
    if sweep <= 0:
        sweep += 360
    steps = max(12, math.ceil(sweep / 4))
    points = []
    for i in range(steps + 1):
        rad = math.radians(start + sweep * i / steps)
        points.append((round(cx + rx * math.cos(rad)), round(cy + ry * math.sin(rad))))

    line(draw, points, color, width)
    cap_radius = max(2, math.ceil(width / 2))
    ellipse(draw, points[0], cap_radius, cap_radius, color)
    ellipse(draw, points[-1], cap_radius, cap_radius, color)


def ellipse(draw: ImageDraw.ImageDraw, center: tuple[int, int], rx: int, ry: int, fill, outline=None, width=1) -> None:
    x, y = center
    draw.ellipse(sc_box(x, y, rx, ry), fill=fill, outline=outline, width=width * SCALE)


def draw_eye(draw: ImageDraw.ImageDraw, center: tuple[int, int], style: str) -> None:
    cx, cy = center
    if style == "closed_happy":
        round_arc(draw, (cx - 30, cy - 27, cx + 30, cy + 20), 205, 335, LINE, 7)
    elif style == "closed_relaxed":
        round_arc(draw, (cx - 31, cy - 20, cx + 31, cy + 13), 35, 145, LINE, 6)
    elif style == "closed_line":
        draw.rounded_rectangle(
            ((cx - 15) * SCALE, (cy - 4) * SCALE, (cx + 15) * SCALE, (cy + 4) * SCALE),
            radius=4 * SCALE,
            fill=LINE,
        )
    elif style == "wink_right":
        line(draw, [(cx + 20, cy - 18), (cx - 15, cy), (cx + 18, cy + 15)], LINE, 7)
    elif style == "small_open":
        radius = 12
        ellipse(draw, center, radius, radius, LINE)
    elif style == "large_open":
        radius = 16
        ellipse(draw, center, radius, radius, LINE)
    else:
        radius = 15
        ellipse(draw, center, radius, radius, LINE)


def draw_brow(draw: ImageDraw.ImageDraw, center: tuple[int, int], style: str, left: bool) -> None:
    cx, cy = center
    if style == "none":
        return
    if style in {"raised", "thinking"}:
        round_arc(draw, (cx - 23, cy - 17, cx + 23, cy + 7), 205, 335, LINE, 4)
    elif style == "soft":
        arc(draw, (cx - 18, cy - 14, cx + 18, cy + 8), 205, 335, LINE, 4)
    elif style == "tilt_in":
        if left:
            line(draw, [(cx - 17, cy - 4), (cx + 17, cy + 7)], LINE, 4)
        else:
            line(draw, [(cx - 17, cy + 7), (cx + 17, cy - 4)], LINE, 4)
    elif style == "worried":
        if left:
            arc(draw, (cx - 18, cy - 10, cx + 18, cy + 12), 205, 335, LINE, 4)
        else:
            arc(draw, (cx - 18, cy - 10, cx + 18, cy + 12), 205, 335, LINE, 4)


def rounded_rect(draw: ImageDraw.ImageDraw, center: tuple[int, int], width: int, height: int, radius: int, fill=LINE) -> None:
    cx, cy = center
    box = (
        (cx - width // 2) * SCALE,
        (cy - height // 2) * SCALE,
        (cx + width // 2) * SCALE,
        (cy + height // 2) * SCALE,
    )
    draw.rounded_rectangle(box, radius=radius * SCALE, fill=fill)


def happy_mouth(draw: ImageDraw.ImageDraw, center: tuple[int, int]) -> None:
    cx, cy = center
    left = (cx - 31) * SCALE
    right = (cx + 31) * SCALE
    flat_y = (cy - 8) * SCALE
    bottom = (cy + 27) * SCALE
    ellipse_top = 2 * flat_y - bottom
    draw.pieslice((left, ellipse_top, right, bottom), 0, 180, fill=LINE)
    draw.rounded_rectangle(
        (left, flat_y - 3 * SCALE, right, flat_y + 10 * SCALE),
        radius=9 * SCALE,
        fill=LINE,
    )


def grin_mouth(draw: ImageDraw.ImageDraw, center: tuple[int, int]) -> None:
    cx, cy = center
    rx = 49
    ry = 44
    flat_y = cy - 8
    bottom_y = cy + 36
    for y in range(flat_y, bottom_y + 1):
        t = (y - flat_y) / ry
        half = round(rx * math.sqrt(max(0.0, 1.0 - t * t)))
        draw.line(
            ((cx - half) * SCALE, y * SCALE, (cx + half) * SCALE, y * SCALE),
            fill=LINE,
            width=SCALE,
        )
    draw.rounded_rectangle(
        ((cx - rx) * SCALE, (flat_y - 4) * SCALE, (cx + rx) * SCALE, (flat_y + 16) * SCALE),
        radius=10 * SCALE,
        fill=LINE,
    )
    for x in (cx - 16, cx + 16):
        dx = (x - cx) / rx
        divider_bottom = flat_y + round(ry * math.sqrt(max(0.0, 1.0 - dx * dx))) - 3
        line(draw, [(x, flat_y + 3), (x, divider_bottom)], TOOTH_LINE, 2)


def draw_mouth(
    draw: ImageDraw.ImageDraw,
    center: tuple[int, int],
    style: str,
    width: int = 42,
    height: int = 8,
    radius: int = 4,
) -> None:
    cx, cy = center
    if style == "closed":
        rounded_rect(draw, center, width, height, radius)
    elif style == "speak1":
        rounded_rect(draw, (cx, cy + 2), 34, 14, 6)
    elif style == "speak2":
        rounded_rect(draw, (cx, cy + 4), 50, 21, 9)
    elif style == "wide":
        rounded_rect(draw, (cx, cy + 2), 56, 16, 8)
    elif style == "frown":
        arc(draw, (cx - 18, cy - 2, cx + 18, cy + 25), 200, 340, LINE, 5)
    elif style == "smile":
        arc(draw, (cx - 20, cy - 18, cx + 20, cy + 15), 35, 145, LINE, 5)
    elif style == "smile_wide":
        arc(draw, (cx - 34, cy - 25, cx + 34, cy + 20), 35, 145, LINE, 6)
    elif style == "happy_open":
        happy_mouth(draw, center)
    elif style == "grin":
        grin_mouth(draw, center)


def draw_cheek(draw: ImageDraw.ImageDraw, center: tuple[int, int], side: str, style: str) -> None:
    if style == "none":
        return
    cx, cy = center
    offsets = [-13, 0, 13]
    slant = 7 if side == "left" else -7
    for xoff in offsets:
        line(draw, [(cx + xoff - slant // 2, cy + 7), (cx + xoff + slant // 2, cy - 7)], BLUSH, 4)


def draw_guides(draw: ImageDraw.ImageDraw) -> None:
    for x, y in [LEFT_EYE, RIGHT_EYE, LEFT_BROW, RIGHT_BROW, MOUTH, LEFT_CHEEK, RIGHT_CHEEK]:
        ellipse(draw, (x, y), 2, 2, GUIDE)
    line(draw, [(LEFT_EYE[0], LEFT_EYE[1]), (RIGHT_EYE[0], RIGHT_EYE[1])], GUIDE, 1)
    line(draw, [(MOUTH[0], 40), (MOUTH[0], 205)], GUIDE, 1)


def render_pose(pose: Pose, guides: bool = True) -> Image.Image:
    img = canvas()
    draw = d(img)
    if guides:
        draw_guides(draw)
    draw_brow(draw, p(LEFT_BROW, pose), pose.left_brow, True)
    draw_brow(draw, p(RIGHT_BROW, pose), pose.right_brow, False)
    draw_eye(draw, p(LEFT_EYE, pose), pose.left_eye)
    draw_eye(draw, p(RIGHT_EYE, pose), pose.right_eye)
    draw_cheek(draw, p(LEFT_CHEEK, pose), "left", pose.cheek)
    draw_cheek(draw, p(RIGHT_CHEEK, pose), "right", pose.cheek)
    draw_mouth(
        draw,
        p(MOUTH, pose, 0, pose.mouth_y),
        pose.mouth,
        pose.mouth_width,
        pose.mouth_height,
        pose.mouth_radius,
    )
    return img.resize((W, H), Image.Resampling.LANCZOS)


def expression_poses() -> list[Pose]:
    return [
        Pose(label="calm", left_eye="open", right_eye="open", mouth="closed", mouth_width=38, mouth_height=7),
        Pose(label="calm_blink", left_eye="closed_line", right_eye="closed_line", mouth="closed", mouth_width=38, mouth_height=7),
        Pose(label="speak1", left_eye="open", right_eye="open", mouth="speak1"),
        Pose(label="speak2", left_eye="open", right_eye="open", mouth="speak2"),
        Pose(label="thinking", left_eye="open", right_eye="open", left_brow="thinking", right_brow="none", mouth="frown", mouth_y=-3),
        Pose(label="thinking_blink", left_eye="closed_line", right_eye="closed_line", left_brow="thinking", right_brow="none", mouth="frown", mouth_y=-3),
        Pose(label="shy", left_eye="open", right_eye="open", mouth="smile", mouth_y=-4, cheek="shy"),
        Pose(label="shy_blink", left_eye="closed_line", right_eye="closed_line", mouth="smile", mouth_y=-4, cheek="shy"),
        Pose(label="smile", left_eye="open", right_eye="open", mouth="smile_wide", mouth_y=-5, cheek="shy"),
        Pose(label="smile_blink", left_eye="closed_line", right_eye="closed_line", mouth="smile_wide", mouth_y=-5, cheek="shy"),
        Pose(label="happy", left_eye="closed_happy", right_eye="closed_happy", mouth="happy_open", mouth_y=-4, cheek="shy"),
        Pose(label="relaxed", left_eye="closed_relaxed", right_eye="closed_relaxed", mouth="smile", mouth_y=-6, cheek="shy"),
        Pose(label="wink_open", left_eye="open", right_eye="open", mouth="smile", mouth_y=-4, cheek="shy"),
        Pose(label="wink_blink", left_eye="open", right_eye="wink_right", mouth="smile", mouth_y=-5, cheek="shy"),
        Pose(label="grin", left_eye="open", right_eye="open", mouth="grin", mouth_y=5, cheek="shy"),
        Pose(label="grin_blink", left_eye="closed_line", right_eye="closed_line", mouth="grin", mouth_y=5, cheek="shy"),
    ]


def requested_variant_poses() -> list[Pose]:
    return [
        Pose(label="calm", left_eye="open", right_eye="open", mouth="closed", mouth_width=38, mouth_height=7),
        Pose(label="calm_blink", left_eye="closed_line", right_eye="closed_line", mouth="closed", mouth_width=38, mouth_height=7),
        Pose(label="thinking", left_eye="open", right_eye="open", left_brow="thinking", right_brow="none", mouth="frown", mouth_y=-3),
        Pose(label="thinking_blink", left_eye="closed_line", right_eye="closed_line", left_brow="thinking", right_brow="none", mouth="frown", mouth_y=-3),
        Pose(label="shy", left_eye="open", right_eye="open", mouth="smile", mouth_y=-4, cheek="shy"),
        Pose(label="shy_blink", left_eye="closed_line", right_eye="closed_line", mouth="smile", mouth_y=-4, cheek="shy"),
        Pose(label="smile", left_eye="open", right_eye="open", mouth="smile_wide", mouth_y=-5, cheek="shy"),
        Pose(label="smile_blink", left_eye="closed_line", right_eye="closed_line", mouth="smile_wide", mouth_y=-5, cheek="shy"),
        Pose(label="wink open", left_eye="open", right_eye="open", mouth="smile", mouth_y=-4, cheek="shy"),
        Pose(label="wink blink", left_eye="open", right_eye="wink_right", mouth="smile", mouth_y=-5, cheek="shy"),
        Pose(label="grin", left_eye="open", right_eye="open", mouth="grin", mouth_y=5, cheek="shy"),
        Pose(label="grin blink", left_eye="closed_line", right_eye="closed_line", mouth="grin", mouth_y=5, cheek="shy"),
    ]


def load_font(size: int) -> ImageFont.ImageFont:
    for path in [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    ]:
        if Path(path).exists():
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def compose_grid(items: list[tuple[str, Image.Image]], cols: int = 4) -> Image.Image:
    label_h = 26
    pad = 14
    cell_w = W
    cell_h = H + label_h
    rows = math.ceil(len(items) / cols)
    grid = Image.new("RGB", (cols * cell_w + (cols + 1) * pad, rows * cell_h + (rows + 1) * pad), GRID_BG)
    draw = ImageDraw.Draw(grid)
    font = load_font(15)
    for i, (name, img) in enumerate(items):
        col = i % cols
        row = i // cols
        x = pad + col * (cell_w + pad)
        y = pad + row * (cell_h + pad)
        grid.paste(img, (x, y + label_h))
        draw.text((x + 4, y + 3), name, fill=LABEL, font=font)
        draw.rectangle((x, y + label_h, x + W - 1, y + label_h + H - 1), outline=(48, 54, 64), width=1)
    return grid


def main() -> None:
    poses = expression_poses()
    items = [(pose.label, render_pose(pose, guides=False)) for pose in poses]

    grid = compose_grid(items, cols=4)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    grid.save(OUT)

    guide_items = [(pose.label, render_pose(pose, guides=True)) for pose in poses]
    compose_grid(guide_items, cols=4).save(OUT_GUIDES)

    variant_items = [(pose.label, render_pose(pose, guides=False)) for pose in requested_variant_poses()]
    compose_grid(variant_items, cols=2).save(OUT_VARIANTS)

    print(f"resolution: {W}x{H}")
    print(f"wrote: {OUT}")
    print(f"wrote: {OUT_GUIDES}")
    print(f"wrote: {OUT_VARIANTS}")


if __name__ == "__main__":
    main()
