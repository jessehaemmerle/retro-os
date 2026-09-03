#!/usr/bin/env python3
"""Macht aus den Lucide-Symbolen (ISC) die Bilddaten der Oberflaeche.

Die Vorlagen sind Strichzeichnungen in einem 24x24-Feld. Damit sie auf
hellem wie auf dunklem Grund lesbar bleiben, wird jedes Symbol zweimal
gezeichnet: erst dick in einem fast schwarzen Ton, darueber duenn in
seiner Farbe. Das ergibt den umrandeten Strich, den ein Desktop dieser
Bauart braucht.

Gerendert wird in zwei Groessen - 16 und 32 Pixel -, denn ein auf das
Doppelte gezogenes 16er-Bild sieht grob aus. Beide Groessen entstehen
aus derselben Vorlage in voller Aufloesung.

Aufruf:  python3 scripts/gen_icons.py
Ergebnis: kernel/src/gui/icon_data.c
"""

import io
import os
import re
import sys

import cairosvg
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SVG_DIR = os.path.join(ROOT, "third_party", "lucide", "icons")
OUT = os.path.join(ROOT, "kernel", "src", "gui", "icon_data.c")

OUTLINE = "#141414"

# Name in RetroOS, Vorlage bei Lucide, Farbe des Strichs.
ICONS = [
    ("ICON_FOLDER",      "folder",           "#F2C14E"),
    ("ICON_FOLDER_OPEN", "folder-open",      "#F2C14E"),
    ("ICON_FILE",        "file",             "#E6EDF2"),
    ("ICON_FILE_TEXT",   "file-text",        "#E6EDF2"),
    ("ICON_DISK",        "hard-drive",       "#BFCBD4"),
    ("ICON_COMPUTER",    "monitor",          "#9BD1E8"),
    ("ICON_TERMINAL",    "square-terminal",  "#7BE08C"),
    ("ICON_EDITOR",      "file-pen",         "#F0A55E"),
    ("ICON_INFO",        "info",             "#7FD4E8"),
    ("ICON_TRASH",       "trash",            "#AEB8C0"),
    ("ICON_TRASH_FULL",  "trash-2",          "#E08A6A"),
    ("ICON_UP",          "arrow-up",         "#8FB8E8"),
    ("ICON_BACK",        "arrow-left",       "#8FB8E8"),
    ("ICON_HOME",        "house",            "#8FB8E8"),
    ("ICON_NEW_FOLDER",  "folder-plus",      "#F2C14E"),
    ("ICON_NEW_FILE",    "file-plus",        "#E6EDF2"),
    ("ICON_SETTINGS",    "settings",         "#C3CBD3"),
    ("ICON_BROWSER",     "globe",            "#7FB2F0"),
    ("ICON_RELOAD",      "rotate-cw",        "#8FB8E8"),
    ("ICON_NETWORK",     "network",          "#84D9C0"),
    ("ICON_CODE",        "code-xml",         "#C79BF0"),
    ("ICON_DOWNLOAD",    "download",         "#7BE0AE"),
    ("ICON_IMAGE",       "image",            "#E89BC8"),
    ("ICON_RESTORE",     "undo-2",           "#7BE08C"),
    ("ICON_PLAY",        "play",             "#7BE08C"),
    ("ICON_SAVE",        "save",             "#8FB8E8"),
    ("ICON_CLOCK",       "clock",            "#7FD4E8"),

    # Systemmonitor, Aufgaben und Protokoll.
    ("ICON_MONITOR",     "activity",         "#7BE08C"),
    ("ICON_TASKS",       "list-todo",        "#F2C14E"),
    ("ICON_LOG",         "scroll-text",      "#BFCBD4"),
    ("ICON_WARN",        "triangle-alert",   "#F2C14E"),
    ("ICON_DONE",        "circle-check",     "#7BE08C"),
    ("ICON_CALENDAR",    "calendar",         "#8FB8E8"),
    ("ICON_FLAG",        "flag",             "#E08A6A"),
    ("ICON_STOP",        "circle-x",         "#E06A6A"),

    # Benutzer, Gruppen und Rechte.
    ("ICON_USER",        "user",             "#9BD1E8"),
    ("ICON_USERS",       "users",            "#9BD1E8"),
    ("ICON_USER_ADD",    "user-plus",        "#7BE08C"),
    ("ICON_LOCK",        "lock",             "#F2C14E"),
    ("ICON_KEY",         "key-round",        "#F2C14E"),
    ("ICON_SHIELD",      "shield-check",     "#7BE0AE"),
    ("ICON_LOGOUT",      "log-out",          "#E08A6A"),

    # Bueroprogramme und ihre Formatleisten. 
    ("ICON_TABLE",       "table",            "#7BE08C"),
    ("ICON_DOCUMENT",    "letter-text",      "#8FB8E8"),
    ("ICON_SLIDES",      "presentation",     "#F0A55E"),
    ("ICON_PRESENT",     "monitor-play",     "#F0A55E"),
    ("ICON_BOLD",        "bold",             "#C3CBD3"),
    ("ICON_ITALIC",      "italic",           "#C3CBD3"),
    ("ICON_UNDERLINE",   "underline",        "#C3CBD3"),
    ("ICON_ALIGN_LEFT",  "align-left",       "#C3CBD3"),
    ("ICON_ALIGN_MID",   "align-center",     "#C3CBD3"),
    ("ICON_ALIGN_RIGHT", "align-right",      "#C3CBD3"),
    ("ICON_LIST",        "list",             "#C3CBD3"),
    ("ICON_HEADING",     "heading",          "#C3CBD3"),
    ("ICON_SUM",         "sigma",            "#7BE08C"),
    ("ICON_PLUS",        "plus",             "#7BE08C"),
    ("ICON_PREV",        "chevron-left",     "#8FB8E8"),
    ("ICON_NEXT",        "chevron-right",    "#8FB8E8"),

    # Rechner und Bildschirmfoto.
    ("ICON_CALC",        "calculator",       "#C3CBD3"),
    ("ICON_CAMERA",      "camera",           "#9BD1E8"),
]

