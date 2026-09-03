#!/usr/bin/env python3
"""Erzeugt die Uebersetzungstabelle aus data/sprache-en.txt.

Die Textdatei enthaelt je Zeile ein Paar, getrennt durch einen
Tabulator:

    Einstellungen<TAB>Settings

Zeilen, die mit # anfangen, und leere Zeilen sind Anmerkungen. In
beiden Haelften steht \\n fuer einen Zeilenumbruch - manche Meldungen
sind mehrzeilig.

Sortiert wird hier und nicht im Kern: Die Suche zur Laufzeit ist binaer
und verlaesst sich darauf. Eine Pruefung im Testlauf besteht spaeter
noch einmal darauf, damit eine von Hand geaenderte Tabelle auffliegt.
"""

import sys
from pathlib import Path

QUELLE = Path("data/sprache-en.txt")
ZIEL = Path("kernel/src/lib/lang_data.c")


def c_string(text):
    out = []
    for ch in text:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 32 or ord(ch) > 126:
            raise SystemExit(
                f"Nur ASCII in der Tabelle - Umlaute als ae/oe/ue: {text!r}")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def main():
    quelle = Path(sys.argv[1]) if len(sys.argv) > 1 else QUELLE
    ziel = Path(sys.argv[2]) if len(sys.argv) > 2 else ZIEL

    paare = {}
    for nummer, zeile in enumerate(quelle.read_text(encoding="utf-8").splitlines(), 1):
        if not zeile.strip() or zeile.lstrip().startswith("#"):
            continue
        if "\t" not in zeile:
            raise SystemExit(f"{quelle}:{nummer}: kein Tabulator in der Zeile")
        de, en = zeile.split("\t", 1)
        # Nicht beschneiden: Fuehrende Leerzeichen ruecken Zeilen ein
        # ("  Kern %u"), nachgestellte trennen einen Wert ab
        # ("Fehler: "). Wer hier strip() aufruft, macht aus einem
        # Schluessel einen anderen, und der Text bleibt deutsch.
        de = de.replace("\\n", "\n")
        en = en.replace("\\n", "\n")
        if not de or not en:
            raise SystemExit(f"{quelle}:{nummer}: eine Haelfte ist leer")
        if de in paare and paare[de] != en:
            raise SystemExit(f"{quelle}:{nummer}: {de!r} steht zweimal da")
        paare[de] = en

    zeilen = [
        "/* lang_data.c - erzeugt von scripts/gen_sprache.py. Nicht von Hand",
        " * aendern, sondern data/sprache-en.txt und dann \"make sprache\". */",
        "",
        '#include "retro.h"',
        "",
        "const struct lang_entry {",
        "    const char *de;",
        "    const char *en;",
        "} lang_table[] = {",
    ]
    for de in sorted(paare):
        zeilen.append(f"    {{ {c_string(de)},")
        zeilen.append(f"      {c_string(paare[de])} }},")
    zeilen.append("};")
    zeilen.append("")
    zeilen.append("const size_t lang_table_count = ARRAY_LEN(lang_table);")
    zeilen.append("")

    ziel.write_text("\n".join(zeilen), encoding="utf-8")
    print(f"  SPRACHE {ziel} ({len(paare)} Eintraege)")


if __name__ == "__main__":
    main()
