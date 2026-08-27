#!/usr/bin/env python3
"""Holt den Startsektor-Teil von Limine aus dem C-Header heraus.

Das Limine-Paket liefert `limine-bios-hdd.h` - ein C-Array mit dem
Stueck Code, das beim Einrichten einer Festplatte in den ersten Sektor
und in die Luecke dahinter geschrieben wird. Das Installationsprogramm
von RetroOS braucht diese Bytes zur Laufzeit, deshalb werden sie hier
in eine Datei geschrieben und spaeter fest ins Kernelabbild gelegt.
"""
import re
import sys

def main():
    if len(sys.argv) != 3:
        print("Aufruf: gen_limine_hdd.py <limine-bios-hdd.h> <ausgabe.bin>",
              file=sys.stderr)
        return 1

    text = open(sys.argv[1]).read()
    body = text[text.index("{") + 1:text.rindex("}")]
    data = bytes(int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]{1,2}", body))

    if len(data) < 1024 or data[510] != 0x55 or data[511] != 0xAA:
        print("gen_limine_hdd: das sieht nicht nach einem Startsektor aus",
              file=sys.stderr)
        return 1

    open(sys.argv[2], "wb").write(data)
    print("  GEN     %s (%d Byte)" % (sys.argv[2], len(data)))
    return 0

sys.exit(main())
