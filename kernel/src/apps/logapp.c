/* logapp.c - das Protokollfenster.
 *
 * Eine Liste, die von selbst am Ende bleibt, solange man sie nicht
 * anfasst - so wie man ein laufendes Protokoll auch lesen moechte.
 * Scrollt jemand nach oben, hoert das auf, bis er wieder ganz unten
 * ist; sonst wuerde ihm die Zeile, die er gerade liest, unter dem
 * Blick weglaufen.
 *
 * Oben eine Leiste mit den Dringlichkeiten. Sie sind zugleich Filter
 * und Zaehler: Wie viele Warnungen es gibt, sieht man, bevor man
 * danach sucht.
 */

#include "apps.h"

#include "arch.h"
#include "kstring.h"
#include "log.h"
#include "mm.h"
#include "theme.h"
#include "user.h"
#include "widgets.h"

#define ROW_H     16
#define TOOLBAR_H 34
#define STATUS_H  22
#define TIME_W    88
#define LEVEL_W   32
#define SOURCE_W  96

enum filter_id {
    F_ALL,
    F_INFO,
    F_WARN,
    F_ERROR,
    F_COUNT
};

enum button_id {
    B_SAVE = F_COUNT,
    B_CLEAR,
    B_COUNT
};

struct log_ui {
    int      filter;
    int32_t  scroll;
    bool     follow;          /* haengt die Anzeige am Ende? */
    int      hover;
    char     status[96];
    uint32_t last_seen;       /* Nummer der zuletzt gezeigten Meldung */
};

/* ------------------------------------------------------------------ */

static const char *filter_label(int id)
{
    switch (id) {
    case F_INFO:  return "Hinweise";
    case F_WARN:  return "Warnungen";
    case F_ERROR: return "Fehler";
    case B_SAVE:  return "Speichern";
    case B_CLEAR: return "Leeren";
    default:      return "Alle";
    }
}

static bool passes(const struct log_ui *ui, const struct log_entry *e)
{
    switch (ui->filter) {
    case F_INFO:  return e->level == LOG_INFO || e->level == LOG_DEBUG;
    case F_WARN:  return e->level == LOG_WARN;
    case F_ERROR: return e->level == LOG_ERROR;
    default:      return true;
    }
}

/* Der Filter arbeitet auf der Anzeige, nicht auf dem Ring: Die Liste
 * wird bei jedem Bild neu durchgezaehlt. Bei ein paar hundert Zeilen
 * ist das billiger als eine zweite Liste, die man pflegen muesste. */
static size_t visible_count(const struct log_ui *ui)
{
    size_t n = 0;
    size_t total = log_count();
    struct log_entry e;

    for (size_t i = 0; i < total; i++)
        if (log_get(i, &e) && passes(ui, &e))
            n++;
    return n;
}

static bool visible_at(const struct log_ui *ui, size_t index,
                       struct log_entry *out)
{
    size_t total = log_count();
    size_t n = 0;

    for (size_t i = 0; i < total; i++) {
        if (!log_get(i, out) || !passes(ui, out))
            continue;
        if (n++ == index)
            return true;
    }
    return false;
}

static struct rect list_rect(struct window *win)
{
    return rect_make(0, TOOLBAR_H, gui_client_width(win),
                     gui_client_height(win) - TOOLBAR_H - STATUS_H);
}

static int32_t rows_fitting(struct window *win)
{
    return MAX(list_rect(win).h / ROW_H, 1);
}

static struct rect button_rect(int id)
{
    static const int32_t width[B_COUNT] = { 66, 100, 108, 80, 92, 74 };
    int32_t x = 6;

    for (int i = 0; i < id; i++)
        x += width[i] + 4;
    return rect_make(x, 5, width[id], 24);
}

static uint32_t level_color(uint8_t level)
{
    switch (level) {
    case LOG_ERROR: return RGB(0xC0, 0x28, 0x28);
    case LOG_WARN:  return RGB(0xA0, 0x60, 0x00);
    case LOG_DEBUG: return COL_TEXT_DIM;
    default:        return COL_TEXT;
    }
}

/* ------------------------------------------------------------------ */

