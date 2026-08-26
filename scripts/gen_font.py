#!/usr/bin/env python3
"""Erzeugt die Bitmap-Schrift des Kernels aus einer TrueType-Datei.

Ergebnis ist kernel/src/gui/font_data.c: fuer jedes Zeichen von 0x20 bis 0xFF
16 Bytes zu je 8 Pixeln. Die erzeugte Datei ist eingecheckt, der Build
braucht also weder Python noch die Schriftdatei.

Aufruf:  python3 scripts/gen_font.py
"""

import os
import sys

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 8, 16
FIRST, LAST = 0x20, 0xFF

CANDIDATES = [
    ("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 13, 0, 12),
    ("/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", 13, 0, 12),
]


def load_font():
    for path, size, xoff, baseline in CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size), path, xoff, baseline
    sys.exit("Keine geeignete TrueType-Schrift gefunden.")


def render(font, ch, xoff, baseline):
    img = Image.new("1", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(img)
    draw.text((xoff, baseline), ch, font=font, fill=1, anchor="ls")
    rows = []
    for y in range(HEIGHT):
        bits = 0
        for x in range(WIDTH):
            if img.getpixel((x, y)):
                bits |= 1 << (7 - x)
        rows.append(bits)
    return rows


def main():
    font, path, xoff, baseline = load_font()
    out = ["/* font_data.c - erzeugt von scripts/gen_font.py, nicht von Hand aendern.",
           " *",
           " * Quelle : %s" % os.path.basename(path),
           " * Format : %d Zeichen (0x%02X-0x%02X) zu je %d Zeilen mit %d Pixeln." %
           (LAST - FIRST + 1, FIRST, LAST, HEIGHT, WIDTH),
           " */",
           "",
           '#include "font.h"',
           "",
           "const uint8_t font8x16[FONT_GLYPHS][FONT_HEIGHT] = {"]

    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        try:
            rows = render(font, ch, xoff, baseline)
        except Exception:
            rows = [0] * HEIGHT
        if code == 0x20:
            rows = [0] * HEIGHT
        body = ", ".join("0x%02X" % r for r in rows)
        out.append("    { %s },  /* 0x%02X %s */" %
                   (body, code, ch if 33 <= code < 127 else " "))

    out.append("};")
    out.append("")

    dest = os.path.join(os.path.dirname(__file__), "..",
                        "kernel", "src", "gui", "font_data.c")
    with open(os.path.normpath(dest), "w") as fh:
        fh.write("\n".join(out))
    print("geschrieben:", os.path.normpath(dest), "aus", path)


if __name__ == "__main__":
    main()
