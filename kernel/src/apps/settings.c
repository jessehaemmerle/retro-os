/* settings.c - das Einstellungsfenster.
 *
 * Sechs Dinge lassen sich hier aendern; alle landen in derselben
 * Textdatei auf der Festplatte. Ohne Festplatte gilt jede Aenderung
 * nur bis zum Ausschalten - das sagt das Fenster dann auch.
 */

#include "apps.h"
#include "config.h"
#include "font.h"
#include "user.h"
#include "keymap.h"
#include "kstring.h"
#include "mm.h"
#include "net.h"
#include "theme.h"
#include "vfs.h"
#include "widgets.h"

#define ROW_H     30
#define LABEL_X   16
#define VALUE_X   180
#define CHOICE_W  160

enum row_id {
    ROW_KEYMAP,
    ROW_CLOCK,
    ROW_TIMEZONE,
    ROW_BACKGROUND,
    ROW_FONT,
    ROW_HOSTNAME,
    ROW_COUNT
};

struct settings_ui {
    int  hover;          /* Knopf unter dem Zeiger, -1 = keiner */
    bool hostname_focus;
    size_t hostname_cursor;
    char status[80];
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
    case ROW_KEYMAP: {
        const struct keymap *map = keymap_current();

        strlcpy(out, map->name, size);
        break;
    }
    case ROW_CLOCK:
        strlcpy(out, c->clock == CLOCK_UTC ? "UTC" : "Ortszeit", size);
        break;
    case ROW_TIMEZONE:
        strlcpy(out, zones[zone_index()].name, size);
        break;
    case ROW_BACKGROUND:
        strlcpy(out, background_name(c->background), size);
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
    case ROW_KEYMAP:     return "Tastatur";
    case ROW_CLOCK:      return "Batterieuhr";
    case ROW_TIMEZONE:   return "Zeitzone";
    case ROW_BACKGROUND: return "Hintergrund";
    case ROW_FONT:       return "Schrift";
    case ROW_HOSTNAME:   return "Rechnername";
    default:             return "";
    }
}

/* Blaettert eine Zeile eins vor oder zurueck. */
static void row_step(int row, int delta)
{
    struct config *c = config_current();

    switch (row) {
    case ROW_KEYMAP: {
        size_t count = keymap_count();
        size_t next = (keymap_current_index() + count +
                       (size_t)(delta > 0 ? 1 : count - 1)) % count;

        keymap_select_index(next);
        strlcpy(c->keymap, keymap_at(next)->code, sizeof(c->keymap));
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
    gfx_text_bold(&local, 40, 16, "Einstellungen", COL_WHITE);

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

    widget_button(&local, save_rect(win), "Speichern",
                  ui->hover == 100, true);

    if (ui->status[0])
        gfx_text(&local, LABEL_X, gui_client_height(win) - 38, ui->status,
                 COL_TEXT_DIM);
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
        strlcpy(ui->status, "Ohne Festplatte gilt das nur bis zum Ausschalten.",
                sizeof(ui->status));
    } else if (config_save()) {
        ksnprintf(ui->status, sizeof(ui->status), "Gespeichert in %s",
                  CONFIG_PATH);
    } else {
        strlcpy(ui->status, "Die Datei liess sich nicht schreiben.",
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
        if (rect_contains(arrow_rect(row, false), ev->x, ev->y))
            row_step(row, -1);
        else if (rect_contains(arrow_rect(row, true), ev->x, ev->y))
            row_step(row, +1);
        else
            continue;

        ui->status[0] = '\0';
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
    if (!fs_disk_mounted())
        strlcpy(ui->status, "Ohne Festplatte bleibt nichts gespeichert.",
                sizeof(ui->status));

    struct window *win = gui_create_window("Einstellungen", 0, 0, 560, 322,
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
