#!/usr/bin/env python3
"""Erzeugt die Beispielbilder, die RetroOS mitbringt.

Sie liegen als PNG und JPEG in data/ und werden in den Kern eingebaut,
damit der Browser auch ohne Netzwerk etwas zu zeigen hat.
"""
import math
import os
import struct
import zlib

HIER = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ZIEL = os.path.join(HIER, "data")
os.makedirs(ZIEL, exist_ok=True)


def png_schreiben(pfad, breite, hoehe, pixel):
    """Schreibt ein PNG mit echten Farben und Alphakanal."""
    roh = bytearray()
    for y in range(hoehe):
        roh.append(0)                      # Filter "keiner"
        for x in range(breite):
            roh.extend(pixel(x, y))

    def block(art, inhalt):
        daten = art + inhalt
        return (struct.pack(">I", len(inhalt)) + daten +
                struct.pack(">I", zlib.crc32(daten) & 0xFFFFFFFF))

    kopf = struct.pack(">IIBBBBB", breite, hoehe, 8, 6, 0, 0, 0)
    with open(pfad, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(block(b"IHDR", kopf))
        f.write(block(b"IDAT", zlib.compress(bytes(roh), 9)))
        f.write(block(b"IEND", b""))


# --- Das Wappen von RetroOS: eine Scheibe mit Farbverlauf ----------------
def wappen(x, y):
    mitte = 63.5
    dx, dy = x - mitte, y - mitte
    r = math.hypot(dx, dy)

    if r > 62:
        return (0, 0, 0, 0)

    # Ein Ring aussen, innen ein Verlauf von oben nach unten.
    if r > 56:
        return (0x20, 0x48, 0x78, 255)

    oben = (0x50, 0x90, 0xD0)
    unten = (0x18, 0x38, 0x68)
    t = y / 127.0
    farbe = [int(oben[i] + (unten[i] - oben[i]) * t) for i in range(3)]

    # Ein "R" als Aussparung.
    if 34 <= x <= 46 and 30 <= y <= 96:
        return (255, 255, 255, 255)
    if 46 <= x <= 82 and 30 <= y <= 42:
        return (255, 255, 255, 255)
    if 46 <= x <= 82 and 56 <= y <= 68:
        return (255, 255, 255, 255)
    if 70 <= x <= 82 and 30 <= y <= 68:
        return (255, 255, 255, 255)
    if 56 <= y <= 96 and abs((x - 46) - (y - 56) * 0.9) < 7 and x >= 46:
        return (255, 255, 255, 255)

    return (farbe[0], farbe[1], farbe[2], 255)


png_schreiben(os.path.join(ZIEL, "wappen.png"), 128, 128, wappen)


# --- Ein Farbmuster zum Pruefen der Skalierung --------------------------
def muster(x, y):
    kachel = ((x // 16) + (y // 16)) % 2
    r = (x * 255) // 191
    g = (y * 255) // 127
    b = 255 - (r + g) // 2
    if kachel:
        r, g, b = r // 2, g // 2, b // 2
    return (r, g, b, 255)


png_schreiben(os.path.join(ZIEL, "muster.png"), 192, 128, muster)

print("Bilder erzeugt in", ZIEL)
