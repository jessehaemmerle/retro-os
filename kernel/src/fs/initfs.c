/* initfs.c - der Datenbestand, mit dem RetroOS startet.
 *
 * Das Dateisystem liegt im RAM und ist nach jedem Neustart wieder in diesem
 * Zustand. Hier entsteht also gewissermassen die Auslieferungskonfiguration.
 */

#include "vfs.h"
#include "kstring.h"

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
        "Der Bootloader (Limine) steht unter der BSD-2-Clause-Lizenz.\n", true);

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
    put(programs, "info.txt",
        "Die Programme von RetroOS sind fest in den Kernel eingebaut.\n"
        "Sie werden ueber das Startmenue geoeffnet.\n", true);

    put(docs, "beispiel.html",
        "<html>\n"
        "<head><title>Beispielseite</title></head>\n"
        "<body>\n"
        "<h1>Eine Seite aus dem Dateisystem</h1>\n"
        "<p>Diese Datei liegt unter <b>/Dokumente/beispiel.html</b> und wird\n"
        "vom Browser aus dem Dateisystem geladen - ganz ohne Netzwerk.</p>\n"
        "<h2>Was der Browser kann</h2>\n"
        "<ul>\n"
        "<li>Ueberschriften, Absaetze und Aufzaehlungen</li>\n"
        "<li>Fett ausgezeichneten <b>Text</b></li>\n"
        "<li>Verweise, etwa zurueck zur <a href=\"start:\">Startseite</a></li>\n"
        "<li>Seiten aus dem Netz ueber HTTP</li>\n"
        "</ul>\n"
        "<hr>\n"
        "<p>Umlaute funktionieren auch: Gr&uuml;&szlig;e aus RetroOS.</p>\n"
        "</body>\n"
        "</html>\n", false);

    /* --- Medien --- */
    put(media, "liesmich.txt",
        "Hier waeren Bilder und Klaenge zu Hause.\n"
        "Bislang beherrscht RetroOS nur Text - das darf sich aendern.\n", false);
}
