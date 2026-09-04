#!/usr/bin/env python3
"""Generate an LVGL 8 bitmap font of monochrome emoji.

The display is 1bpp e-paper, so colour emoji fonts are of no use here: what is
wanted is a silhouette. Noto Emoji is drawn as monochrome outlines, which is
exactly right, and is OFL licensed so the rendered glyphs can ship in firmware.

lv_font_conv is a Node tool and is not available in this environment, so this
renders with Pillow and emits the LVGL structures directly. The format is the
one in lib/lvgl/src/font/lv_font_fmt_txt.h:

  * each glyph's rows are a continuous bitstream of box_w bits, MSB first, with
    no padding between rows; only the glyph as a whole is padded to a byte, as
    `bitmap_index` counts bytes
  * `ofs_y` is the distance from the baseline to the bottom of the ink, positive
    when the ink sits above the baseline. The renderer places the glyph top at
    `baseline - box_h - ofs_y` (lv_draw_sw_letter.c)
  * `adv_w` is in 1/16ths of a pixel

Usage:
    pip install pillow fonttools
    curl -sSLo NotoEmoji.ttf \\
      "https://github.com/google/fonts/raw/main/ofl/notoemoji/NotoEmoji%5Bwght%5D.ttf"
    python tools/gen_emoji_font.py NotoEmoji.ttf 13 src/assets/Font_Emoji_16.c Font_Emoji_16

    The render size is 13, not 16: DILATE grows each glyph by a pixel on every
    side, so 13 lands at roughly the 16px line the UI text sits on.
"""

import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont
from fontTools.ttLib import TTFont

# The Unicode blocks worth carrying. Regional indicators are left out: they only
# mean anything in pairs and would otherwise render as stray letters.
EMOJI_RANGES = [
    (0x00A9, 0x00AE), (0x203C, 0x203C), (0x2049, 0x2049), (0x2122, 0x2122),
    (0x2139, 0x2139), (0x2194, 0x21AA), (0x231A, 0x231B), (0x2328, 0x2328),
    (0x23CF, 0x23FA), (0x24C2, 0x24C2), (0x25AA, 0x25FE), (0x2600, 0x27BF),
    (0x2934, 0x2935), (0x2B00, 0x2BFF), (0x3030, 0x3030), (0x303D, 0x303D),
    (0x3297, 0x3299),
    (0x1F000, 0x1F0FF), (0x1F200, 0x1F2FF), (0x1F300, 0x1F5FF),
    (0x1F600, 0x1F64F), (0x1F680, 0x1F6FF), (0x1F780, 0x1F7FF),
    (0x1F900, 0x1F9FF), (0x1FA00, 0x1FAFF),
]

# Rendered as nothing at all rather than left out. A left-out code point draws
# LVGL's placeholder box, and U+FE0F in particular trails almost every emoji a
# phone sends - a box after each one would be worse than no emoji at all.
ZERO_WIDTH = set([0x200D, 0xFE0E, 0xFE0F]) | set(range(0x1F3FB, 0x1F400))

THRESHOLD = 128

# Noto Emoji is line art, not silhouettes: its heart is drawn with an internal
# hatch fill and most glyphs are outlines. Thresholded straight to 1bpp at text
# size that turns to dithered mush. Dilating before the threshold merges the
# hatching and the outlines into a solid shape, which is what actually reads on
# a monochrome panel - at the cost of some interior detail, so the render size
# is deliberately below the target and the dilation grows it back.
DILATE = 3
# The variable font's heaviest instance, for the same reason.
WEIGHT = 700


def covered(font_path):
    tt = TTFont(font_path, fontNumber=0)
    return set(tt.getBestCmap().keys())


