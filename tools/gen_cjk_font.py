#!/usr/bin/env python3
"""Generate the LVGL 4bpp CJK subset used by the translation label.

The application receives arbitrary Chinese translations from DeepSeek, so a
fixed set of glyphs from LVGL's built-in Source Han Sans subset is not enough.
This script rasterizes a fixed character contract once, offline, and emits a
plain C font table:

  * ASCII U+0020..U+007E (contiguous cmap)
  * every character encodable in GB2312 (sparse cmap)

Rendered output is LVGL fmt_txt PLAIN, 4bpp, glyphs packed as a continuous
high-nibble-first stream with no row padding (stride = 0). The table uses
Noto Sans SC (SIL Open Font License) by default so the generated bitmaps can be
redistributed with the firmware.

Usage:
  python3 tools/gen_cjk_font.py                 # write the default output
  python3 tools/gen_cjk_font.py --check-only    # print coverage/size stats
  python3 tools/gen_cjk_font.py --text FILE ... # fail on characters not in
                                                # the generated inventory
"""

from __future__ import annotations

import argparse
import os
import sys
from typing import Iterable

try:
    from PIL import ImageFont
except ImportError:  # pragma: no cover - actionable message for first run
    sys.exit("Pillow is required: python3 -m pip install pillow")

ASCII_START = 0x20
ASCII_END = 0x7E
DEFAULT_FONT = "/mnt/c/Windows/Fonts/NotoSansSC-VF.ttf"
DEFAULT_OUTPUT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "main",
    "langstudy_cjk_16.c",
)
FONT_SYMBOL = "langstudy_cjk_16"
FONT_GUARD = "LV_FONT_LANGSTUDY_CJK_16"
MAX_BITMAP_INDEX = 0xFFFFF  # lv_font_fmt_txt_glyph_dsc_t.bitmap_index is 20 bit


def gb2312_codepoints() -> set[int]:
    """Every Unicode character reachable through a valid GB2312 byte pair."""
    points: set[int] = set()
    for high in range(0xA1, 0xF8):
        for low in range(0xA1, 0xFF):
            try:
                text = bytes((high, low)).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(text) == 1:
                points.add(ord(text))
    return points


def glyph_inventory() -> list[int]:
    points = {p for p in gb2312_codepoints() if not ASCII_START <= p <= ASCII_END}
    points.update(range(ASCII_START, ASCII_END + 1))
    return sorted(points)


def rasterize(font: ImageFont.FreeTypeFont, codepoint: int):
    ch = chr(codepoint)
    mask, offset = font.getmask2(ch, mode="L", anchor="ls")
    adv_w = int(round(font.getlength(ch) * 16))
    if adv_w < 0:
        adv_w = 0
    box_w, box_h = mask.size
    ofs_x, ofs_y_top = offset
    if box_w == 0 or box_h == 0:
        return adv_w, 0, 0, 0, 0, b""
    ofs_y = -(ofs_y_top + box_h)

    # getmask2() returns an ImagingCore in Pillow's typed API; bytes() gives
    # one 8-bit coverage value per pixel.
    pixels = bytes(mask)
    if len(pixels) != box_w * box_h:
        raise SystemExit(
            f"U+{codepoint:04X}: mask size {len(pixels)} != {box_w}x{box_h}"
        )
    packed = bytearray()
    for i in range(0, len(pixels), 2):
        high = pixels[i] >> 4
        low = pixels[i + 1] >> 4 if i + 1 < len(pixels) else 0
        packed.append((high << 4) | low)
    return int(adv_w), int(box_w), int(box_h), int(ofs_x), int(ofs_y), bytes(packed)


def read_text_codepoints(paths: Iterable[str]) -> set[int]:
    needed: set[int] = set()
    for path in paths:
        with open(path, "r", encoding="utf-8") as handle:
            for ch in handle.read():
                if ord(ch) >= 0x80:
                    needed.add(ord(ch))
    return needed


