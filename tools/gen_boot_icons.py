#!/usr/bin/env python3
"""Draw the boot screen's system icons and emit them as C bitmaps.

    pip install pillow
    python tools/gen_boot_icons.py > src/assets/boot_icons.h

The boot screen runs before LVGL exists - it is drawn straight onto the panel
with GxEPD2 - so it cannot use the LVGL image assets in src/assets/. It needs
plain packed 1bpp bitmaps in the layout Adafruit_GFX::drawBitmap() reads: rows
padded to whole bytes, most significant bit leftmost.

The icons are line art rather than filled shapes. On a 1bpp panel at this size a
filled silhouette turns into a black blob, whereas an outline drawn two pixels
wide still reads as the thing it is meant to be. Two pixels, not one: a single
pixel line is legible on a backlit screen and nearly invisible on e-paper under
room lighting.

Regenerating is only necessary when an icon changes. The output is committed.
"""

import math
import sys
from PIL import Image, ImageDraw

SIZE = 32          # pixels square, which is what the boot screen's cells expect
WIDE = 2           # stroke width; see the note above about thin lines


def new():
    img = Image.new("1", (SIZE, SIZE), 0)
    return img, ImageDraw.Draw(img)


def ring(d, box, **kw):
    d.ellipse(box, outline=1, width=WIDE, **kw)


# --- the icons ---------------------------------------------------------------
# Each returns a 32x32 image. They are deliberately simple: at this size and
# this contrast, detail is lost and only the silhouette of the outline survives.

def icon_display():
    img, d = new()
    d.rounded_rectangle((3, 4, 28, 23), radius=3, outline=1, width=WIDE)
    d.line((12, 27, 20, 27), fill=1, width=WIDE)     # the stand
    d.line((16, 23, 16, 27), fill=1, width=WIDE)
    return img


def icon_touch():
    img, d = new()
    ring(d, (11, 11, 21, 21))                        # the fingertip
    d.arc((4, 4, 28, 28), start=200, end=340, fill=1, width=WIDE)
    d.arc((0, 0, 32, 32), start=210, end=330, fill=1, width=WIDE)
    return img


def icon_keyboard():
    img, d = new()
    d.rounded_rectangle((2, 8, 29, 25), radius=3, outline=1, width=WIDE)
    for row in range(2):
        for col in range(4):
            x = 6 + col * 6
            y = 12 + row * 5
            d.rectangle((x, y, x + 2, y + 2), fill=1)
    d.line((11, 22, 20, 22), fill=1, width=WIDE)     # the space bar
    return img


def icon_charger():
    img, d = new()
    d.rounded_rectangle((5, 6, 25, 28), radius=2, outline=1, width=WIDE)
    d.rectangle((12, 2, 18, 6), fill=1)              # the terminal
    d.polygon([(17, 10), (11, 19), (15, 19), (13, 25), (20, 15), (16, 15)], fill=1)
    return img


def icon_gauge():
    img, d = new()
    d.rounded_rectangle((3, 9, 26, 23), radius=2, outline=1, width=WIDE)
    d.rectangle((27, 13, 29, 19), fill=1)            # the nub
    for i in range(3):                               # the charge bars
        x = 6 + i * 6
        d.rectangle((x, 12, x + 3, 20), fill=1)
    return img


def icon_storage():
    img, d = new()
    for y in (5, 14, 23):                            # a stack of platters
        d.ellipse((4, y, 27, y + 6), outline=1, width=WIDE)
    d.line((4, 8, 4, 26), fill=1, width=WIDE)
    d.line((27, 8, 27, 26), fill=1, width=WIDE)
    return img


def icon_sdcard():
    img, d = new()
    d.polygon([(7, 3), (21, 3), (26, 8), (26, 29), (7, 29)], outline=1)
    d.polygon([(7, 3), (21, 3), (26, 8), (26, 29), (7, 29)], outline=1, width=WIDE)
    for i in range(3):                               # the contacts
        x = 11 + i * 4
        d.line((x, 7, x, 13), fill=1, width=WIDE)
    return img