def render(font, cp, size):
    """Returns (box_w, box_h, ofs_x, ofs_y, rows) with rows as lists of 0/1."""
    pad = size * 2
    canvas_w, canvas_h = size * 4, size * 4
    baseline = size * 3

    img = Image.new("L", (canvas_w, canvas_h), 0)
    draw = ImageDraw.Draw(img)
    try:
        draw.text((pad, baseline), chr(cp), font=font, fill=255, anchor="ls")
    except Exception:
        return None

    if DILATE:
        img = img.filter(ImageFilter.MaxFilter(DILATE))

    mono = img.point(lambda v: 255 if v >= THRESHOLD else 0)
    bbox = mono.getbbox()
    if bbox is None:
        return None

    left, top, right, bottom = bbox
    box_w, box_h = right - left, bottom - top

    rows = []
    px = mono.load()
    for y in range(top, bottom):
        rows.append([1 if px[x, y] else 0 for x in range(left, right)])

    # ofs_y is measured up from the baseline to the bottom of the ink.
    return box_w, box_h, left - pad, baseline - bottom, rows


def pack(rows):
    """Rows into a continuous MSB-first bitstream, padded to a whole byte."""
    out = bytearray()
    acc = 0
    nbits = 0
    for row in rows:
        for bit in row:
            acc = (acc << 1) | bit
            nbits += 1
            if nbits == 8:
                out.append(acc)
                acc, nbits = 0, 0
    if nbits:
        out.append(acc << (8 - nbits))
    return bytes(out)