def emit_c(
    out_path: str,
    font_path: str,
    size: int,
    line_height: int,
    base_line: int,
    font: ImageFont.FreeTypeFont,
    points: list[int] | None = None,
    font_symbol: str = FONT_SYMBOL,
    font_guard: str = FONT_GUARD,
    coverage: str | None = None,
    generator: str = "tools/gen_cjk_font.py",
    license_note: str = "SIL Open Font License 1.1.",
) -> None:
    if points is None:
        points = glyph_inventory()
    points = sorted(points)
    ascii_points = [p for p in points if ASCII_START <= p <= ASCII_END]
    cjk_points = [p for p in points if p > ASCII_END]
    if not cjk_points:
        raise SystemExit("glyph inventory needs at least one non-ASCII point")
    if coverage is None:
        coverage = (
            f"ASCII U+{ASCII_START:04X}..U+{ASCII_END:04X} plus GB2312 "
            f"({len(cjk_points)} CJK/symbol glyphs)"
        )
    if len(ascii_points) != ASCII_END - ASCII_START + 1:
        raise SystemExit("ASCII range is incomplete")

    bitmap = bytearray()
    descriptors: list[tuple[int, int, int, int, int, int, int]] = []
    rendered = 0
    for codepoint in points:
        adv_w, box_w, box_h, ofs_x, ofs_y, data = rasterize(font, codepoint)
        descriptors.append(
            (len(bitmap), adv_w, box_w, box_h, ofs_x, ofs_y, codepoint)
        )
        if data:
            rendered += 1
            bitmap.extend(data)
        if len(bitmap) > MAX_BITMAP_INDEX:
            raise SystemExit(
                "glyph bitmap exceeds the 20-bit bitmap_index limit; "
                "reduce the size or the inventory"
            )

    cjk_start = 1 + len(ascii_points)
    cjk_base = cjk_points[0]
    cjk_range = cjk_points[-1] - cjk_base + 1
    if cjk_range > 0xFFFF:
        raise SystemExit("sparse cmap range does not fit uint16")
    if len(cjk_points) > 0xFFFF:
        raise SystemExit("sparse cmap list does not fit uint16")

    with open(out_path, "w", encoding="utf-8", newline="\n") as out:
        out.write(
            f"// Generated by {generator}. Do not edit by hand.\n"
            f"// Source font: {os.path.basename(font_path)} ({size} px), "
            f"{license_note}\n"
            f"// Coverage: {coverage}.\n"
            f"// Size: {len(bitmap)} bitmap bytes, {rendered} rendered glyphs.\n"
            "#include \"lvgl.h\"\n\n"
            f"#ifndef {font_guard}\n#define {font_guard} 1\n#endif\n\n"
            f"#if {font_guard}\n\n"
        )

        out.write("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")
        for i in range(0, len(bitmap), 16):
            chunk = bitmap[i : i + 16]
            out.write("    " + ", ".join(f"0x{b:02x}" for b in chunk) + ",\n")
        out.write("};\n\n")

        out.write("static const uint16_t unicode_list_1[] = {\n")
        for i in range(0, len(cjk_points), 12):
            chunk = cjk_points[i : i + 12]
            vals = ", ".join(f"0x{p - cjk_base:04x}" for p in chunk)
            out.write("    " + vals + ",\n")
        out.write("};\n\n")

        out.write("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
        out.write(
            "    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, "
            ".ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,\n"
        )
        for index, adv_w, box_w, box_h, ofs_x, ofs_y, codepoint in descriptors:
            out.write(
                f"    {{.bitmap_index = {index}, .adv_w = {adv_w}, "
                f".box_w = {box_w}, .box_h = {box_h}, .ofs_x = {ofs_x}, "
                f".ofs_y = {ofs_y}}}, /* U+{codepoint:04X} */\n"
            )
        out.write("};\n\n")

        out.write(
            "static const lv_font_fmt_txt_cmap_t cmaps[] = {\n"
            "    {\n"
            f"        .range_start = {ASCII_START}, "
            f".range_length = {ASCII_END - ASCII_START + 1}, "
            ".glyph_id_start = 1,\n"
            "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, "
            ".list_length = 0,\n"
            "        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n"
            "    },\n"
            "    {\n"
            f"        .range_start = {cjk_base}, .range_length = {cjk_range}, "
            f".glyph_id_start = {cjk_start},\n"
            "        .unicode_list = unicode_list_1, "
            ".glyph_id_ofs_list = NULL,\n"
            f"        .list_length = {len(cjk_points)}, "
            ".type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n"
            "    }\n"
            "};\n\n"
        )

        out.write(
            "static const lv_font_fmt_txt_dsc_t font_dsc = {\n"
            "    .glyph_bitmap = glyph_bitmap,\n"
            "    .glyph_dsc = glyph_dsc,\n"
            "    .cmaps = cmaps,\n"
            "    .kern_dsc = NULL,\n"
            "    .kern_scale = 0,\n"
            "    .cmap_num = 2,\n"
            "    .bpp = 4,\n"
            "    .kern_classes = 0,\n"
            "    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,\n"
            "    .stride = 0,\n"
            "};\n\n"
            f"const lv_font_t {font_symbol} = {{\n"
            "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n"
            "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n"
            f"    .line_height = {line_height},\n"
            f"    .base_line = {base_line},\n"
            "    .subpx = LV_FONT_SUBPX_NONE,\n"
            "    .underline_position = -2,\n"
            "    .underline_thickness = 1,\n"
            "    .dsc = &font_dsc,\n"
            "};\n\n"
            f"#endif /* {font_guard} */\n"
        )

    print(
        f"wrote {out_path}: {len(points)} glyphs, {len(bitmap)} bitmap bytes, "
        f"line_height={line_height}, base_line={base_line}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", default=DEFAULT_FONT, help="TTF/TTC path")
    parser.add_argument("--size", type=int, default=16, help="pixel size")
    parser.add_argument("--output", default=DEFAULT_OUTPUT, help="output .c file")
    parser.add_argument(
        "--text",
        action="append",
        default=[],
        metavar="FILE",
        help="verify every non-ASCII character in FILE is covered (repeatable)",
    )
    parser.add_argument(
        "--check-only",
        action="store_true",
        help="report coverage without writing the C file",
    )
    args = parser.parse_args()

    if not os.path.exists(args.font):
        raise SystemExit(f"font not found: {args.font}")

    font = ImageFont.truetype(args.font, args.size)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent
    base_line = descent
    # Keep the label compact: two 20 px lines are allocated on the study page.
    if line_height != 20:
        print(
            f"warning: {os.path.basename(args.font)} reports "
            f"line_height={line_height} at {args.size}px (expected 20)",
            file=sys.stderr,
        )

    points = set(glyph_inventory())
    missing: set[int] = set()
    if args.text:
        needed = read_text_codepoints(args.text)
        missing = needed - points
        if missing:
            sample = " ".join(chr(p) for p in sorted(missing)[:40])
            print(
                f"missing {len(missing)} character(s): {sample}",
                file=sys.stderr,
            )
        else:
            print(f"text coverage OK: {len(needed)} non-ASCII characters")

    if args.check_only:
        print(f"inventory: {len(points)} glyphs")
        return 1 if missing else 0

    emit_c(args.output, args.font, args.size, line_height, base_line, font)
    return 1 if missing else 0


if __name__ == "__main__":
    raise SystemExit(main())
