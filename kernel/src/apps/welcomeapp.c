/* welcomeapp.c - das Begruessungsfenster.
 *
 * Ein Betriebssystem, das man nicht kennt, ist erst einmal ein
 * Bildschirm mit Symbolen darauf. Was es alles kann, steht sonst
 * nirgends - ausser in der Anleitung, die niemand liest, bevor etwas
 * schiefgeht.
 *
 * Darum dieses Fenster: links die Kapitel, rechts das, was in jedem
 * steckt, und ein Knopf, der das beschriebene Programm gleich
 * aufmacht. Erklaeren und Ausprobieren liegen damit einen Klick
 * auseinander.
 *
 * Es geht beim Start von selbst auf, solange der Haken unten links
 * steht. Wer es nicht mehr sehen will, nimmt ihn heraus - auf einem
 * installierten System bleibt das gemerkt.
 */

#include "apps.h"
#include "config.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"
#include "theme.h"
#include "widgets.h"
#include "lang.h"

#define SIDEBAR_W   196
#define ROW_H       30
#define LINE_H      19
#define TEXT_MAX    12

struct chapter {
    const char  *title;           /* kurz, fuer die Leiste links */
    const char  *heading;         /* lang, ueber dem Text        */
    enum icon_id icon;
    const char  *action;          /* Beschriftung des Knopfes, NULL = keiner */
    void       (*launch)(void);
    const char  *lines[TEXT_MAX];
};

/* Die Reihenfolge ist die eines ersten Rundgangs: erst der
 * Bildschirm, dann die Dateien, dann die Programme, dann das Netz,
 * und ganz zuletzt, wie man es dauerhaft macht. */