def main():
    if len(sys.argv) != 5:
        print(__doc__)
        return 1

    ttf_path, size, out_path, name = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]

    font = ImageFont.truetype(ttf_path, size)
    try:
        font.set_variation_by_axes([WEIGHT])
    except Exception:
        pass
    have = covered(ttf_path)

    wanted = []
    for lo, hi in EMOJI_RANGES:
        wanted.extend(cp for cp in range(lo, hi + 1) if cp in have)
    wanted.extend(cp for cp in sorted(ZERO_WIDTH) if cp not in wanted)
    wanted = sorted(set(wanted))

    bitmap = bytearray()
    glyphs = [(0, 0, 0, 0, 0, 0)]  # index 0 is the reserved "not found" entry
    kept = []

    for cp in wanted:
        if cp in ZERO_WIDTH:
            glyphs.append((len(bitmap), 0, 0, 0, 0, 0))
            kept.append(cp)
            continue

        r = render(font, cp, size)
        if r is None:
            continue

        box_w, box_h, ofs_x, ofs_y, rows = r
        # Dilation makes the ink wider than the font's own advance, so keep
        # enough room that neighbouring glyphs do not collide.
        adv_px = max(font.getlength(chr(cp)), box_w + max(0, ofs_x) + 1)
        adv = int(round(adv_px * 16))
        index = len(bitmap)
        bitmap.extend(pack(rows))
        glyphs.append((index, adv, box_w, box_h, ofs_x, ofs_y))
        kept.append(cp)

    bmp = [c for c in kept if c <= 0xFFFF]
    astral = [c for c in kept if c > 0xFFFF]

    cmaps = []
    gid = 1
    for group in (bmp, astral):
        if not group:
            continue
        start = group[0]
        span = group[-1] - start + 1
        assert span <= 0xFFFF, "a cmap range must fit a uint16 length"
        cmaps.append((start, span, gid, group))
        gid += len(group)

    # From the glyphs as they actually came out, not the font's metrics: the
    # dilation makes them taller than the nominal render size.
    tops = [oy + bh for _, _, _, bh, _, oy in glyphs[1:] if bh]
    bottoms = [oy for _, _, _, bh, _, oy in glyphs[1:] if bh]
    ascent = max(tops) if tops else size
    descent = -min(bottoms) if bottoms else 0
    if descent < 0:
        descent = 0

    guard = name.upper()

    with open(out_path, "w", newline="\n") as f:
        w = f.write
        w("/*******************************************************************************\n")
        w(" * Monochrome emoji, generated by tools/gen_emoji_font.py - do not edit by hand.\n")
        w(" *\n")
        w(" * Rendered from Noto Emoji (SIL Open Font License 1.1) at %dpx, dilated to\n" % size)
        w(" * thresholded to 1bpp, which is all this e-paper panel can show. Colour emoji\n")
        w(" * fonts would be wasted here; Noto Emoji is drawn as silhouettes to begin with.\n")
        w(" *\n")
        w(" * %d glyphs. Regional indicators are omitted (they only mean anything in\n" % (len(glyphs) - 1))
        w(" * pairs); variation selectors, the zero width joiner and the skin tone\n")
        w(" * modifiers are present but render as nothing, so they do not leave a\n")
        w(" * placeholder box after every emoji.\n")
        w(" ******************************************************************************/\n\n")
        w('#include "lvgl.h"\n\n')
        w("#ifndef %s\n#define %s 1\n#endif\n\n" % (guard, guard))
        w("#if %s\n\n" % guard)

        w("/*-----------------\n *    BITMAPS\n *----------------*/\n\n")
        w("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n")
        for i in range(0, len(bitmap), 16):
            w("    " + ", ".join("0x%02x" % b for b in bitmap[i:i + 16]) + ",\n")
        w("};\n\n")

        w("/*---------------------\n *  GLYPH DESCRIPTION\n *--------------------*/\n\n")
        w("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n")
        for index, adv, bw, bh, ox, oy in glyphs:
            w("    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, "
              ".ofs_x = %d, .ofs_y = %d},\n" % (index, adv, bw, bh, ox, oy))
        w("};\n\n")

        w("/*---------------------\n *  CHARACTER MAPPING\n *--------------------*/\n\n")
        for n, (start, span, first_gid, group) in enumerate(cmaps):
            w("static const uint16_t unicode_list_%d[] = {\n" % n)
            offs = [c - start for c in group]
            for i in range(0, len(offs), 12):
                w("    " + ", ".join("0x%x" % o for o in offs[i:i + 12]) + ",\n")
            w("};\n\n")

        w("static const lv_font_fmt_txt_cmap_t cmaps[] =\n{\n")
        for n, (start, span, first_gid, group) in enumerate(cmaps):
            w("    {\n")
            w("        .range_start = %d, .range_length = %d, .glyph_id_start = %d,\n"
              % (start, span, first_gid))
            w("        .unicode_list = unicode_list_%d, .glyph_id_ofs_list = NULL, "
              ".list_length = %d, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n"
              % (n, len(group)))
            w("    }%s\n" % ("," if n + 1 < len(cmaps) else ""))
        w("};\n\n")

        w("/*--------------------\n *  ALL CUSTOM DATA\n *--------------------*/\n\n")
        w("#if LVGL_VERSION_MAJOR == 8\nstatic lv_font_fmt_txt_glyph_cache_t cache;\n#endif\n\n")
        w("#if LVGL_VERSION_MAJOR >= 8\nstatic const lv_font_fmt_txt_dsc_t font_dsc = {\n")
        w("#else\nstatic lv_font_fmt_txt_dsc_t font_dsc = {\n#endif\n")
        w("    .glyph_bitmap = glyph_bitmap,\n    .glyph_dsc = glyph_dsc,\n")
        w("    .cmaps = cmaps,\n    .kern_dsc = NULL,\n    .kern_scale = 0,\n")
        w("    .cmap_num = %d,\n    .bpp = 1,\n    .kern_classes = 0,\n" % len(cmaps))
        w("    .bitmap_format = 0,\n")
        w("#if LVGL_VERSION_MAJOR == 8\n    .cache = &cache\n#endif\n};\n\n")

        w("/*-----------------\n *  PUBLIC FONT\n *----------------*/\n\n")
        w("#if LVGL_VERSION_MAJOR >= 8\nconst lv_font_t %s = {\n" % name)
        w("#else\nlv_font_t %s = {\n#endif\n" % name)
        w("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n")
        w("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n")
        w("    .line_height = %d,\n    .base_line = %d,\n" % (ascent + descent, descent))
        w("#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)\n")
        w("    .subpx = LV_FONT_SUBPX_NONE,\n#endif\n")
        w("#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8\n")
        w("    .underline_position = 0,\n    .underline_thickness = 0,\n#endif\n")
        w("    .dsc = &font_dsc,\n")
        w("#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9\n    .fallback = NULL,\n#endif\n")
        w("    .user_data = NULL,\n};\n\n")
        w("#endif /*#if %s*/\n" % guard)

    print("%s: %d glyphs, %d bytes of bitmap, %d cmaps"
          % (out_path, len(glyphs) - 1, len(bitmap), len(cmaps)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
