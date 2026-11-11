#!/usr/bin/env python3
"""Generate the 240x192 cyber-pixel launch-screen artwork.

The splash is shown for five seconds on every cold boot and reboot. It carries
the dedication "Gift for my beloved daughters, April and Abby." and a scene of
the whole family (father, mother, April and Abby) holding hands in front of a
neon city.

The scene is drawn procedurally at 120x96 and scaled 2x with nearest-neighbour
sampling, so every pixel stays a crisp square on the 240x320 panel. The
generated file stores little-endian RGB565 bytes plus an ``lv_image_dsc_t`` so
LVGL can draw it straight from flash without a decoder.

Usage:
  python3 tools/gen_splash.py
  python3 tools/gen_splash.py --check-only
"""

from __future__ import annotations

import argparse
import os
import random
import sys

try:
    from PIL import Image, ImageDraw, ImageFilter
except ImportError:  # pragma: no cover - actionable message for first run
    sys.exit("Pillow is required: python3 -m pip install pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT_C = os.path.join(ROOT, "main", "langstudy_splash_img.c")
OUTPUT_H = os.path.join(ROOT, "main", "langstudy_splash_img.h")
PREVIEW = os.path.join(ROOT, "build", "splash_preview.png")

BASE_W = 120
BASE_H = 96
SCALE = 2
WIDTH = BASE_W * SCALE
HEIGHT = BASE_H * SCALE

SKY_TOP = (4, 5, 20)
SKY_HORIZON = (42, 17, 74)
CITY_FAR = (18, 10, 44)
CITY_NEAR = (8, 6, 20)
GROUND = (6, 5, 16)
GRID = (12, 74, 96)
SILHOUETTE = (9, 11, 26, 255)
NEON_MAGENTA = (255, 72, 190)
NEON_CYAN = (72, 240, 255)
NEON_VIOLET = (150, 112, 255)
NEON_DEEP = (104, 62, 208)
NEON_GOLD = (255, 186, 84)
WINDOW_COLORS = ((0, 196, 224), (255, 92, 190), (255, 196, 96))

# Rim-light bands, one per family member, keyed by x. Left-edge and right-edge
# colors differ so the four people read as distinct without extra detail.
RIM_BANDS = (
    (0, 42, NEON_MAGENTA, NEON_CYAN),    # father
    (42, 58, NEON_VIOLET, NEON_CYAN),    # mother
    (58, 72, NEON_GOLD, NEON_MAGENTA),   # April
    (72, BASE_W, NEON_CYAN, NEON_GOLD),  # Abby
)


def lerp(a: int, b: int, t: float) -> int:
    return int(round(a + (b - a) * t))


def draw_sky(img: Image.Image, draw: ImageDraw.ImageDraw, rng: random.Random) -> None:
    for y in range(65):
        t = (y / 64.0) ** 1.4
        color = (
            lerp(SKY_TOP[0], SKY_HORIZON[0], t),
            lerp(SKY_TOP[1], SKY_HORIZON[1], t),
            lerp(SKY_TOP[2], SKY_HORIZON[2], t),
        )
        draw.line([(0, y), (BASE_W, y)], fill=color)

    for _ in range(34):
        x = rng.randrange(0, BASE_W)
        y = rng.randrange(0, 44)
        bright = rng.choice((90, 130, 170, 210))
        tint = rng.choice(((bright, bright, bright),
                           (bright, bright - 20, bright + 20),
                           (bright - 20, bright, bright)))
        draw.point((x, y), fill=tint)


def draw_moon(img: Image.Image, draw: ImageDraw.ImageDraw) -> None:
    """A glowing neon moon behind the skyline, well above the family."""
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    gdraw.ellipse([78, 12, 108, 42], fill=(255, 90, 200, 100))
    glow = glow.filter(ImageFilter.GaussianBlur(5))
    img.alpha_composite(glow)
    draw.ellipse([84, 18, 102, 36], fill=(255, 148, 214))
    # Crescent shadow keeps it from looking like a flat sticker.
    draw.ellipse([88, 14, 106, 32], fill=(30, 12, 56))


def draw_skyline(draw: ImageDraw.ImageDraw, rng: random.Random) -> None:
    # Far row: soft purple blocks with a few lit windows.
    x = -4
    while x < BASE_W + 4:
        w = rng.randrange(6, 15)
        top = rng.randrange(38, 52)
        draw.rectangle([x, top, x + w - 1, 65], fill=CITY_FAR)
        for wy in range(top + 2, 64, 3):
            for wx in range(x + 1, x + w - 1, 3):
                if rng.random() < 0.18:
                    draw.point((wx, wy), fill=rng.choice(WINDOW_COLORS))
        x += w + rng.randrange(1, 4)

    # Near row: darker blocks, sparse bright windows and two antennas.
    x = -6
    while x < BASE_W + 6:
        w = rng.randrange(8, 18)
        top = rng.randrange(50, 62)
        draw.rectangle([x, top, x + w - 1, 65], fill=CITY_NEAR)
        for wy in range(top + 2, 64, 4):
            for wx in range(x + 2, x + w - 2, 4):
                if rng.random() < 0.12:
                    draw.point((wx, wy), fill=rng.choice(WINDOW_COLORS))
        if rng.random() < 0.22:
            ax = x + w // 2
            draw.line([(ax, top), (ax, top - rng.randrange(3, 7))],
                      fill=CITY_NEAR)
            draw.point((ax, top - rng.randrange(1, 3)), fill=NEON_MAGENTA)
        x += w + rng.randrange(0, 3)


def draw_horizon_and_ground(draw: ImageDraw.ImageDraw) -> None:
    # Neon band at the horizon, with a brighter core two rows wide.
    draw.line([(0, 64), (BASE_W, 64)], fill=(88, 40, 130))
    draw.line([(0, 65), (BASE_W, 65)], fill=(160, 70, 190))

    draw.rectangle([0, 66, BASE_W - 1, BASE_H - 1], fill=GROUND)

    # Perspective grid lines converge at the centre of the horizon.
    vanish = (BASE_W // 2, 65)
    for xb in range(-60, BASE_W + 61, 12):
        draw.line([vanish, (xb, BASE_H - 1)], fill=(9, 46, 62))
    y = 68
    step = 3
    while y < BASE_H:
        draw.line([(0, y), (BASE_W - 1, y)], fill=GRID)
        y += step
        step += 2


def draw_hand_glow(img: Image.Image, hands: list[tuple[float, float]]) -> None:
    glow = Image.new("RGBA", img.size, (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(glow)
    for hx, hy in hands:
        gdraw.ellipse([hx - 2, hy - 2, hx + 2, hy + 2],
                      fill=(255, 120, 220, 190))
    glow = glow.filter(ImageFilter.GaussianBlur(1.6))
    img.alpha_composite(glow)


def draw_family() -> tuple[Image.Image, list[tuple[int, int]]]:
    """Return the silhouette layer and the four hand positions."""
    layer = Image.new("RGBA", (BASE_W, BASE_H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)

    # (cx, head_top, head_w, shoulder_y, hip_y, torso_half, leg_half)
    figures = [
        (31, 38, 8, 49, 73, 6, 2),   # father: broad shoulders
        (48, 43, 7, 53, 73, 4, 2),   # mother: hair and skirt
        (65, 53, 6, 61, 77, 3, 1),   # April: pigtails
        (79, 58, 6, 65, 79, 3, 1),   # Abby: pigtails
    ]
    hand_y = [64, 68, 70]
    hands: list[tuple[int, int]] = []

    # Mother's hair behind the head and shoulders.
    mother_cx, mother_top, _, mother_shoulder, mother_hip, _, _ = figures[1]
    draw.polygon(
        [(mother_cx - 4, mother_top + 2), (mother_cx + 4, mother_top + 2),
         (mother_cx + 5, mother_shoulder + 6),
         (mother_cx - 5, mother_shoulder + 6)],
        fill=SILHOUETTE,
    )
    # Mother's skirt widens the silhouette between waist and hip.
    draw.polygon(
        [(mother_cx - 4, mother_hip - 10), (mother_cx + 4, mother_hip - 10),
         (mother_cx + 6, mother_hip + 1), (mother_cx - 6, mother_hip + 1)],
        fill=SILHOUETTE,
    )

    for i, (cx, top, head_w, shoulder, hip, torso_half, leg_half) in \
            enumerate(figures):
        half = head_w // 2
        draw.ellipse([cx - half, top, cx + half, top + head_w], fill=SILHOUETTE)
        if i >= 2:
            # Pigtails make the two daughters read as children at a glance.
            draw.ellipse([cx - half - 2, top + 2, cx - half + 1, top + 5],
                         fill=SILHOUETTE)
            draw.ellipse([cx + half - 1, top + 2, cx + half + 2, top + 5],
                         fill=SILHOUETTE)
        # Neck.
        draw.rectangle([cx - 1, top + head_w - 1, cx + 1, shoulder + 1],
                       fill=SILHOUETTE)
        # Torso: shoulders wide, waist slightly narrower.
        draw.polygon(
            [(cx - torso_half, shoulder), (cx + torso_half, shoulder),
             (cx + torso_half - 1, hip), (cx - torso_half + 1, hip)],
            fill=SILHOUETTE,
        )
        # Legs.
        draw.rectangle([cx - leg_half - 1, hip, cx - 1, BASE_H - 8],
                       fill=SILHOUETTE)
        draw.rectangle([cx + 1, hip, cx + leg_half + 1, BASE_H - 8],
                       fill=SILHOUETTE)

    # Arms reach down to the joined hands between neighbouring figures.
    pairs = [(figures[0], figures[1], hand_y[0]),
             (figures[1], figures[2], hand_y[1]),
             (figures[2], figures[3], hand_y[2])]
    for left, right, y in pairs:
        lx = left[0] + left[5] - 1
        rx = right[0] - right[5] + 1
        draw.line([(lx, left[3] + 3), ((lx + rx) // 2, y)], fill=SILHOUETTE,
                  width=2)
        draw.line([(rx, right[3] + 3), ((lx + rx) // 2, y)], fill=SILHOUETTE,
                  width=2)
        hands.append(((lx + rx) // 2, y))

    return layer, hands


def rim_colors(x: int) -> tuple[tuple[int, int, int], tuple[int, int, int]]:
    for start, end, left, right in RIM_BANDS:
        if start <= x < end:
            return left, right
    return NEON_MAGENTA, NEON_CYAN


def neon_rim(layer: Image.Image) -> Image.Image:
    """One-pixel rim light, tinted per family member along the x axis."""
    rim = Image.new("RGBA", layer.size, (0, 0, 0, 0))
    src = layer.load()
    dst = rim.load()
    w, h = layer.size
    for y in range(h):
        for x in range(w):
            if src[x, y][3] == 0:
                continue
            left, right = rim_colors(x)
            if x == 0 or src[x - 1, y][3] == 0:
                dst[x, y] = (*left, 255)
            elif x == w - 1 or src[x + 1, y][3] == 0:
                dst[x, y] = (*right, 255)
            elif y == 0 or src[x, y - 1][3] == 0:
                dst[x, y] = (*left, 255)
            elif y == h - 1 or src[x, y + 1][3] == 0:
                dst[x, y] = (*right, 255)
    return rim


def build_scene() -> Image.Image:
    rng = random.Random(7)
    img = Image.new("RGBA", (BASE_W, BASE_H), (0, 0, 0, 255))
    draw = ImageDraw.Draw(img)

    draw_sky(img, draw, rng)
    draw_moon(img, draw)
    draw_skyline(draw, rng)
    draw_horizon_and_ground(draw)

    # Warm/violet halo behind the family separates them from the skyline.
    halo = Image.new("RGBA", img.size, (0, 0, 0, 0))
    hdraw = ImageDraw.Draw(halo)
    hdraw.ellipse([18, 30, 96, 92], fill=(120, 40, 150, 120))
    halo = halo.filter(ImageFilter.GaussianBlur(6))
    img.alpha_composite(halo)

    family, hands = draw_family()
    rim = neon_rim(family)

    glow = rim.filter(ImageFilter.GaussianBlur(2.2))
    img.alpha_composite(glow)
    img.alpha_composite(family)
    img.alpha_composite(rim)
    draw_hand_glow(img, hands)

    # Bright 2x2 hand pixels so the linked hands read at a glance.
    draw = ImageDraw.Draw(img)
    for i, (hx, hy) in enumerate(hands):
        color = NEON_MAGENTA if i % 2 == 0 else NEON_CYAN
        draw.rectangle([hx, hy, hx + 1, hy + 1], fill=(*color, 255))

    # A few warm pixel sparkles above the family; the scene stays calm.
    for sx, sy, color in ((20, 30, NEON_GOLD), (98, 28, NEON_CYAN),
                          (70, 22, NEON_MAGENTA)):
        draw.point((sx, sy), fill=(*color, 255))
        draw.point((sx - 1, sy), fill=(*color, 180))
        draw.point((sx + 1, sy), fill=(*color, 180))
        draw.point((sx, sy - 1), fill=(*color, 180))
        draw.point((sx, sy + 1), fill=(*color, 180))

    return img.convert("RGB")


def to_rgb565_bytes(img: Image.Image) -> bytes:
    out = bytearray()
    for r, g, b in img.getdata():
        value = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out.append(value & 0xFF)
        out.append(value >> 8)
    return bytes(out)


def emit_c(pixels: bytes) -> str:
    lines = [
        "// Generated by tools/gen_splash.py. Do not edit by hand.",
        "// 240x192 RGB565 launch-screen artwork; see tools/gen_splash.py.",
        '#include "langstudy_splash_img.h"',
        "",
        # LVGL reads RGB565 as 16-bit words; keep the array word-aligned.
        "static const uint8_t s_langstudy_splash_map[] "
        "__attribute__((aligned(4))) = {",
    ]
    for i in range(0, len(pixels), 16):
        chunk = pixels[i:i + 16]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    lines += [
        "};",
        "",
        "const lv_image_dsc_t langstudy_splash_img = {",
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
        "    .header.cf = LV_COLOR_FORMAT_RGB565,",
        f"    .header.w = {WIDTH},",
        f"    .header.h = {HEIGHT},",
        "    .data_size = sizeof(s_langstudy_splash_map),",
        "    .data = s_langstudy_splash_map,",
        "};",
        "",
    ]
    return "\n".join(lines)


HEADER = """// Generated by tools/gen_splash.py. Do not edit by hand.
#pragma once

#include "lvgl.h"

// 240x192 RGB565 art shown by ls_splash_show().
extern const lv_image_dsc_t langstudy_splash_img;
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true",
                        help="verify the committed files match the script")
    args = parser.parse_args()

    scene = build_scene().resize((WIDTH, HEIGHT), Image.NEAREST)
    source = emit_c(to_rgb565_bytes(scene))

    if args.check_only:
        stale = []
        for path, expected in ((OUTPUT_C, source), (OUTPUT_H, HEADER)):
            try:
                with open(path, "r", encoding="utf-8", newline="") as handle:
                    if handle.read() != expected:
                        stale.append(path)
            except OSError:
                stale.append(path)
        if stale:
            for path in stale:
                print(f"STALE: {os.path.relpath(path, ROOT)}", file=sys.stderr)
            return 1
        print("OK: splash image is up to date")
        return 0

    with open(OUTPUT_C, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(source)
    with open(OUTPUT_H, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(HEADER)

    os.makedirs(os.path.dirname(PREVIEW), exist_ok=True)
    scene.save(PREVIEW)
    print(f"Wrote {os.path.relpath(OUTPUT_C, ROOT)} "
          f"({len(source)} bytes of C, {WIDTH}x{HEIGHT})")
    print(f"Preview: {os.path.relpath(PREVIEW, ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