static const struct chapter chapters[] = {
{
    "Willkommen", "Willkommen bei RetroOS", ICON_INFO, NULL, NULL,
    {
      "RetroOS ist ein Betriebssystem, das bei null anfaengt: eigener",
      "Kern, eigene Treiber, eigenes Fenstersystem, eigener Browser.",
      "Es startet auf echter Hardware wie in einer virtuellen Maschine.",
      "",
      "Links stehen die Kapitel. Jedes erklaert einen Teil des Systems,",
      "und wo es etwas auszuprobieren gibt, steht unten rechts",
      "ein Knopf, der es gleich aufmacht.",
      "",
      "Der Haken unten links entscheidet, ob dieses Fenster beim",
      "naechsten Start wieder aufgeht.",
      NULL }
},
{
    "Fenster", "Arbeitsflaeche und Fenster", ICON_COMPUTER, NULL, NULL,
    {
      "Doppelklick auf ein Symbol startet ein Programm, der Start-Knopf",
      "unten links zeigt alle. Fenster lassen sich an der Titelleiste",
      "ziehen und an der Ecke unten rechts groesser machen.",
      "",
      "Alt+Eingabe maximiert, Alt+Links und Alt+Rechts docken an eine",
      "Haelfte an, Alt+Tabulator holt das naechste Fenster nach vorne,",
      "Alt+F4 schliesst.",
      "",
      "Es gibt vier Arbeitsflaechen: die Felder rechts in der",
      "Taskleiste oder Strg+Alt+Links/Rechts. Alt+Umschalt dazu",
      "nimmt das vorderste Fenster mit hinueber.",
      NULL }
},
{
    "Rechte Maustaste", "Die rechte Maustaste", ICON_LIST, NULL, NULL,
    {
      "Sie fragt ueberall, was hier moeglich ist.",
      "",
      "Auf der Arbeitsflaeche: Suche, Dateimanager, Bildschirmfoto,",
      "heller oder dunkler Modus, der naechste Hintergrund.",
      "",
      "Auf der Titelleiste und auf einem Knopf der Taskleiste steht das",
      "Fenstermenue - andocken, auf eine andere Arbeitsflaeche",
      "schieben, schliessen.",
      "",
      "Im Text: Ausschneiden, Kopieren, Einfuegen. Jedes Menue laesst",
      "sich auch mit den Pfeiltasten bedienen.",
      NULL }
},
{
    "Suche, Meldungen", "Suchen und Benachrichtigungen", ICON_SEARCH, "Suche oeffnen", app_search,
    {
      "Alt+Leertaste oeffnet die Suche. Getippt wird, gesucht wird",
      "sofort - in Programmen, Dateien und Einstellungen zugleich.",
      "Die Pfeiltasten waehlen, Eingabe oeffnet, Escape macht zu.",
      "",
      "Was das System nur mitteilen will - ein fertiges Bildschirmfoto,",
      "ein gepacktes Archiv, ein beendeter Download -, erscheint rechts",
      "unten und geht nach fuenf Sekunden von selbst wieder.",
      "",
      "Der Verlauf bleibt hinter der Glocke in der Taskleiste stehen.",
      NULL }
},
{
    "Dateien", "Dateien und Datentraeger", ICON_FOLDER_OPEN, "Dateimanager oeffnen", app_filemanager,
    {
      "Der Zweig /Festplatte liegt auf dem Datentraeger, alles",
      "andere im Arbeitsspeicher - und ist nach dem Ausschalten weg.",
      "",
      "Geloeschtes wandert in den Papierkorb und laesst sich von dort",
      "zurueckholen. Die rechte Maustaste im Dateimanager packt einen",
      "Ordner in ein ZIP, packt eines aus, aendert Rechte oder legt ein",
      "Bild als Hintergrund fest.",
      "",
      "Doppelklick oeffnet jede Datei mit dem Programm, das zu ihrer",
      "Endung gehoert.",
      NULL }
},
{
    "Programme", "Die Programme", ICON_TABLE, "Rechner oeffnen", app_calculator,
    {
      "Editor und Programmieren fuer Text und Quelltext - das zweite",
      "fuehrt JavaScript gleich im Fenster aus.",
      "",
      "Tabelle rechnet mit Formeln, Schreiben kennt Absatzformate,",
      "Vortrag hat einen Vorfuehrmodus.",
      "",
      "Dazu Rechner, Bildbetrachter, Kalender, Uhr mit Stoppuhr,",
      "Aufgabenliste, Systemmonitor und Protokoll.",
      "",
      "Alle stehen im Startmenue - oder eine Suche weit entfernt.",
      NULL }
},
{
    "Konsole", "Die Konsole", ICON_TERMINAL, "Konsole oeffnen", app_terminal,
    {
      "hilfe zeigt alle Befehle nach Gebiet geordnet, hilfe <befehl>",
      "und man <befehl> erklaeren einen einzelnen.",
      "",
      "Fast jeder Befehl hat neben dem deutschen Namen den gewohnten",
      "englischen als Zweitnamen: kopiere und cp, suche und grep.",
      "",
      "Es gibt Roehren, Umleitungen, Variablen und Skripte - und",
      "Befehle fuer Netz, Platten, Benutzer und Prozesse.",
      "",
      "Die Pfeiltasten holen die letzten zwanzig Zeilen zurueck.",
      NULL }
},
{
    "Netz und Browser", "Netz und Browser", ICON_BROWSER, "Browser oeffnen", app_browser,
    {
      "Die Adresse kommt per DHCP, Namen loest DNS auf. Der Browser",
      "spricht HTTP und HTTPS - TLS 1.3 mit eigener Kryptografie und",
      "152 eingebauten Wurzelzertifikaten.",
      "",
      "Er stellt HTML mit CSS dar, zeigt Bilder und fuehrt JavaScript",
      "aus. Der gruene Pfeil laedt eine Adresse als Datei herunter.",
      "",
      "RetroOS kann auch selbst zuhoeren: Der Webserver ist ein",
      "gewoehnliches Programm im Ring 3.",
      NULL }
},
{
    "Sicherheit", "Benutzer und Sicherheit", ICON_SHIELD, "Benutzer verwalten", app_users,
    {
      "Jede Datei hat Eigentuemer, Gruppe und Rechte nach dem Muster",
      "rwxrwxrwx. Rollen fassen Faehigkeiten zusammen - Konten, Netz,",
      "Platte, Protokoll, Strom, Einstellungen -, statt nur zwischen",
      "Verwalter und Nicht-Verwalter zu unterscheiden.",
      "",
      "Programme laufen in einem Kaefig: erlaubte Systemaufrufe, ein",
      "Wurzelpfad im Dateibaum, eine Speichergrenze.",
      "",
      "Die Pruefspur haelt fest, wer was woran versucht hat.",
      NULL }
},
{
    "Aussehen", "Sprache, Farben, Schrift", ICON_SETTINGS, "Einstellungen oeffnen", app_settings,
    {
      "Sprache und Tastaturbelegung, Aufloesung und Vergroesserung,",
      "Hintergrund und eigenes Hintergrundbild, zehn Schriftarten.",
      "",
      "Erscheinungsbild schaltet zwischen hell und dunkel - oder",
      "automatisch, dann ist es zwischen 19 und 7 Uhr dunkel.",
      "",
      "Kantenglaettung macht die Schrift weich: Graustufen taugt auf",
      "jedem Bildschirm, Subpixel steuert die drei Leuchtpunkte eines",
      "LCD einzeln an - das Verfahren, das als ClearType bekannt ist.",
      "Es braucht einen LCD ohne Vergroesserung.",
      NULL }
},
{
    "Installieren", "Auf die Festplatte", ICON_DISK, "Installieren", app_setup,
    {
      "Bis hierher laeuft alles vom Startmedium: Jede Aenderung",
      "ist nach dem Ausschalten weg.",
      "",
      "Das Installationsprogramm schreibt eine Partitionstabelle,",
      "formatiert FAT32, kopiert Bootloader und Kern und macht den",
      "Datentraeger startbar - fuer UEFI wie fuer BIOS.",
      "",
      "Danach bleiben Einstellungen, Benutzer und Dateien erhalten,",
      "und dieses Fenster merkt sich, ob es noch aufgehen soll.",
      NULL }
},
{
    "Ueber RetroOS", "Ueber RetroOS", ICON_KEY, "Systeminformation", app_sysinfo,
    {
      "Geschrieben in C und etwas Assembler, ohne fremde Bibliotheken.",
      "Der Quelltext ist auf Deutsch kommentiert und erklaert, warum",
      "etwas so ist - nicht, was dasteht.",
      "",
      "Die Schriften stammen von DejaVu, Liberation, JetBrains, IBM,",
      "Mozilla, Adobe, Ubuntu und anderen, die Symbole von Lucide.",
      "Ihre Lizenzen liegen unter third_party.",
      "",
      "Was sich ohne Bildschirm pruefen laesst, wird geprueft: ueber",
      "1400 Pruefungen in einundzwanzig Sammlungen.",
      NULL }
},
};

