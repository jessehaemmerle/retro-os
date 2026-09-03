/* calendarapp.c - der Kalender.
 *
 * Das Rechnen steht schon da: Wochentag nach Zeller, Schaltjahre und
 * die Laenge der Monate liegen in shellutil.c, weil die Konsole sie
 * fuer "kalender" braucht. Hier kommt die Ansicht dazu - und die
 * Verbindung zu den Aufgaben.
 *
 * Denn ein Kalender, der nur Zahlen zeigt, ist ein Poster. Die
 * Aufgabenliste kennt Termine; also traegt der Kalender einen Punkt
 * unter jeden Tag, an dem etwas ansteht, und zeigt rechts, was es
 * ist. Beide lesen dieselbe Datei - der Kalender allerdings nur.
 * Schreiben bleibt bei den Aufgaben, sonst haetten zwei Fenster
 * dieselbe Datei in der Hand.
 */

#include "apps.h"
#include "font.h"
#include "kstring.h"
#include "lang.h"
#include "mm.h"
#include "rtc.h"
#include "shellutil.h"
#include "tasks.h"
#include "theme.h"
#include "user.h"
#include "vfs.h"
#include "widgets.h"

#define HEADER_H   44
#define STATUS_H   26
#define PANEL_W    220
#define CELL_MIN_W 34

struct cal_state {
    uint16_t year;
    uint8_t  month;
    uint8_t  selected;          /* Tag, 0 = keiner */

    uint16_t today_year;
    uint8_t  today_month, today_day;

    struct tasklist list;
    bool     have_tasks;
    char     status[80];
    int      hover;             /* -1 = keiner, sonst Knopfnummer */
};

/* ------------------------------------------------------------------ */

static void read_tasks(struct cal_state *st)
{
    char path[FS_PATH_MAX];

    user_home_file("Aufgaben.txt", "/Dokumente/Aufgaben.txt", path,
                   sizeof(path));
    tasks_clear(&st->list);
    st->have_tasks = false;

    struct fs_node *file = fs_lookup(NULL, path);

    if (!file || file->type != FS_FILE || !fs_load(file) || !file->data)
        return;

    char *text = kmalloc(file->size + 1);

    if (!text)
        return;

    memcpy(text, file->data, file->size);
    text[file->size] = '\0';
    tasks_from_text(&st->list, text);
    kfree(text);
    st->have_tasks = true;
}

/* Wie viele Aufgaben an diesem Tag anstehen. */
static size_t tasks_on(const struct cal_state *st, uint8_t day)
{
    size_t n = 0;

    for (size_t i = 0; i < TASK_MAX; i++) {
        const struct task *t = &st->list.items[i];

        if (t->used && t->year == st->year && t->month == st->month &&
            t->day == day)
            n++;
    }
    return n;
}

static void step_month(struct cal_state *st, int delta)
{
    int month = st->month + delta;

    while (month < 1) {
        month += 12;
        st->year--;
    }
    while (month > 12) {
        month -= 12;
        st->year++;
    }
    st->month = (uint8_t)month;

    /* Ein Tag, den es im neuen Monat nicht gibt, waere eine leere
     * Auswahl - dann lieber gar keine. */
    if (st->selected > sh_days_in_month(st->year, st->month))
        st->selected = 0;
}

