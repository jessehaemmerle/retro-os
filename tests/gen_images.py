#!/usr/bin/env python3
"""Erzeugt Testbilder und die zugehoerigen Rohdaten fuer image_test.c."""
import os, struct
from PIL import Image

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bilder")
os.makedirs(OUT, exist_ok=True)

def muster(w, h, alpha=False):
    img = Image.new("RGBA" if alpha else "RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            r = (x * 255) // max(w - 1, 1)
            g = (y * 255) // max(h - 1, 1)
            b = ((x + y) * 255) // max(w + h - 2, 1)
            if alpha:
                px[x, y] = (r, g, b, (x * 7 + y * 3) % 256)
            else:
                px[x, y] = (r, g, b)
    return img

def raw(name, img):
    img = img.convert("RGBA")
    w, h = img.size
    with open(f"{OUT}/{name}.raw", "wb") as f:
        f.write(struct.pack("<II", w, h))
        f.write(img.tobytes())

rgb = muster(61, 43)
rgb.save(f"{OUT}/rgb.png"); raw("rgb", rgb)

rgba = muster(48, 31, alpha=True)
rgba.save(f"{OUT}/rgba.png"); raw("rgba", rgba)

pal = muster(57, 39).convert("P", palette=Image.ADAPTIVE, colors=200)
pal.save(f"{OUT}/palette.png"); raw("palette", pal)

grau = muster(50, 50).convert("L")
grau.save(f"{OUT}/grau.png"); raw("grau", grau)

pal4 = muster(33, 21).convert("P", palette=Image.ADAPTIVE, colors=16)
pal4.save(f"{OUT}/pal4.png", bits=4); raw("pal4", pal4)

# Bei 16 Bit je Kanal nimmt unser Leser das obere Byte. Pillow rundet
# beim Umwandeln anders, darum bauen wir die Vergleichsdaten selbst.
werte = [(x * 1600 + y * 37) % 65536 for y in range(25) for x in range(40)]
g16 = Image.new("I;16", (40, 25))
g16.putdata(werte)
g16.save(f"{OUT}/rgb16.png")
with open(f"{OUT}/rgb16.raw", "wb") as f:
    f.write(struct.pack("<II", 40, 25))
    for v in werte:
        hoch = v >> 8
        f.write(bytes((hoch, hoch, hoch, 255)))

adam = muster(45, 29)
adam.save(f"{OUT}/adam7.png", interlace=True); raw("adam7", adam)

gif = muster(53, 37).convert("P", palette=Image.ADAPTIVE, colors=128)
gif.save(f"{OUT}/bild.gif"); raw("gif", gif)

lace = muster(64, 48).convert("P", palette=Image.ADAPTIVE, colors=64)
lace.save(f"{OUT}/lace.gif", interlace=True); raw("lace", lace)

bmp = muster(37, 23)
bmp.save(f"{OUT}/bild.bmp"); raw("bmp", bmp)

voll = muster(64, 48)
voll.save(f"{OUT}/voll.jpg", quality=95, subsampling=0)
raw("voll", Image.open(f"{OUT}/voll.jpg").convert("RGB"))

halb = muster(80, 64)
halb.save(f"{OUT}/halb.jpg", quality=90, subsampling=2)
raw("halb", Image.open(f"{OUT}/halb.jpg").convert("RGB"))

gj = muster(56, 40).convert("L")
gj.save(f"{OUT}/grau.jpg", quality=92)
raw("graujpg", Image.open(f"{OUT}/grau.jpg").convert("L"))

print("Testbilder erzeugt in", OUT)