# Groesse -> (Strichbreite, Breite der Umrandung). Kleine Bilder
# brauchen den dickeren Strich, sonst verschwindet er im Weichzeichnen.
SIZES = {16: (2.2, 3.6), 32: (2.0, 3.8)}

SVG_HEAD = re.compile(r"^.*?<svg\b[^>]*>", re.S)
SVG_TAIL = re.compile(r"</svg>\s*$", re.S)


def children(path):
    """Der Inhalt der Vorlage ohne das umschliessende <svg>."""
    text = open(path, encoding="utf-8").read()
    text = SVG_HEAD.sub("", text)
    text = SVG_TAIL.sub("", text)
    return text.strip()


def render(inner, size, color):
    stroke, outline = SIZES[size]
    common = ('fill="none" stroke-linecap="round" stroke-linejoin="round"')
    svg = (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" '
        'width="%d" height="%d">'
        '<g %s stroke="%s" stroke-width="%s">%s</g>'
        '<g %s stroke="%s" stroke-width="%s">%s</g>'
        "</svg>"
        % (size, size, common, OUTLINE, outline, inner,
           common, color, stroke, inner)
    )
    # Vierfach zeichnen und verkleinern: Die Kanten werden dadurch
    # sauberer, als cairo sie in einem Zug hinbekaeme.
    png = cairosvg.svg2png(bytestring=svg.encode("utf-8"),
                           output_width=size * 4, output_height=size * 4)
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    return img.resize((size, size), Image.LANCZOS)


def as_words(img):
    raw = img.tobytes()
    out = []
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i], raw[i + 1], raw[i + 2], raw[i + 3]
        out.append((a << 24) | (r << 16) | (g << 8) | b)
    return out


def main():
    missing = [n for _, n, _ in ICONS
               if not os.path.exists(os.path.join(SVG_DIR, n + ".svg"))]
    if missing:
        sys.exit("Diese Vorlagen fehlen in %s: %s" % (SVG_DIR, ", ".join(missing)))

    parts = [
        "/* icon_data.c - die Bilder der Symbole.",
        " *",
        " * ERZEUGT von scripts/gen_icons.py - nicht von Hand aendern.",
        " * Vorlagen: Lucide (ISC), siehe third_party/lucide/LICENSE.",
        " *",
        " * Jedes Symbol liegt in zwei Groessen bereit, damit es weder",
        " * in der Werkzeugleiste noch auf dem Desktop gedehnt werden",
        " * muss. Ein Wort je Punkt: 0xAARRGGBB.",
        " */",
        "",
        '#include "icons.h"',
        "",
    ]

    for size in sorted(SIZES):
        for name, source, color in ICONS:
            img = render(children(os.path.join(SVG_DIR, source + ".svg")),
                         size, color)
            words = as_words(img)
            parts.append("static const uint32_t bits%d_%s[%d] = {"
                         % (size, name[5:].lower(), size * size))
            for row in range(size):
                line = "    "
                for col in range(size):
                    line += "0x%08X," % words[row * size + col]
                parts.append(line)
            parts.append("};")
            parts.append("")

    for size in sorted(SIZES):
        parts.append("const uint32_t *const icon_bits%d[ICON_COUNT] = {" % size)
        for name, _, _ in ICONS:
            parts.append("    [%s] = bits%d_%s," % (name, size, name[5:].lower()))
        parts.append("};")
        parts.append("")

    open(OUT, "w", encoding="utf-8").write("\n".join(parts))
    print("%s: %d Symbole in %s" % (OUT, len(ICONS),
                                    ", ".join("%dx%d" % (s, s) for s in sorted(SIZES))))


if __name__ == "__main__":
    main()