static void go_today(struct cal_state *st)
{
    struct datetime now;

    rtc_read(&now);
    st->today_year = now.year;
    st->today_month = now.month;
    st->today_day = now.day;
    st->year = now.year;
    st->month = now.month;
    st->selected = now.day;
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static struct rect grid_rect(struct window *win)
{
    return rect_make(8, HEADER_H,
                     gui_client_width(win) - PANEL_W - 16,
                     gui_client_height(win) - HEADER_H - STATUS_H - 8);
}

/* Der Kasten eines Tages. Spalte 0 ist Montag. */
static struct rect day_rect(struct window *win, int column, int row)
{
    struct rect g = grid_rect(win);
    int32_t cw = g.w / 7;
    int32_t ch = (g.h - 20) / 6;

    return rect_make(g.x + column * cw, g.y + 20 + row * ch, cw - 2, ch - 2);
}

static struct rect button_rect(struct window *win, int index)
{
    static const int32_t widths[] = { 28, 28, 90, 28, 28 };
    int32_t x = 8;

    for (int i = 0; i < index; i++)
        x += widths[i] + 4;
    return rect_make(x, 8, widths[index], 26);
}

static void cal_paint(struct window *win, struct canvas *c)
{
    struct cal_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);
    struct rect g = grid_rect(win);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);
    widget_toolbar(&local, rect_make(0, 0, local.w, HEADER_H));

    static const char *const labels[] = { "<<", "<", "Heute", ">", ">>" };

    for (int i = 0; i < 5; i++)
        widget_button(&local, button_rect(win, i), tr(labels[i]),
                      st->hover == i, true);

    char title[40];

    ksnprintf(title, sizeof(title), "%s %u", tr(sh_month_name(st->month)),
              (unsigned)st->year);
    gfx_text_bold(&local, button_rect(win, 4).x + 44, 14, title, COL_TEXT);

    /* Kopfzeile mit den Wochentagen. */
    static const char *const days[] = { "Mo", "Di", "Mi", "Do", "Fr",
                                        "Sa", "So" };
    int32_t cw = g.w / 7;

    for (int i = 0; i < 7; i++) {
        int32_t x = g.x + i * cw + (cw - gfx_text_width(tr(days[i]))) / 2;

        gfx_text(&local, x, g.y, tr(days[i]),
                 i >= 5 ? COL_ACCENT : COL_TEXT_DIM);
    }

    int first = sh_weekday(st->year, st->month, 1);
    uint8_t count = sh_days_in_month(st->year, st->month);

    for (uint8_t day = 1; day <= count; day++) {
        int index = first + day - 1;
        struct rect r = day_rect(win, index % 7, index / 7);
        bool is_today = st->year == st->today_year &&
                        st->month == st->today_month && day == st->today_day;
        bool is_selected = day == st->selected;

        if (is_selected) {
            gfx_fill(&local, r, COL_SELECT);
        } else {
            gfx_fill(&local, r, COL_FIELD);
            gfx_bevel_thin(&local, r, false);
        }

        /* Heute bekommt einen Rahmen, auch wenn es nicht gewaehlt
         * ist - sonst sucht man im eigenen Monat nach dem Datum. */
        if (is_today)
            gfx_frame(&local, r, COL_ACCENT);

        char text[4];

        ksnprintf(text, sizeof(text), "%u", (unsigned)day);
        gfx_text(&local, r.x + 5, r.y + 4, text,
                 is_selected ? COL_SELECT_TEXT
                             : (index % 7 >= 5 ? COL_ACCENT : COL_TEXT));

        size_t n = tasks_on(st, day);

        /* Ein Punkt je Aufgabe, hoechstens vier - mehr waeren nur
         * noch ein Strich. */
        for (size_t i = 0; i < n && i < 4; i++)
            gfx_fill(&local, rect_make(r.x + 5 + (int32_t)i * 6,
                                       r.y + r.h - 8, 4, 4),
                     is_selected ? COL_SELECT_TEXT : COL_ACCENT);
    }

    /* Rechts die Aufgaben des gewaehlten Tages. */
    struct rect panel = rect_make(local.w - PANEL_W + 4, HEADER_H,
                                  PANEL_W - 12, g.h);

    gfx_fill(&local, panel, COL_FIELD);
    gfx_bevel_thin(&local, panel, false);

    if (st->selected) {
        char head[40];

        ksnprintf(head, sizeof(head), "%u. %s", (unsigned)st->selected,
                  tr(sh_month_name(st->month)));
        gfx_text_bold(&local, panel.x + 8, panel.y + 8, head, COL_TEXT);

        int32_t y = panel.y + 30;
        size_t shown = 0;

        for (size_t i = 0; i < TASK_MAX && y < panel.y + panel.h - 18; i++) {
            const struct task *t = &st->list.items[i];

            if (!t->used || t->year != st->year || t->month != st->month ||
                t->day != st->selected)
                continue;

            icon_draw(&local, panel.x + 8, y, t->done ? ICON_DONE : ICON_FLAG,
                      1);
            gfx_text_clipped(&local, panel.x + 28, y + 1, t->text,
                             t->done ? COL_TEXT_DIM : COL_TEXT,
                             panel.w - 36);
            y += 20;
            shown++;
        }

        if (!shown)
            gfx_text(&local, panel.x + 8, panel.y + 30,
                     tr("Nichts an diesem Tag."), COL_TEXT_DIM);
    }

    widget_statusbar(&local,
                     rect_make(0, gui_client_height(win) - STATUS_H,
                               gui_client_width(win), STATUS_H),
                     st->status, "");
}

