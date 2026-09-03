/* settings.c - das Einstellungsfenster.
 *
 * Zehn Dinge lassen sich hier aendern; alle landen in derselben
 * Textdatei auf der Festplatte. Ohne Festplatte gilt jede Aenderung
 * nur bis zum Ausschalten - das sagt das Fenster dann auch.
 *
 * Jede Aenderung wirkt sofort und nicht erst beim Speichern: Die
 * Sprache wechselt, waehrend das Fenster offen ist, die Schrift auch,
 * und das Hintergrundbild liegt gleich hinter dem Fenster. Speichern
 * heisst darum nur "so soll es bleiben" - was etwas ganz anderes ist
 * als "so soll es sein".
 */

#include "apps.h"
#include "config.h"
#include "display.h"
#include "font.h"
#include "user.h"
#include "keymap.h"
#include "kstring.h"
#include "mm.h"
#include "net.h"
#include "theme.h"
#include "vfs.h"
#include "widgets.h"
#include "wallpaper.h"
#include "lang.h"

#define ROW_H     30
#define LABEL_X   16
#define VALUE_X   180
#define CHOICE_W  160

enum row_id {
    ROW_LANGUAGE,
    ROW_KEYMAP,
    ROW_RESOLUTION,
    ROW_SCALE,
    ROW_CLOCK,
    ROW_TIMEZONE,
    ROW_BACKGROUND,
    ROW_WALLPAPER,
    ROW_FONT,
    ROW_HOSTNAME,
    ROW_COUNT
};

#define WALLPAPER_MAX 12

struct settings_ui {
    int  hover;          /* Knopf unter dem Zeiger, -1 = keiner */
    bool hostname_focus;
    size_t hostname_cursor;
    char status[96];

    /* Die Bilder, die zur Auswahl stehen. Platz 0 bleibt leer und
     * heisst "kein Bild" - so ist der Weg zurueck zum Verlauf
     * dieselbe Bewegung wie die Wahl eines Bildes. */
    char   images[WALLPAPER_MAX][64];
    size_t image_count;
    size_t image_at;
};

/* Die Zeitzonen, die zur Auswahl stehen - mehr braucht es nicht. */
static const struct { int32_t minutes; const char *name; } zones[] = {
    { -300, "UTC-5  New York"  },
    {    0, "UTC+0  London"    },
    {   60, "UTC+1  Berlin"    },
    {  120, "UTC+2  Athen"     },
    {  330, "UTC+5:30 Delhi"   },
    {  540, "UTC+9  Tokio"     },
};

/* Sammelt Bilder aus einem Ordner ein - nur die oberste Ebene, denn
 * wer sein Bild tiefer vergraebt, tippt den Pfad ohnehin lieber
 * selbst. */
static void collect_images(struct settings_ui *ui, const char *dir_path)
{
    struct fs_node *dir = fs_lookup(NULL, dir_path);

    if (!dir || dir->type != FS_DIR)
        return;

    struct fs_node *entries[32];
    size_t n = fs_list(dir, entries, ARRAY_LEN(entries));

    for (size_t i = 0; i < n && ui->image_count < WALLPAPER_MAX; i++) {
        if (entries[i]->type != FS_FILE)
            continue;

        const char *dot = strrchr(entries[i]->name, '.');

        if (!dot)
            continue;
        if (strcasecmp(dot, ".png") != 0 && strcasecmp(dot, ".jpg") != 0 &&
            strcasecmp(dot, ".jpeg") != 0 && strcasecmp(dot, ".gif") != 0 &&
            strcasecmp(dot, ".bmp") != 0)
            continue;

        fs_path(entries[i], ui->images[ui->image_count],
                sizeof(ui->images[0]));
        ui->image_count++;
    }
}

/* Baut die Liste neu auf und merkt sich, wo das eingestellte Bild
 * darin steht. Ein Bild, das nirgends gefunden wurde - von Hand
 * eingetippt etwa -, kommt hinten dazu; sonst waere es beim ersten
 * Klick auf einen Pfeil verloren. */
