/* initfs.c - der Datenbestand, mit dem RetroOS startet.
 *
 * Das Dateisystem liegt im RAM und ist nach jedem Neustart wieder in diesem
 * Zustand. Hier entsteht also gewissermassen die Auslieferungskonfiguration.
 */

#include "vfs.h"
#include "kstring.h"

void programs_install(struct fs_node *directory);

extern struct fs_node *fs_root_slot(void);

static void put(struct fs_node *dir, const char *name, const char *text,
                bool readonly)
{
    struct fs_node *f = fs_create(dir, name, FS_FILE);

    if (!f)
        return;

    fs_write(f, text, strlen(text));
    f->readonly = readonly;
}

static struct fs_node *dir(struct fs_node *parent, const char *name)
{
    return fs_create(parent, name, FS_DIR);
}

/* Die Beispielbilder liegen als Datenscheiben im Kern. */
extern const uint8_t _binary_wappen_png_start[];
extern const uint8_t _binary_wappen_png_end[];
extern const uint8_t _binary_muster_png_start[];
extern const uint8_t _binary_muster_png_end[];

static void put_binary(struct fs_node *dir, const char *name,
                       const uint8_t *start, const uint8_t *end)
{
    struct fs_node *f = fs_create(dir, name, FS_FILE);

    if (!f)
        return;
    fs_write(f, start, (size_t)(end - start));
    f->readonly = true;
}