#define CHAPTER_COUNT ((int)ARRAY_LEN(chapters))

struct welcome_ui {
    int  at;              /* das aufgeschlagene Kapitel */
    int  hover;           /* -1 keiner, sonst Kapitelnummer */
    bool action_hover;
    bool next_hover;
    bool back_hover;
};

/* --- Masse ---------------------------------------------------------- */

static struct rect chapter_rect(int index)
{
    return rect_make(6, 8 + index * ROW_H, SIDEBAR_W - 12, ROW_H - 2);
}

static struct rect action_rect(struct window *win)
{
    return rect_make(SIDEBAR_W + 16, gui_client_height(win) - 40, 190, 28);
}

static struct rect back_rect(struct window *win)
{
    return rect_make(gui_client_width(win) - 190, gui_client_height(win) - 40,
                     84, 28);
}

static struct rect next_rect(struct window *win)
{
    return rect_make(gui_client_width(win) - 100, gui_client_height(win) - 40,
                     84, 28);
}

static struct rect check_rect(struct window *win)
{
    return rect_make(10, gui_client_height(win) - 34, 14, 14);
}

/* --- Zeichnen -------------------------------------------------------- */

static void paint_sidebar(struct canvas *c, struct welcome_ui *ui)
{
    struct rect bar = rect_make(0, 0, SIDEBAR_W, c->h);

    gfx_fill(c, bar, COL_FACE_LIGHT);
    gfx_vline(c, SIDEBAR_W - 1, 0, c->h, COL_SHADOW);

    for (int i = 0; i < CHAPTER_COUNT; i++) {
        struct rect r = chapter_rect(i);
        bool here = (i == ui->at);

        if (here)
            gfx_fill(c, r, COL_SELECT);
        else if (i == ui->hover)
            gfx_fill(c, r, COL_FACE);

        icon_draw(c, r.x + 6, r.y + 6, chapters[i].icon, 1);
        gfx_set_clip(c, rect_intersect(c->clip, r));
        gfx_text_clipped(c, r.x + 28, r.y + 8, tr(chapters[i].title),
                         here ? COL_SELECT_TEXT : COL_TEXT, r.w - 34);
        gfx_reset_clip(c);
    }
}