static void refresh_images(struct settings_ui *ui)
{
    const char *set = config_current()->wallpaper;
    char home[FS_PATH_MAX];

    ui->image_count = 1;
    ui->images[0][0] = '\0';
    ui->image_at = 0;

    collect_images(ui, "/Medien");
    user_home_file("Bilder", "/Medien", home, sizeof(home));
    collect_images(ui, home);

    if (!set[0])
        return;

    for (size_t i = 1; i < ui->image_count; i++) {
        if (strcmp(ui->images[i], set) == 0) {
            ui->image_at = i;
            return;
        }
    }
    if (ui->image_count < WALLPAPER_MAX) {
        strlcpy(ui->images[ui->image_count], set, sizeof(ui->images[0]));
        ui->image_at = ui->image_count++;
    }
}

/* Nur der letzte Teil des Pfades passt in das Feld. */
static const char *short_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

static size_t zone_index(void)
{
    for (size_t i = 0; i < ARRAY_LEN(zones); i++)
        if (zones[i].minutes == config_current()->timezone)
            return i;
    return 2;
}

/* ------------------------------------------------------------------ */

static struct rect row_rect(int row)
{
    return rect_make(VALUE_X, 70 + row * ROW_H, CHOICE_W, 24);
}

static struct rect arrow_rect(int row, bool right)
{
    struct rect r = row_rect(row);

    return right ? rect_make(r.x + r.w + 4, r.y, 24, 24)
                 : rect_make(r.x - 28, r.y, 24, 24);
}

static struct rect save_rect(struct window *win)
{
    return rect_make(gui_client_width(win) - 150, gui_client_height(win) - 44,
                     134, 28);
}

static void row_value(int row, char *out, size_t size)
{
    struct config *c = config_current();

    switch (row) {
    case ROW_LANGUAGE:
        strlcpy(out, lang_name(lang_current()), size);
        break;
    case ROW_KEYMAP: {
        const struct keymap *map = keymap_current();

        strlcpy(out, tr(map->name), size);
        break;
    }
    case ROW_RESOLUTION:
        if (display_can_switch())
            disp_format_mode(out, size, display_width(), display_height());
        else
            ksnprintf(out, size, "%dx%d %s", (int)display_width(),
                      (int)display_height(), tr("(fest)"));
        break;
    case ROW_SCALE:
        if (display_scale_is_auto())
            ksnprintf(out, size, "%ux %s", (unsigned)display_scale(),
                      tr("(automatisch)"));
        else
            ksnprintf(out, size, "%ux", (unsigned)display_scale());
        break;
    case ROW_CLOCK:
        strlcpy(out, c->clock == CLOCK_UTC ? "UTC" : tr("Ortszeit"), size);
        break;
    case ROW_TIMEZONE:
        strlcpy(out, zones[zone_index()].name, size);
        break;
    case ROW_BACKGROUND:
        strlcpy(out, tr(background_name(c->background)), size);
        break;
    case ROW_WALLPAPER:
        strlcpy(out, c->wallpaper[0] ? short_name(c->wallpaper)
                                     : tr("kein Bild"), size);
        break;
    case ROW_FONT:
        strlcpy(out, font_name(font_current()), size);
        break;
    case ROW_HOSTNAME:
        strlcpy(out, c->hostname, size);
        break;
    default:
        out[0] = '\0';
        break;
    }
}

static const char *row_label(int row)
{
    switch (row) {
    case ROW_LANGUAGE:   return tr("Sprache");
    case ROW_KEYMAP:     return tr("Tastatur");
    case ROW_RESOLUTION: return tr("Aufloesung");
    case ROW_SCALE:      return tr("Vergroesserung");
    case ROW_CLOCK:      return tr("Batterieuhr");
    case ROW_TIMEZONE:   return tr("Zeitzone");
    case ROW_BACKGROUND: return tr("Hintergrund");
    case ROW_WALLPAPER:  return tr("Hintergrundbild");
    case ROW_FONT:       return tr("Schrift");
    case ROW_HOSTNAME:   return tr("Rechnername");
    default:             return "";
    }
}