def icon_gps():
    img, d = new()
    # A map pin: a circle with a tail, which is the one location glyph everybody
    # already knows.
    ring(d, (7, 3, 25, 21))
    d.ellipse((13, 9, 19, 15), fill=1)
    d.polygon([(11, 17), (21, 17), (16, 29)], fill=1)
    return img


def icon_light():
    img, d = new()
    ring(d, (11, 11, 21, 21))
    for i in range(8):                               # the rays
        a = i * math.pi / 4
        x0, y0 = 16 + 9.5 * math.cos(a), 16 + 9.5 * math.sin(a)
        x1, y1 = 16 + 14.5 * math.cos(a), 16 + 14.5 * math.sin(a)
        d.line((round(x0), round(y0), round(x1), round(y1)), fill=1, width=WIDE)
    return img


def icon_modem():
    img, d = new()
    d.polygon([(13, 12), (19, 12), (22, 29), (10, 29)], outline=1, width=WIDE)
    d.line((12, 21, 20, 21), fill=1, width=WIDE)     # the cross brace
    d.arc((6, 2, 26, 18), start=200, end=340, fill=1, width=WIDE)
    d.arc((2, -2, 30, 22), start=210, end=330, fill=1, width=WIDE)
    return img


def icon_mesh():
    img, d = new()
    # Hub and spoke rather than every node to every other: at this size a fully
    # connected graph turns into a blob with dots on the corners.
    hub = (16, 16)
    nodes = [(5, 5), (27, 5), (5, 27), (27, 27)]
    for n in nodes:
        d.line(hub + n, fill=1, width=WIDE)
    for (x, y) in nodes:
        d.ellipse((x - 3, y - 3, x + 3, y + 3), fill=1)
    d.ellipse((11, 11, 21, 21), fill=0)              # clear a moat round the hub
    d.ellipse((12, 12, 20, 20), fill=1)
    return img


ICONS = [
    ("display",  icon_display),
    ("touch",    icon_touch),
    ("keyboard", icon_keyboard),
    ("charger",  icon_charger),
    ("gauge",    icon_gauge),
    ("storage",  icon_storage),
    ("sdcard",   icon_sdcard),
    ("gps",      icon_gps),
    ("light",    icon_light),
    ("modem",    icon_modem),
    ("mesh",     icon_mesh),
]


def pack(img):
    """Rows padded to whole bytes, most significant bit leftmost."""
    px = img.load()
    out = []
    for y in range(SIZE):
        for byte in range(SIZE // 8):
            v = 0
            for bit in range(8):
                if px[byte * 8 + bit, y]:
                    v |= 0x80 >> bit
            out.append(v)
    return out


def main():
    w = sys.stdout.write

    w("/* Generated by tools/gen_boot_icons.py - do not edit by hand.\n"
      " *\n"
      " * Packed 1bpp, rows padded to whole bytes, most significant bit leftmost,\n"
      " * which is the layout Adafruit_GFX::drawBitmap() reads. The boot screen\n"
      " * draws these directly onto the panel, before LVGL exists.\n"
      " */\n"
      "#ifndef __BOOT_ICONS_H__\n"
      "#define __BOOT_ICONS_H__\n\n"
      "#include <stdint.h>\n\n"
      "#define BOOT_ICON_SIZE %d\n\n" % SIZE)

    for name, fn in ICONS:
        data = pack(fn())
        w("static const uint8_t boot_icon_%s[] = {\n" % name)
        per_line = SIZE // 8
        for i in range(0, len(data), per_line):
            w("    " + " ".join("0x%02X," % b for b in data[i:i + per_line]) + "\n")
        w("};\n\n")

    w("#endif\n")


if __name__ == "__main__":
    main()
