# Schriftvorlagen

Aus diesen Dateien erzeugt `scripts/gen_font.py` die eingebauten
Bitmap-Schriften in `kernel/src/gui/font_data.c`.

Jede Vorlage ist auf Latin-1 (U+0020 bis U+00FF) verkleinert und als
woff2 abgelegt - zusammen rund 130 KB. Das reicht, weil RetroOS ohnehin
nur diesen Bereich zeichnet, und macht den Lauf ohne Netz und ohne
installierte Systemschriften wiederholbar.

| Ordner            | Schrift          | Lizenz                       |
| ----------------- | ---------------- | ---------------------------- |
| dejavu-sans-mono  | DejaVu Sans Mono | Bitstream Vera / Arev        |
| liberation-mono   | Liberation Mono  | SIL Open Font License 1.1    |
| jetbrains-mono    | JetBrains Mono   | SIL Open Font License 1.1    |
| ibm-plex-mono     | IBM Plex Mono    | SIL Open Font License 1.1    |
| fira-mono         | Fira Mono        | SIL Open Font License 1.1    |
| source-code-pro   | Source Code Pro  | SIL Open Font License 1.1    |
| inconsolata       | Inconsolata      | SIL Open Font License 1.1    |
| ubuntu-mono       | Ubuntu Mono      | Ubuntu Font Licence 1.0      |
| unifont           | GNU Unifont      | SIL Open Font License 1.1 *  |
| vt323             | VT323            | SIL Open Font License 1.1    |

Der volle Lizenztext liegt je Ordner als `LICENSE` daneben.

\* Unifont steht doppelt unter Lizenz: fuer die fertigen Schriften gilt
wahlweise die OFL 1.1 oder die GPL mit der Font-Ausnahme. RetroOS nimmt
die OFL.