/* Blaettert eine Zeile eins vor oder zurueck. */
static void row_step(struct settings_ui *ui, int row, int delta)
{
    struct config *c = config_current();

    switch (row) {
    case ROW_LANGUAGE: {
        enum language before = lang_current();
        enum language next = (enum language)((before + LANG_COUNT +
                                              (delta > 0 ? 1 : LANG_COUNT - 1)) %
                                             LANG_COUNT);

        lang_select(next);
        strlcpy(c->language, lang_code(next), sizeof(c->language));

        /* Die Tastatur wandert mit - aber nur, solange sie noch die
         * ist, die zur alten Sprache gehoerte. Wer sich bewusst eine
         * andere Belegung gesucht hat, soll sie behalten; sonst waere
         * ein Blick in die englische Oberflaeche jedes Mal ein Verlust
         * seiner Umlaute. */
        if (strcasecmp(c->keymap, lang_default_keymap(before)) == 0) {
            keymap_select(lang_default_keymap(next));
            strlcpy(c->keymap, keymap_current()->code, sizeof(c->keymap));
            strlcpy(ui->status,
                    tr("Die Sprache gilt sofort - die Tastatur ist mitgewandert."),
                    sizeof(ui->status));
        }
        break;
    }
    case ROW_KEYMAP: {
        size_t count = keymap_count();
        size_t next = (keymap_current_index() + count +
                       (size_t)(delta > 0 ? 1 : count - 1)) % count;

        keymap_select_index(next);
        strlcpy(c->keymap, keymap_at(next)->code, sizeof(c->keymap));
        break;
    }
    case ROW_RESOLUTION: {
        size_t count = display_mode_count();

        if (!display_can_switch() || count < 2) {
            strlcpy(ui->status,
                    tr("Diese Grafikkarte laesst den Modus nicht wechseln."),
                    sizeof(ui->status));
            break;
        }

        /* Reihum, bis einer klappt: Eine Karte darf einen Modus
         * ablehnen, den sie in der Liste stehen hat. */
        size_t at = display_current_mode();

        for (size_t tries = 0; tries < count; tries++) {
            at = (at + count + (size_t)(delta > 0 ? 1 : count - 1)) % count;

            const struct disp_mode *mode = display_mode_at(at);

            if (mode && display_set_mode(mode->w, mode->h)) {
                disp_format_mode(c->resolution, sizeof(c->resolution),
                                 mode->w, mode->h);
                gui_invalidate();
                break;
            }
        }
        break;
    }
    case ROW_SCALE: {
        /* Der Reigen ist 1x, 2x, ... bis zum Moeglichen und dann
         * "automatisch" - so kommt man in beide Richtungen ohne
         * Umweg zu jeder Stufe. */
        uint32_t highest = disp_max_scale(display_width(), display_height());
        uint32_t steps = highest + 1;
        uint32_t now = display_scale_is_auto() ? 0 : display_scale();
        uint32_t next = (now + steps + (uint32_t)(delta > 0 ? 1 : steps - 1)) %
                        steps;

        if (display_set_scale(next)) {
            c->scale = next;
        } else {
            strlcpy(ui->status, tr("Diese Vergroesserung geht hier nicht."),
                    sizeof(ui->status));
        }
        break;
    }
    case ROW_CLOCK:
        c->clock = c->clock == CLOCK_UTC ? CLOCK_LOCAL : CLOCK_UTC;
        break;
    case ROW_TIMEZONE: {
        size_t count = ARRAY_LEN(zones);
        size_t next = (zone_index() + count +
                       (size_t)(delta > 0 ? 1 : count - 1)) % count;

        c->timezone = zones[next].minutes;
        break;
    }
    case ROW_BACKGROUND: {
        size_t count = background_count();

        c->background = (uint32_t)((c->background + count +
                                    (size_t)(delta > 0 ? 1 : count - 1)) %
                                   count);
        break;
    }
    case ROW_WALLPAPER: {
        size_t count = ui->image_count;

        if (count < 2)
            break;

        ui->image_at = (ui->image_at + count +
                        (size_t)(delta > 0 ? 1 : count - 1)) % count;

        const char *path = ui->images[ui->image_at];

        /* Auch hier gilt: Es wirkt sofort. Der Desktop liegt hinter
         * dem Fenster, die Wahl ist damit ihre eigene Vorschau. */
        if (!path[0]) {
            wallpaper_set(NULL);
            c->wallpaper[0] = '\0';
        } else if (wallpaper_set(path)) {
            strlcpy(c->wallpaper, path, sizeof(c->wallpaper));
        } else {
            strlcpy(ui->status, tr("Das Bild liess sich nicht laden."),
                    sizeof(ui->status));
            break;
        }
        gui_invalidate();
        break;
    }
    case ROW_FONT: {
        size_t count = font_count();
        size_t next = (font_current() + count +
                       (size_t)(delta > 0 ? 1 : count - 1)) % count;

        /* Wirkt sofort: das Fenster zeichnet sich gleich in der neuen
         * Schrift, die Auswahl ist damit ihre eigene Vorschau. */
        font_select(next);
        strlcpy(c->font, font_name(next), sizeof(c->font));
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */

static void settings_paint(struct window *win, struct canvas *c)
{
    struct settings_ui *ui = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);
    gfx_gradient_v(&local, rect_make(0, 0, local.w, 48),
                   COL_TITLE_A1, COL_TITLE_A2);
    icon_draw(&local, 14, 14, ICON_SETTINGS, 1);
    gfx_text_bold(&local, 40, 16, tr("Einstellungen"), COL_WHITE);

    for (int row = 0; row < ROW_COUNT; row++) {
        struct rect r = row_rect(row);
        char value[64];

        gfx_text(&local, LABEL_X, r.y + 5, row_label(row), COL_TEXT_DIM);
        row_value(row, value, sizeof(value));

        if (row == ROW_HOSTNAME) {
            struct rect field = rect_make(r.x, r.y, CHOICE_W + 56, 24);

            widget_field(&local, field, value,
                         ui->hostname_focus ? (int32_t)ui->hostname_cursor : -1,
                         ui->hostname_focus);
            continue;
        }

        widget_button(&local, arrow_rect(row, false), "<",
                      ui->hover == row * 2, true);
        widget_button(&local, arrow_rect(row, true), ">",
                      ui->hover == row * 2 + 1, true);

        struct rect box = r;

        gfx_fill(&local, box, COL_FIELD);
        gfx_bevel_thin(&local, box, false);
        gfx_text(&local, box.x + 6, box.y + 5, value, COL_TEXT);

        /* Bei der Schrift steht die Lizenz daneben - sie gehoert zur
         * Schrift und soll nicht erst im Quelltext auffindbar sein. */
        if (row == ROW_FONT)
            gfx_text(&local, box.x + box.w + 34, box.y + 5,
                     font_license(font_current()), COL_TEXT_DIM);
    }

    widget_button(&local, save_rect(win), tr("Speichern"),
                  ui->hover == 100, true);

    /* Rechts steht der Knopf - die Meldung wird davor abgeschnitten,
     * statt darunterzulaufen. */
    if (ui->status[0])
        gfx_text_clipped(&local, LABEL_X, gui_client_height(win) - 38,
                         ui->status, COL_TEXT_DIM,
                         save_rect(win).x - LABEL_X - 12);
}

static void settings_save(struct window *win)
{
    struct settings_ui *ui = win->user;

    config_apply();

    /* Die Datei gilt fuer den ganzen Rechner - Tastatur, Zeitzone,
     * Rechnername. Wer sie aendern darf, entscheidet fuer alle mit. */
    if (!session_can(CAP_CONFIG)) {
        strlcpy(ui->status, "Dauerhaft speichern darf nur, wer das Recht "
                            "an den Einstellungen hat.",
                sizeof(ui->status));
        gui_invalidate();
        return;
    }

    if (!fs_disk_mounted()) {
        strlcpy(ui->status, tr("Ohne Festplatte gilt das nur bis zum Ausschalten."),
                sizeof(ui->status));
    } else if (config_save()) {
        ksnprintf(ui->status, sizeof(ui->status), tr("Gespeichert in %s"),
                  CONFIG_PATH);
    } else {
        strlcpy(ui->status, tr("Die Datei liess sich nicht schreiben."),
                sizeof(ui->status));
    }
    gui_invalidate();
}

static void settings_event(struct window *win, const struct gui_event *ev)
{
    struct settings_ui *ui = win->user;
    struct config *cfg = config_current();

    if (ev->type == EV_MOUSE_MOVE) {
        int before = ui->hover;

        ui->hover = -1;
        for (int row = 0; row < ROW_COUNT; row++) {
            if (row == ROW_HOSTNAME)
                continue;
            if (rect_contains(arrow_rect(row, false), ev->x, ev->y))
                ui->hover = row * 2;
            else if (rect_contains(arrow_rect(row, true), ev->x, ev->y))
                ui->hover = row * 2 + 1;
        }
        if (rect_contains(save_rect(win), ev->x, ev->y))
            ui->hover = 100;
        if (before != ui->hover)
            gui_invalidate();
        return;
    }

    if (ev->type == EV_KEY_DOWN && ui->hostname_focus) {
        size_t len = strlen(cfg->hostname);

        if (ev->key == KEY_BACKSPACE && ui->hostname_cursor > 0) {
            memmove(&cfg->hostname[ui->hostname_cursor - 1],
                    &cfg->hostname[ui->hostname_cursor],
                    len - ui->hostname_cursor + 1);
            ui->hostname_cursor--;
        } else if (ev->key == KEY_ENTER) {
            ui->hostname_focus = false;
        } else if (ev->ascii > 32 && (unsigned char)ev->ascii < 127 &&
                   len + 1 < sizeof(cfg->hostname)) {
            memmove(&cfg->hostname[ui->hostname_cursor + 1],
                    &cfg->hostname[ui->hostname_cursor],
                    len - ui->hostname_cursor + 1);
            cfg->hostname[ui->hostname_cursor++] = ev->ascii;
        } else {
            return;
        }
        gui_invalidate();
        return;
    }

    if (ev->type != EV_MOUSE_DOWN || ev->button != MB_LEFT)
        return;

    if (rect_contains(save_rect(win), ev->x, ev->y)) {
        settings_save(win);
        return;
    }

    struct rect field = rect_make(row_rect(ROW_HOSTNAME).x,
                                  row_rect(ROW_HOSTNAME).y, CHOICE_W + 56, 24);

    ui->hostname_focus = rect_contains(field, ev->x, ev->y);
    if (ui->hostname_focus)
        ui->hostname_cursor = strlen(cfg->hostname);

    for (int row = 0; row < ROW_COUNT; row++) {
        if (row == ROW_HOSTNAME)
            continue;
        if (!rect_contains(arrow_rect(row, false), ev->x, ev->y) &&
            !rect_contains(arrow_rect(row, true), ev->x, ev->y))
            continue;

        /* Die alte Meldung gilt nicht mehr, sobald etwas anderes
         * eingestellt wird - eine neue darf row_step aber setzen. */
        ui->status[0] = '\0';
        row_step(ui, row,
                 rect_contains(arrow_rect(row, false), ev->x, ev->y) ? -1 : +1);
        break;
    }
    gui_invalidate();
}

static void settings_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_settings(void)
{
    struct window *existing = gui_find_by_paint(settings_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct settings_ui *ui = kzalloc(sizeof(*ui));

    if (!ui)
        return;

    ui->hover = -1;
    refresh_images(ui);
    if (!fs_disk_mounted())
        strlcpy(ui->status, tr("Ohne Festplatte bleibt nichts gespeichert."),
                sizeof(ui->status));

    struct window *win = gui_create_window(tr("Einstellungen"), 0, 0, 620, 442,
                                           WF_CENTER, ICON_SETTINGS);
    if (!win) {
        kfree(ui);
        return;
    }

    win->user     = ui;
    win->on_paint = settings_paint;
    win->on_event = settings_event;
    win->on_close = settings_close;

    gui_focus_window(win);
}