static void clamp_scroll(struct window *win, struct log_ui *ui)
{
    int32_t rows = rows_fitting(win);
    int32_t max = (int32_t)visible_count(ui) - rows;

    if (max < 0)
        max = 0;
    if (ui->scroll > max)
        ui->scroll = max;
    if (ui->scroll < 0)
        ui->scroll = 0;
    if (ui->follow)
        ui->scroll = max;
}

static void log_paint(struct window *win, struct canvas *c)
{
    struct log_ui *ui = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    /* --- Leiste --- */
    widget_toolbar(&local, rect_make(0, 0, local.w, TOOLBAR_H));

    for (int i = 0; i < B_COUNT; i++) {
        struct rect r = button_rect(i);
        char label[32];

        if (i < F_COUNT) {
            size_t n = i == F_ALL ? log_count()
                                  : log_count_level(i == F_INFO ? LOG_INFO
                                                    : i == F_WARN ? LOG_WARN
                                                                  : LOG_ERROR);

            ksnprintf(label, sizeof(label), "%s %u", filter_label(i),
                      (unsigned)n);
        } else {
            strlcpy(label, filter_label(i), sizeof(label));
        }

        widget_button(&local, r, label,
                      i < F_COUNT ? ui->filter == i : ui->hover == i,
                      i != B_CLEAR || (session_caps() & CAP_LOG) != 0);
    }

    /* --- Liste --- */
    struct rect l = list_rect(win);

    gfx_fill(&local, l, COL_FIELD);
    gfx_bevel_thin(&local, l, false);
    gfx_set_clip(&local, l);

    int32_t rows = rows_fitting(win);
    struct log_entry e;

    for (int32_t i = 0; i < rows; i++) {
        if (!visible_at(ui, (size_t)(ui->scroll + i), &e))
            break;

        int32_t y = l.y + 1 + i * ROW_H;
        char    time[16];

        ksnprintf(time, sizeof(time), "%5u.%03u", (unsigned)(e.ms / 1000),
                  (unsigned)(e.ms % 1000));

        /* Warnungen und Fehler bekommen einen blassen Streifen - so
         * findet das Auge sie beim Ueberfliegen, ohne die Farbe des
         * Textes zu ueberdecken. */
        if (e.level == LOG_ERROR)
            gfx_fill(&local, rect_make(l.x + 1, y, l.w - 2, ROW_H),
                     RGB(0xFF, 0xE4, 0xE4));
        else if (e.level == LOG_WARN)
            gfx_fill(&local, rect_make(l.x + 1, y, l.w - 2, ROW_H),
                     RGB(0xFF, 0xF6, 0xDE));

        gfx_text(&local, l.x + 6, y, time, COL_TEXT_DIM);
        gfx_text(&local, l.x + 6 + TIME_W, y, log_level_short(e.level),
                 level_color(e.level));
        gfx_text_clipped(&local, l.x + 6 + TIME_W + LEVEL_W, y, e.source,
                         COL_TEXT_DIM, SOURCE_W - 6);
        gfx_text_clipped(&local, l.x + 6 + TIME_W + LEVEL_W + SOURCE_W, y,
                         e.text, level_color(e.level),
                         l.w - TIME_W - LEVEL_W - SOURCE_W - 16);
    }
    gfx_reset_clip(&local);

    int32_t total = (int32_t)visible_count(ui);

    if (total > rows)
        widget_vscroll(&local,
                       rect_make(l.x + l.w - SCROLLBAR_WIDTH, l.y,
                                 SCROLLBAR_WIDTH, l.h),
                       ui->scroll, total, rows);

    /* --- Fusszeile --- */
    char left[96];

    if (ui->status[0]) {
        strlcpy(left, ui->status, sizeof(left));
    } else if (log_lost()) {
        ksnprintf(left, sizeof(left),
                  "%d von %u Meldungen - %u aeltere sind aus dem Ring gefallen",
                  total, (unsigned)log_count(), (unsigned)log_lost());
    } else {
        ksnprintf(left, sizeof(left), "%d von %u Meldungen", total,
                  (unsigned)log_count());
    }

    widget_statusbar(&local, rect_make(0, local.h - STATUS_H, local.w, STATUS_H),
                     left, ui->follow ? "haengt am Ende" : "angehalten");
}