static void update_status(struct cal_state *st)
{
    size_t open = tasks_count(&st->list, true);

    if (!st->have_tasks)
        strlcpy(st->status, tr("Keine Aufgabenliste gefunden."),
                sizeof(st->status));
    else if (open == 1)
        strlcpy(st->status, tr("Eine offene Aufgabe"), sizeof(st->status));
    else
        ksnprintf(st->status, sizeof(st->status), tr("%u offene Aufgaben"),
                  (unsigned)open);
}

static void cal_event(struct window *win, const struct gui_event *ev)
{
    struct cal_state *st = win->user;

    if (ev->type == EV_MOUSE_MOVE) {
        int before = st->hover;

        st->hover = -1;
        for (int i = 0; i < 5; i++)
            if (rect_contains(button_rect(win, i), ev->x, ev->y))
                st->hover = i;
        if (before != st->hover)
            gui_invalidate();
        return;
    }

    if (ev->type == EV_TICK) {
        /* Die Aufgabenliste kann sich nebenan geaendert haben. */
        read_tasks(st);
        update_status(st);
        return;
    }

    if (ev->type == EV_MOUSE_DOWN) {
        for (int i = 0; i < 5; i++) {
            if (!rect_contains(button_rect(win, i), ev->x, ev->y))
                continue;
            switch (i) {
            case 0: step_month(st, -12); break;
            case 1: step_month(st, -1);  break;
            case 2: go_today(st);        break;
            case 3: step_month(st, +1);  break;
            default: step_month(st, +12); break;
            }
            gui_invalidate();
            return;
        }

        int first = sh_weekday(st->year, st->month, 1);
        uint8_t count = sh_days_in_month(st->year, st->month);

        for (uint8_t day = 1; day <= count; day++) {
            int index = first + day - 1;

            if (rect_contains(day_rect(win, index % 7, index / 7),
                              ev->x, ev->y)) {
                st->selected = day;
                gui_invalidate();
                return;
            }
        }
        return;
    }

    if (ev->type != EV_KEY_DOWN)
        return;

    switch (ev->key) {
    case KEY_LEFT:     step_month(st, -1);  break;
    case KEY_RIGHT:    step_month(st, +1);  break;
    case KEY_PAGEUP:   step_month(st, -12); break;
    case KEY_PAGEDOWN: step_month(st, +12); break;
    case KEY_HOME:     go_today(st);        break;
    default:           return;
    }
    gui_invalidate();
}

static void cal_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_calendar(void)
{
    struct window *existing = gui_find_by_paint(cal_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct cal_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    st->hover = -1;
    go_today(st);
    read_tasks(st);
    update_status(st);

    struct window *win = gui_create_window(tr("Kalender"), 130, 90, 700, 400,
                                           WF_RESIZABLE, ICON_CALENDAR);
    if (!win) {
        kfree(st);
        return;
    }

    win->user     = st;
    win->min_w    = 560;
    win->min_h    = 320;
    win->on_paint = cal_paint;
    win->on_event = cal_event;
    win->on_close = cal_close;

    gui_focus_window(win);
}