void initfs_populate(struct fs_node *root)
{
    struct fs_node *system   = dir(root, "System");
    struct fs_node *programs = dir(root, "Programme");
    struct fs_node *docs     = dir(root, "Dokumente");
    struct fs_node *media    = dir(root, "Medien");
    dir(root, "Temp");

    /* --- System --- */
    put(system, "version.txt",
        "RetroOS 1.0\n"
        "Ein kleines 64-Bit-Betriebssystem mit eigener Oberflaeche.\n"
        "\n"
        "Kernel   : monolithisch, x86-64, Long Mode\n"
        "Grafik   : linearer Framebuffer, 32 Bit Farbtiefe\n"
        "Eingabe  : PS/2-Tastatur und -Maus\n"
        "Dateien  : RAM-Dateisystem\n", true);

    put(system, "lizenz.txt",
        "RetroOS steht unter der MIT-Lizenz.\n"
        "Der Bootloader (Limine) steht unter der BSD-2-Clause-Lizenz.\n"
        "Die Symbole stammen aus dem Lucide-Satz (ISC-Lizenz).\n", true);

    struct fs_node *cfg = dir(system, "Konfiguration");
    put(cfg, "desktop.cfg",
        "# Einstellungen der Oberflaeche\n"
        "hintergrund = verlauf\n"
        "taskleiste  = unten\n"
        "doppelklick = 400\n", false);

    put(cfg, "tastatur.cfg",
        "# Tastaturbelegung\n"
        "layout = de\n"
        "wiederholrate = 30\n", false);

    /* --- Dokumente --- */
    put(docs, "willkommen.txt",
        "Willkommen bei RetroOS!\n"
        "=======================\n"
        "\n"
        "Dieses System ist von Grund auf neu geschrieben: eigener Kernel,\n"
        "eigene Treiber, eigenes Fenstersystem.\n"
        "\n"
        "Bedienung\n"
        "---------\n"
        "  * Fenster mit der Titelleiste verschieben\n"
        "  * Mit [X] schliessen, mit [_] in die Taskleiste legen\n"
        "  * Doppelklick im Dateimanager oeffnet Ordner und Dateien\n"
        "  * Der Start-Knopf unten links oeffnet das Menue\n"
        "\n"
        "Viel Vergnuegen beim Stoebern.\n", false);

    put(docs, "notizen.txt",
        "Einkaufsliste\n"
        "-------------\n"
        "- Disketten (3,5 Zoll)\n"
        "- Kaffee\n"
        "- Ein neues Modem\n", false);

    struct fs_node *briefe = dir(docs, "Briefe");
    put(briefe, "antrag.txt",
        "Sehr geehrte Damen und Herren,\n"
        "\n"
        "hiermit beantrage ich die Zuteilung von weiteren 640 KiB\n"
        "Arbeitsspeicher. Wie allgemein bekannt ist, sollte das\n"
        "eigentlich fuer jeden genug sein.\n"
        "\n"
        "Mit freundlichen Gruessen\n", false);

    /* --- Programme --- */
    programs_install(programs);

    put(docs, "beispiel.html",
        "<html>\n"
        "<head><title>Beispielseite</title>\n"
        "<style>\n"
        "body { background: #f6f4ee; color: #202020; margin: 20px }\n"
        "h1 { color: #204878; border-bottom: 2px solid #6890c0 }\n"
        "h2 { color: #305888 }\n"
        ".kasten { background: #ffffff; border: 1px solid #ccc8bc;\n"
        "          padding: 12px 16px; margin: 14px 0 }\n"
        ".wappen { float: right; margin-left: 16px }\n"
        "</style></head>\n"
        "<body>\n"
        "<img class=\"wappen\" src=\"/Medien/wappen.png\" width=\"96\"\n"
        "     alt=\"Wappen\">\n"
        "<h1>Eine Seite aus dem Dateisystem</h1>\n"
        "<p>Diese Datei liegt unter <b>/Dokumente/beispiel.html</b> und wird\n"
        "vom Browser aus dem Dateisystem geladen - ganz ohne Netzwerk.</p>\n"
        "<div class=\"kasten\">\n"
        "<h2>Was der Browser kann</h2>\n"
        "<ul>\n"
        "<li>Ueberschriften, Absaetze und Aufzaehlungen</li>\n"
        "<li>Fett ausgezeichneten <b>Text</b> und <i>kursiven</i> dazu</li>\n"
        "<li>Formatvorlagen mit Farben, Rahmen und Abstaenden</li>\n"
        "<li>Bilder in PNG, JPEG, GIF und BMP</li>\n"
        "<li>JavaScript samt Zugriff auf den Dokumentbaum</li>\n"
        "<li>Verweise, etwa zur <a href=\"pruefung.html\">Pruefseite</a>\n"
        "    oder zurueck zur <a href=\"start:\">Startseite</a></li>\n"
        "</ul>\n"
        "</div>\n"
        "<hr>\n"
        "<p>Umlaute funktionieren auch: Gr&uuml;&szlig;e aus RetroOS.</p>\n"
        "</body>\n"
        "</html>\n", false);

    put(docs, "pruefung.html",
        "<html>\n"
        "<head><title>Selbsttest der Darstellung</title>\n"
        "<style>\n"
        "body { background: #fbfbf7; color: #1a1a1a; margin: 0;\n"
        "       font-family: sans-serif }\n"
        "header { background: #204878; color: #ffffff; padding: 14px 20px }\n"
        "header h1 { color: #ffffff; margin: 0; font-size: 26px }\n"
        "main { padding: 16px 20px }\n"
        "section { margin-bottom: 20px }\n"
        "h2 { color: #204878; font-size: 20px; margin: 14px 0 6px 0 }\n"
        ".reihe { margin: 8px 0 }\n"
        ".feld { display: inline-block; width: 84px; height: 46px;\n"
        "        margin-right: 6px; color: #ffffff; text-align: center;\n"
        "        padding: 8px 0 }\n"
        ".a { background: #c02020 } .b { background: #208040 }\n"
        ".c { background: #2050a0 } .d { background: #a06010 }\n"
        "table { border-collapse: collapse }\n"
        "th { background: #dde4ee }\n"
        "td, th { border: 1px solid #98a8bc; padding: 4px 10px }\n"
        "#zaehler { font-size: 30px; color: #204878; font-weight: bold }\n"
        "button { font-size: 15px }\n"
        ".gross { font-size: 24px } .klein { font-size: 12px }\n"
        "blockquote { color: #505050 }\n"
        "</style></head>\n"
        "<body>\n"
        "<header><h1>Selbsttest der Darstellung</h1></header>\n"
        "<main>\n"
        "<section><h2>Schrift</h2>\n"
        "<p><span class=\"klein\">klein</span> - normal -\n"
        "<span class=\"gross\">gross</span>, <b>fett</b>, <i>kursiv</i>,\n"
        "<u>unterstrichen</u>, <s>durchgestrichen</s>,\n"
        "<code>nichtproportional</code></p>\n"
        "<blockquote>Ein eingerueckter Absatz mit Strich am Rand.</blockquote>\n"
        "</section>\n"
        "<section><h2>Kaesten</h2>\n"
        "<div class=\"reihe\">\n"
        "<span class=\"feld a\">rot</span><span class=\"feld b\">gruen</span>\n"
        "<span class=\"feld c\">blau</span><span class=\"feld d\">braun</span>\n"
        "</div></section>\n"
        "<section><h2>Tabelle</h2>\n"
        "<table><tr><th>Baustein</th><th>Stand</th></tr>\n"
        "<tr><td>Dokumentbaum</td><td>fertig</td></tr>\n"
        "<tr><td>Formatvorlagen</td><td>fertig</td></tr>\n"
        "<tr><td>Bilder</td><td>fertig</td></tr>\n"
        "<tr><td>JavaScript</td><td>fertig</td></tr></table>\n"
        "</section>\n"
        "<section><h2>Bilder</h2>\n"
        "<img src=\"/Medien/wappen.png\" width=\"72\" alt=\"Wappen\">\n"
        "<img src=\"/Medien/muster.png\" width=\"192\" alt=\"Muster\">\n"
        "<img src=\"/Medien/wappen.png\" width=\"36\" alt=\"klein\">\n"
        "</section>\n"
        "<section><h2>JavaScript</h2>\n"
        "<p>Stand: <span id=\"zaehler\">0</span></p>\n"
        "<button id=\"mehr\">Eins mehr</button>\n"
        "<button id=\"zurueck\">Zuruecksetzen</button>\n"
        "<ul id=\"liste\"></ul>\n"
        "<p id=\"rechnung\"></p>\n"
        "</section>\n"
        "</main>\n"
        "<script>\n"
        "var stand = 0;\n"
        "var anzeige = document.getElementById('zaehler');\n"
        "var liste = document.getElementById('liste');\n"
        "\n"
        "function zeichnen() {\n"
        "  anzeige.textContent = stand;\n"
        "  anzeige.style.color = stand > 4 ? '#a02020' : '#204878';\n"
        "}\n"
        "\n"
        "document.getElementById('mehr').addEventListener('click',\n"
        "  function () { stand++; zeichnen() });\n"
        "document.getElementById('zurueck').addEventListener('click',\n"
        "  function () { stand = 0; zeichnen() });\n"
        "\n"
        "['Dokumentbaum', 'Formatvorlagen', 'Umbruch', 'Bilder', 'Skripte']\n"
        "  .forEach(function (name, i) {\n"
        "    var eintrag = document.createElement('li');\n"
        "    eintrag.textContent = (i + 1) + '. ' + name + ' laeuft';\n"
        "    liste.appendChild(eintrag);\n"
        "  });\n"
        "\n"
        "var quadrate = [];\n"
        "for (var i = 1; i <= 8; i++) quadrate.push(i * i);\n"
        "document.getElementById('rechnung').textContent =\n"
        "  'Quadratzahlen: ' + quadrate.join(', ') +\n"
        "  ' - Summe ' + quadrate.reduce(function (a, b) { return a + b }, 0);\n"
        "zeichnen();\n"
        "</script>\n"
        "</body>\n"
        "</html>\n", false);

    /* --- Medien --- */
    put(media, "liesmich.txt",
        "Hier wohnen die Bilder, die RetroOS mitbringt.\n"
        "Der Browser zeigt sie an; das Dateisystem nimmt gern mehr auf.\n",
        false);
    put_binary(media, "wappen.png", _binary_wappen_png_start,
               _binary_wappen_png_end);
    put_binary(media, "muster.png", _binary_muster_png_start,
               _binary_muster_png_end);
}