/* ------------------------------------------------------------------ */

static void do_save(struct window *win, struct log_ui *ui)
{
    char path[FS_PATH_MAX];

    UNUSED(win);

    /* In das eigene Heim, wenn es eines gibt - dort darf jeder
     * schreiben. Sonst neben die Einstellungen, was dann aber nur ein
     * Verwalter darf. */
    user_home_file("protokoll.txt", LOG_PATH_DEFAULT, path, sizeof(path));

    if (log_save(path))
        ksnprintf(ui->status, sizeof(ui->status), "Gesichert in %s", path);
    else
        ksnprintf(ui->status, sizeof(ui->status),
                  "%s liess sich nicht schreiben.", path);
}

static void log_event(struct window *win, const struct gui_event *ev)
{
    struct log_ui *ui = win->user;
    int32_t rows = rows_fitting(win);

    if (ev->type == EV_TICK) {
        /* Neue Meldungen sollen von selbst erscheinen - aber nur ein
         * neues Bild ausloesen, wenn wirklich etwas dazugekommen ist. */
        struct log_entry last;
        size_t count = log_count();

        if (count && log_get(count - 1, &last) && last.seq != ui->last_seen) {
            ui->last_seen = last.seq;
            clamp_scroll(win, ui);
            gui_invalidate();
        }
        return;
    }

    if (ev->type == EV_SCROLL) {
        ui->follow = false;
        ui->scroll -= ev->scroll * 3;
        clamp_scroll(win, ui);

        /* Wer wieder ganz unten ankommt, haengt auch wieder am Ende. */
        if (ui->scroll >= (int32_t)visible_count(ui) - rows)
            ui->follow = true;
        gui_invalidate();
        return;
    }

    if (ev->type == EV_KEY_DOWN) {
        switch (ev->key) {
        case KEY_HOME:     ui->follow = false; ui->scroll = 0; break;
        case KEY_END:      ui->follow = true;                  break;
        case KEY_PAGEUP:   ui->follow = false; ui->scroll -= rows; break;
        case KEY_PAGEDOWN: ui->scroll += rows;                 break;
        case KEY_UP:       ui->follow = false; ui->scroll--;   break;
        case KEY_DOWN:     ui->scroll++;                       break;
        default:           return;
        }
        clamp_scroll(win, ui);
        gui_invalidate();
        return;
    }

    if (ev->type == EV_MOUSE_MOVE) {
        int before = ui->hover;

        ui->hover = -1;
        for (int i = F_COUNT; i < B_COUNT; i++)
            if (rect_contains(button_rect(i), ev->x, ev->y))
                ui->hover = i;
        if (before != ui->hover)
            gui_invalidate();
        return;
    }

    if (ev->type != EV_MOUSE_DOWN || ev->button != MB_LEFT)
        return;

    for (int i = 0; i < B_COUNT; i++) {
        if (!rect_contains(button_rect(i), ev->x, ev->y))
            continue;

        ui->status[0] = '\0';

        if (i < F_COUNT) {
            ui->filter = i;
            ui->follow = true;
        } else if (i == B_SAVE) {
            do_save(win, ui);
        } else if (session_can(CAP_LOG)) {
            log_clear();
        } else {
            strlcpy(ui->status,
                    "Leeren darf nur, wer das Recht am Protokoll hat.",
                    sizeof(ui->status));
        }

        clamp_scroll(win, ui);
        gui_invalidate();
        return;
    }
}

static void log_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_log(void)
{
    struct window *existing = gui_find_by_paint(log_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct log_ui *ui = kzalloc(sizeof(*ui));

    if (!ui)
        return;

    ui->follow = true;
    ui->hover  = -1;

    struct window *win = gui_create_window("Protokoll", 0, 0, 860, 460,
                                           WF_CENTER | WF_RESIZABLE, ICON_LOG);
    if (!win) {
        kfree(ui);
        return;
    }

    win->user     = ui;
    win->on_paint = log_paint;
    win->on_event = log_event;
    win->on_close = log_close;
    win->min_w    = 520;
    win->min_h    = 240;

    clamp_scroll(win, ui);
    gui_focus_window(win);
}
