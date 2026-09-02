#!/usr/bin/env python3
"""Erzeugt die Bitmap-Schriften des Kernels aus den Dateien in third_party/fonts.

RetroOS zeichnet Text in einer festen Zelle von 8x16 Pixeln - die halbe
Oberflaeche rechnet mit diesen beiden Zahlen. Eine Schrift auszutauschen
heisst darum nicht, das Layout zu aendern, sondern nur, andere Punkte in
dieselbe Zelle zu setzen. Genau das macht dieses Skript: Es rastert jede
Schriftart einmal von 0x20 bis 0xFF und legt alle nebeneinander in
kernel/src/gui/font_data.c ab. Der Kernel waehlt zur Laufzeit aus.

Die Vorlagen liegen als woff2 unter third_party/fonts, auf Latin-1
verkleinert - zusammen gut 130 KB, damit der Lauf ohne Netz und ohne
installierte Systemschriften wiederholbar ist. Gebraucht werden dafuer
fonttools (liest woff2) und Pillow (rastert).

Aufruf:  python3 scripts/gen_font.py
"""

import os
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 8, 16
FIRST, LAST = 0x20, 0xFF

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
FONTS = os.path.join(ROOT, "third_party", "fonts")

# Bezeichner, Anzeigename, Verzeichnis, Lizenz, Punktgroesse, Versatz, Grundlinie.
#
# Groesse und Grundlinie sind je Schrift von Hand nachgezogen: Der Wert
# ist der groesste, bei dem Grossbuchstaben, Unterlaengen und deutsche
# Umlaute noch gemeinsam in die Zelle passen.
FACES = [
    ("DEJAVU",     "DejaVu Sans Mono", "dejavu-sans-mono", "Bitstream Vera", 13, 0, 12),
    ("LIBERATION", "Liberation Mono",  "liberation-mono",  "OFL 1.1",        13, 0, 12),
    ("JETBRAINS",  "JetBrains Mono",   "jetbrains-mono",   "OFL 1.1",        12, 0, 12),
    ("PLEX",       "IBM Plex Mono",    "ibm-plex-mono",    "OFL 1.1",        13, 0, 12),
    ("FIRA",       "Fira Mono",        "fira-mono",        "OFL 1.1",        13, 0, 12),
    ("SOURCE",     "Source Code Pro",  "source-code-pro",  "OFL 1.1",        12, 0, 12),
    ("INCONSOLATA","Inconsolata",      "inconsolata",      "OFL 1.1",        16, 0, 12),
    ("UBUNTU",     "Ubuntu Mono",      "ubuntu-mono",      "Ubuntu 1.0",     16, 0, 12),
    ("UNIFONT",    "Unifont",          "unifont",          "OFL 1.1",        16, 0, 13),
    ("VT323",      "VT323",            "vt323",            "OFL 1.1",        16, 1, 12),
]


def open_face(slug, size, tmp):
    """Oeffnet eine woff2-Datei. Pillow kann das nicht, fonttools schon -
    also einmal nach TrueType wandeln und die Kopie rastern."""
    src = os.path.join(FONTS, slug, slug + ".woff2")
    if not os.path.exists(src):
        sys.exit("Schrift fehlt: " + src)
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        sys.exit("fonttools wird gebraucht: pip install fonttools brotli")
    ttf = os.path.join(tmp, slug + ".ttf")
    if not os.path.exists(ttf):
        face = TTFont(src)
        face.flavor = None
        face.save(ttf)
    return ImageFont.truetype(ttf, size)


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


def table(font, ident, xoff, baseline):
    out = ["static const uint8_t glyphs_%s[FONT_GLYPHS][FONT_HEIGHT] = {"
           % ident.lower()]
    for code in range(FIRST, LAST + 1):
        try:
            rows = render(font, chr(code), xoff, baseline)
        except Exception:
            rows = [0] * HEIGHT
        if code == 0x20:
            rows = [0] * HEIGHT
        body = ", ".join("0x%02X" % r for r in rows)
        out.append("    { %s },  /* 0x%02X %s */" %
                   (body, code, chr(code) if 33 <= code < 127 else " "))
    out.append("};")
    out.append("")
    return out


def main():
    out = ["/* font_data.c - erzeugt von scripts/gen_font.py, nicht von Hand aendern.",
           " *",
           " * %d Schriftarten zu je %d Zeichen (0x%02X-0x%02X), jedes Zeichen"
           % (len(FACES), LAST - FIRST + 1, FIRST, LAST),
           " * %d Zeilen mit %d Pixeln. Die Vorlagen und ihre Lizenzen liegen"
           % (HEIGHT, WIDTH),
           " * unter third_party/fonts.",
           " */",
           "",
           '#include "font.h"',
           ""]

    with tempfile.TemporaryDirectory() as tmp:
        for ident, name, slug, lic, size, xoff, baseline in FACES:
            font = open_face(slug, size, tmp)
            out += table(font, ident, xoff, baseline)

    out.append("const struct font_face font_faces[FONT_FACES] = {")
    for ident, name, slug, lic, size, xoff, baseline in FACES:
        out.append('    { "%s", "%s", glyphs_%s },' % (name, lic, ident.lower()))
    out.append("};")
    out.append("")
    out.append("/* Bis config_apply() etwas anderes sagt, gilt die erste Schrift.")
    out.append(" * Statisch gesetzt, damit auch der Bildschirmtext vor dem Laden")
    out.append(" * der Einstellungen schon Punkte findet. */")
    out.append("const uint8_t (*font_active)[FONT_HEIGHT] = glyphs_%s;"
               % FACES[0][0].lower())
    out.append("")

    dest = os.path.join(ROOT, "kernel", "src", "gui", "font_data.c")
    with open(dest, "w") as fh:
        fh.write("\n".join(out))
    print("geschrieben:", dest, "-", len(FACES), "Schriftarten")


if __name__ == "__main__":
    main()
