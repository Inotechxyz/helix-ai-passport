#!/usr/bin/env python3
"""Generate the LVGL font that renders IPA on the study screen.

The default LVGL fonts used elsewhere in this application do not carry the
International Phonetic Alphabet, so the pronunciation line needs its own small
subset. This script reuses the rasterizer and LVGL table emitter from
gen_cjk_font.py and fills it with:

  * ASCII U+0020..U+007E (contiguous cmap)
  * the IPA symbols produced by ls_pron_to_ipa() (sparse cmap)

The generated table is LVGL fmt_txt PLAIN, 4bpp, stride = 0, matching the CJK
font. DejaVu Sans (Bitstream Vera / public-domain derived) is used by default
because it covers the IPA Extensions block completely; the glyphs are embedded
in the firmware.

Usage:
  python3 tools/gen_ipa_font.py
  python3 tools/gen_ipa_font.py --check-only
"""

from __future__ import annotations

import argparse
import os
import sys

try:
    from PIL import ImageFont
except ImportError:  # pragma: no cover - actionable message for first run
    sys.exit("Pillow is required: python3 -m pip install pillow")

from gen_cjk_font import ASCII_END, ASCII_START, emit_c

DEFAULT_FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
DEFAULT_OUTPUT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "main",
    "langstudy_ipa_16.c",
)
FONT_SYMBOL = "langstudy_ipa_16"
FONT_GUARD = "LV_FONT_LANGSTUDY_IPA_16"

# Superset of every non-ASCII code point that ls_pron_to_ipa() can emit. Keep
# it covering ipa_symbol() in main/langstudy_phoneme.c. A few symbols are now
# mapped to plain-ASCII lookalikes for legibility (ɪ->i, ɚ/ɝ->ər) but stay in
# the table so an older stored pronunciation or a future mapping still renders.
IPA_SYMBOLS = [
    0x00E6,  # æ
    0x00F0,  # ð
    0x014B,  # ŋ
    0x0251,  # ɑ
    0x0254,  # ɔ
    0x0259,  # ə
    0x025A,  # ɚ
    0x025B,  # ɛ
    0x025D,  # ɝ
    0x0261,  # ɡ
    0x026A,  # ɪ
    0x0283,  # ʃ
    0x028A,  # ʊ
    0x028C,  # ʌ
    0x0292,  # ʒ
    0x02C8,  # ˈ primary stress
    0x02CC,  # ˌ secondary stress
    0x02D0,  # ː length mark
    0x03B8,  # θ
]


def glyph_inventory() -> list[int]:
    return list(range(ASCII_START, ASCII_END + 1)) + IPA_SYMBOLS


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", default=DEFAULT_FONT, help="TTF/TTC path")
    parser.add_argument("--size", type=int, default=16, help="pixel size")
    parser.add_argument("--output", default=DEFAULT_OUTPUT, help="output .c file")
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
    if line_height > 20:
        print(
            f"warning: line_height={line_height} exceeds the 20 px label box",
            file=sys.stderr,
        )

    # Fail early if the chosen font is missing any symbol instead of silently
    # emitting a blank bitmap that only shows up on the device.
    missing: list[int] = []
    for codepoint in IPA_SYMBOLS:
        mask, _ = font.getmask2(chr(codepoint), mode="L", anchor="ls")
        if mask.size == (0, 0):
            missing.append(codepoint)
    if missing:
        sample = " ".join(f"U+{p:04X} {chr(p)}" for p in missing)
        raise SystemExit(f"font lacks {len(missing)} IPA glyph(s): {sample}")

    coverage = (
        f"ASCII U+{ASCII_START:04X}..U+{ASCII_END:04X} plus "
        f"{len(IPA_SYMBOLS)} IPA symbols"
    )

    if args.check_only:
        print(
            f"inventory: {len(glyph_inventory())} glyphs, "
            f"line_height={line_height}, base_line={base_line}"
        )
        return 0

    emit_c(
        args.output,
        args.font,
        args.size,
        line_height,
        base_line,
        font,
        points=glyph_inventory(),
        font_symbol=FONT_SYMBOL,
        font_guard=FONT_GUARD,
        coverage=coverage,
        generator="tools/gen_ipa_font.py",
        license_note="Bitstream Vera/DejaVu license.",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