static void welcome_paint(struct window *win, struct canvas *c)
{
    struct welcome_ui *ui = win->user;
    struct canvas local = gui_client_canvas(win, c);
    const struct chapter *ch = &chapters[ui->at];

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);
    paint_sidebar(&local, ui);

    int32_t x = SIDEBAR_W + 16;

    icon_draw(&local, x, 14, ch->icon, 2);
    gfx_text_bold(&local, x + 44, 24, tr(ch->heading), COL_TEXT);
    gfx_hline(&local, x, 62, local.w - x - 16, COL_SHADOW);

    for (int i = 0; i < TEXT_MAX && ch->lines[i]; i++) {
        if (!ch->lines[i][0])
            continue;
        gfx_text(&local, x, 78 + i * LINE_H, tr(ch->lines[i]), COL_TEXT);
    }

    /* Wo bin ich? Eine Zeile, die man sonst zaehlen muesste. */
    char step[32];

    ksnprintf(step, sizeof(step), "%d / %d", ui->at + 1, CHAPTER_COUNT);
    gfx_text(&local, x, local.h - 34, step, COL_TEXT_DIM);

    if (ch->action)
        widget_button(&local, action_rect(win), tr(ch->action),
                      ui->action_hover, true);

    widget_button(&local, back_rect(win), tr("Zurueck"), ui->back_hover,
                  ui->at > 0);
    widget_button(&local, next_rect(win), tr("Weiter"), ui->next_hover,
                  ui->at + 1 < CHAPTER_COUNT);

    /* Der Haken sitzt links unten in der Seitenleiste - dort, wo er
     * niemanden stoert und trotzdem zu finden ist. */
    struct rect box = check_rect(win);

    gfx_fill(&local, box, COL_FIELD);
    gfx_bevel(&local, box, false);
    if (config_current()->welcome) {
        gfx_line(&local, box.x + 3, box.y + 7, box.x + 6, box.y + 10, COL_TEXT);
        gfx_line(&local, box.x + 6, box.y + 10, box.x + 11, box.y + 3, COL_TEXT);
        gfx_line(&local, box.x + 3, box.y + 8, box.x + 6, box.y + 11, COL_TEXT);
        gfx_line(&local, box.x + 6, box.y + 11, box.x + 11, box.y + 4, COL_TEXT);
    }
    gfx_text(&local, box.x + 20, box.y + 1, tr("Beim Start zeigen"),
             COL_TEXT_DIM);
}

/* --- Ereignisse ------------------------------------------------------ */

static void show(struct welcome_ui *ui, int at)
{
    ui->at = CLAMP(at, 0, CHAPTER_COUNT - 1);
    gui_invalidate();
}

static void welcome_event(struct window *win, const struct gui_event *ev)
{
    struct welcome_ui *ui = win->user;

    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int hover = -1;

        if (ev->x < SIDEBAR_W) {
            for (int i = 0; i < CHAPTER_COUNT; i++) {
                if (rect_contains(chapter_rect(i), ev->x, ev->y))
                    hover = i;
            }
        }

        bool a = rect_contains(action_rect(win), ev->x, ev->y);
        bool n = rect_contains(next_rect(win), ev->x, ev->y);
        bool b = rect_contains(back_rect(win), ev->x, ev->y);

        if (hover != ui->hover || a != ui->action_hover ||
            n != ui->next_hover || b != ui->back_hover) {
            ui->hover = hover;
            ui->action_hover = a;
            ui->next_hover = n;
            ui->back_hover = b;
            gui_invalidate();
        }
        break;
    }
    case EV_MOUSE_DOWN:
        for (int i = 0; i < CHAPTER_COUNT; i++) {
            if (ev->x < SIDEBAR_W && rect_contains(chapter_rect(i), ev->x, ev->y)) {
                show(ui, i);
                return;
            }
        }

        if (rect_contains(next_rect(win), ev->x, ev->y)) {
            show(ui, ui->at + 1);
        } else if (rect_contains(back_rect(win), ev->x, ev->y)) {
            show(ui, ui->at - 1);
        } else if (rect_contains(rect_make(check_rect(win).x,
                                           check_rect(win).y - 2, 180, 20),
                                 ev->x, ev->y)) {
            struct config *cfg = config_current();

            cfg->welcome = !cfg->welcome;

            /* Wer den Haken herausnimmt, meint es dauerhaft - sofern
             * es eine Platte gibt, auf der das stehen bleiben kann. */
            if (fs_disk_mounted())
                config_save();
            gui_invalidate();
        } else if (chapters[ui->at].action &&
                   rect_contains(action_rect(win), ev->x, ev->y)) {
            chapters[ui->at].launch();
        }
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_DOWN || ev->key == KEY_PAGEDOWN)
            show(ui, ui->at + 1);
        else if (ev->key == KEY_UP || ev->key == KEY_PAGEUP)
            show(ui, ui->at - 1);
        else if (ev->key == KEY_HOME)
            show(ui, 0);
        else if (ev->key == KEY_END)
            show(ui, CHAPTER_COUNT - 1);
        else if (ev->key == KEY_ESCAPE)
            gui_close_window(win);
        break;
    default:
        break;
    }
}

static void welcome_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_welcome(void)
{
    struct window *existing = gui_find_by_paint(welcome_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct welcome_ui *ui = kzalloc(sizeof(*ui));

    if (!ui)
        return;

    ui->hover = -1;

    struct window *win = gui_create_window(tr("Willkommen"), 0, 0, 764, 460,
                                           WF_CENTER | WF_RESIZABLE, ICON_INFO);
    if (!win) {
        kfree(ui);
        return;
    }

    win->user     = ui;
    win->min_w    = 700;
    win->min_h    = 400;
    win->on_paint = welcome_paint;
    win->on_event = welcome_event;
    win->on_close = welcome_close;

    gui_focus_window(win);
}
