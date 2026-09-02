/* tasksapp.c - das Aufgabenfenster.
 *
 * Oben eine Zeile zum Eintippen, darunter die Liste, rechts die
 * Knoepfe. Neue Aufgaben entstehen dort, wo man sie hinschreibt, und
 * nicht in einem Dialog: Wer eine Aufgabe notieren will, will tippen
 * und weiterarbeiten.
 *
 * Die Liste gehoert dem angemeldeten Benutzer und liegt in seinem
 * Heimatverzeichnis. Wechselt jemand den Benutzer, ist sie eine andere -
 * das ist der ganze Sinn der Sache.
 */

#include "apps.h"

#include "font.h"
#include "kstring.h"
#include "log.h"
#include "mm.h"
#include "rtc.h"
#include "tasks.h"
#include "theme.h"
#include "user.h"
#include "vfs.h"
#include "widgets.h"

#define ENTRY_H   34
#define ROW_H     22
#define STATUS_H  22
#define SIDE_W    166
#define CHECK_W   22

enum button_id {
    B_ADD,
    B_TOGGLE,
    B_PRIO,
    B_DUE,
    B_DELETE,
    B_PURGE,
    B_FILTER,
    B_SAVE,
    B_COUNT
};

struct tasks_ui {
    struct tasklist list;
    char     input[TASK_TEXT_MAX + 1];
    size_t   cursor;
    bool     input_focus;
    bool     hide_done;
    bool     changed;
    int      hover;
    int32_t  scroll;
    uint32_t selected;        /* id, 0 = nichts */
    char     path[FS_PATH_MAX];
    char     status[96];

    /* Die sortierte Sicht der letzten Zeichnung - der Klick trifft
     * dieselbe Reihenfolge, die zu sehen war. */
    struct task *view[TASK_MAX];
    size_t       view_count;
};

static struct window *the_window;

static struct tasks_ui *ui_of(void)
{
    return the_window && gui_window_alive(the_window) ? the_window->user : NULL;
}

/* ------------------------------------------------------------------ */
/* Masse                                                               */
/* ------------------------------------------------------------------ */

static struct rect input_rect(struct window *win)
{
    return rect_make(8, 6, gui_client_width(win) - SIDE_W - 20, 24);
}

static struct rect list_rect(struct window *win)
{
    return rect_make(8, ENTRY_H + 4, gui_client_width(win) - SIDE_W - 20,
                     gui_client_height(win) - ENTRY_H - STATUS_H - 12);
}

static struct rect button_rect(struct window *win, int id)
{
    return rect_make(gui_client_width(win) - SIDE_W + 4, 6 + id * 32,
                     SIDE_W - 12, 28);
}

static const char *button_label(struct tasks_ui *ui, int id)
{
    switch (id) {
    case B_ADD:    return "Hinzufuegen";
    case B_TOGGLE: return "Haken setzen";
    case B_PRIO:   return "Wichtigkeit";
    case B_DUE:    return "Termin";
    case B_DELETE: return "Loeschen";
    case B_PURGE:  return "Erledigte weg";
    case B_FILTER: return ui->hide_done ? "Alle zeigen" : "Nur offene";
    default:       return "Speichern";
    }
}

static int32_t rows_fitting(struct window *win)
{
    return MAX(list_rect(win).h / ROW_H, 1);
}

/* ------------------------------------------------------------------ */
/* Laden und Speichern                                                 */
/* ------------------------------------------------------------------ */

static void pick_path(struct tasks_ui *ui)
{
    user_home_file("Aufgaben.txt", "/Dokumente/Aufgaben.txt", ui->path,
                   sizeof(ui->path));
}

static void load(struct tasks_ui *ui)
{
    tasks_clear(&ui->list);

    struct fs_node *file = fs_lookup(NULL, ui->path);

    if (!file || file->type != FS_FILE || !fs_load(file) || !file->data)
        return;

    char *text = kmalloc(file->size + 1);

    if (!text)
        return;

    memcpy(text, file->data, file->size);
    text[file->size] = '\0';
    tasks_from_text(&ui->list, text);
    kfree(text);
}

static void save(struct tasks_ui *ui)
{
    size_t cap = TASK_MAX * (TASK_TEXT_MAX + 48) + 256;
    char  *text = kmalloc(cap);

    if (!text) {
        strlcpy(ui->status, "Kein Speicher.", sizeof(ui->status));
        return;
    }

    size_t used = tasks_to_text(&ui->list, text, cap);
    struct fs_node *file = fs_lookup(NULL, ui->path);

    if (!file)
        file = fs_create_path(NULL, ui->path, FS_FILE);

    if (file && file->type == FS_FILE && fs_write(file, text, used)) {
        ui->changed = false;
        ksnprintf(ui->status, sizeof(ui->status), "Gesichert in %s", ui->path);
        log_info("aufgaben", "%u Aufgaben in %s gesichert",
                 (unsigned)tasks_count(&ui->list, false), ui->path);
    } else {
        ksnprintf(ui->status, sizeof(ui->status),
                  "%s liess sich nicht schreiben.", ui->path);
    }
    kfree(text);
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static void paint_row(struct canvas *c, struct rect r, struct task *t,
                      bool selected, bool overdue)
{
    if (selected)
        gfx_fill(c, r, COL_SELECT);

    uint32_t fg = selected ? COL_SELECT_TEXT
                           : (t->done ? COL_TEXT_DIM : COL_TEXT);

    /* Der Haken ist ein Kaestchen, kein Symbol: Man soll ihn anklicken
     * koennen, ohne die Zeile erst auszuwaehlen. */
    struct rect box = rect_make(r.x + 4, r.y + 4, 14, 14);

    gfx_fill(c, box, COL_FIELD);
    gfx_bevel_thin(c, box, false);
    if (t->done) {
        gfx_line(c, box.x + 3, box.y + 7, box.x + 6, box.y + 10, COL_TEXT);
        gfx_line(c, box.x + 6, box.y + 10, box.x + 11, box.y + 3, COL_TEXT);
    }

    int32_t x = r.x + CHECK_W + 8;

    if (t->prio == TP_HIGH)
        icon_draw(c, x, r.y + 3, ICON_FLAG, 1);
    else if (t->prio == TP_LOW)
        gfx_text(c, x + 4, r.y + 3, "\xB7", fg);
    x += 22;

    char date[16];

    tasks_format_date(t, date, sizeof(date));
    gfx_text(c, x, r.y + 3, date,
             overdue && !selected ? RGB(0xC0, 0x28, 0x28) : COL_TEXT_DIM);
    x += 92;

    gfx_text_clipped(c, x, r.y + 3, t->text, fg, r.x + r.w - x - 6);

    /* Erledigtes wird durchgestrichen - das liest sich schneller als
     * eine blasse Farbe allein. */
    if (t->done) {
        int32_t width = MIN(gfx_text_width(t->text), r.x + r.w - x - 6);

        gfx_hline(c, x, r.y + 3 + FONT_HEIGHT / 2, width, fg);
    }
}

static void tasks_paint(struct window *win, struct canvas *c)
{
    struct tasks_ui *ui = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    widget_field(&local, input_rect(win), ui->input,
                 ui->input_focus ? (int32_t)ui->cursor : -1, ui->input_focus);
    if (!ui->input[0] && !ui->input_focus)
        gfx_text(&local, input_rect(win).x + 6, input_rect(win).y + 5,
                 "Neue Aufgabe eintippen und Eingabe druecken",
                 COL_TEXT_DIM);

    struct rect l = list_rect(win);

    gfx_fill(&local, l, COL_FIELD);
    gfx_bevel_thin(&local, l, false);
    gfx_set_clip(&local, l);

    ui->view_count = tasks_sorted(&ui->list, ui->view, ARRAY_LEN(ui->view),
                                  ui->hide_done);

    struct datetime dt;

    rtc_read(&dt);

    int32_t rows = rows_fitting(win);

    for (int32_t i = 0; i < rows; i++) {
        size_t index = (size_t)(ui->scroll + i);

        if (index >= ui->view_count)
            break;

        struct task *t = ui->view[index];

        paint_row(&local, rect_make(l.x + 1, l.y + 1 + i * ROW_H, l.w - 2, ROW_H),
                  t, t->id == ui->selected,
                  tasks_overdue(t, dt.year, dt.month, dt.day));
    }
    gfx_reset_clip(&local);

    if ((int32_t)ui->view_count > rows)
        widget_vscroll(&local,
                       rect_make(l.x + l.w - SCROLLBAR_WIDTH, l.y,
                                 SCROLLBAR_WIDTH, l.h),
                       ui->scroll, (int32_t)ui->view_count, rows);

    for (int i = 0; i < B_COUNT; i++) {
        bool has = ui->selected && tasks_by_id(&ui->list, ui->selected);
        bool on = i == B_ADD || i == B_FILTER || i == B_SAVE || i == B_PURGE
                  ? true : has;

        widget_button(&local, button_rect(win, i), button_label(ui, i),
                      ui->hover == i, on);
    }

    char left[96];
    size_t open = tasks_count(&ui->list, true);
    size_t all  = tasks_count(&ui->list, false);

    if (ui->status[0])
        strlcpy(left, ui->status, sizeof(left));
    else
        ksnprintf(left, sizeof(left), "%u offen von %u", (unsigned)open,
                  (unsigned)all);

    widget_statusbar(&local,
                     rect_make(0, local.h - STATUS_H, local.w, STATUS_H),
                     left, ui->changed ? "ungespeichert" : ui->path);
}

/* ------------------------------------------------------------------ */
/* Handeln                                                             */
/* ------------------------------------------------------------------ */

static void clamp(struct window *win, struct tasks_ui *ui)
{
    int32_t max = (int32_t)ui->view_count - rows_fitting(win);

    if (max < 0)
        max = 0;
    ui->scroll = CLAMP(ui->scroll, 0, max);
}

static void add_input(struct tasks_ui *ui)
{
    struct task *t = tasks_add(&ui->list, ui->input);

    if (!t) {
        strlcpy(ui->status, ui->input[0] ? "Die Liste ist voll."
                                         : "Da steht noch nichts.",
                sizeof(ui->status));
        return;
    }

    ui->selected = t->id;
    ui->changed  = true;
    ui->input[0] = '\0';
    ui->cursor   = 0;
    ui->status[0] = '\0';
}

static void prio_entered(const char *text, void *user)
{
    UNUSED(user);

    struct tasks_ui *ui = ui_of();

    if (!ui)
        return;

    struct task *t = tasks_by_id(&ui->list, ui->selected);
    uint8_t prio;

    if (!t)
        return;
    if (!task_prio_parse(text, &prio)) {
        strlcpy(ui->status, "Erwartet wird hoch, mittel oder niedrig.",
                sizeof(ui->status));
        gui_invalidate();
        return;
    }

    t->prio = prio;
    ui->changed = true;
    gui_invalidate();
}

static void due_entered(const char *text, void *user)
{
    UNUSED(user);

    struct tasks_ui *ui = ui_of();

    if (!ui)
        return;

    struct task *t = tasks_by_id(&ui->list, ui->selected);
    uint16_t year;
    uint8_t  month, day;

    if (!t)
        return;
    if (!tasks_parse_date(text, &year, &month, &day)) {
        strlcpy(ui->status, "Erwartet wird 15.09.2026 oder ein Strich.",
                sizeof(ui->status));
        gui_invalidate();
        return;
    }

    t->year = year;
    t->month = month;
    t->day = day;
    ui->changed = true;
    gui_invalidate();
}

static void press(struct window *win, struct tasks_ui *ui, int id)
{
    struct task *t = tasks_by_id(&ui->list, ui->selected);
    char prompt[96];

    ui->status[0] = '\0';

    switch (id) {
    case B_ADD:
        add_input(ui);
        break;
    case B_TOGGLE:
        if (t) {
            t->done = !t->done;
            ui->changed = true;
        }
        break;
    case B_PRIO:
        if (t) {
            ksnprintf(prompt, sizeof(prompt),
                      "Wichtigkeit von \"%s\" (hoch, mittel, niedrig):",
                      t->text);
            dialog_input("Wichtigkeit", prompt, task_prio_name(t->prio),
                         prio_entered, NULL);
        }
        break;
    case B_DUE:
        if (t) {
            char date[16];

            tasks_format_date(t, date, sizeof(date));
            ksnprintf(prompt, sizeof(prompt),
                      "Termin fuer \"%s\" (TT.MM.JJJJ, Strich loescht ihn):",
                      t->text);
            dialog_input("Termin", prompt, date, due_entered, NULL);
        }
        break;
    case B_DELETE:
        if (t && tasks_remove(&ui->list, ui->selected)) {
            ui->selected = 0;
            ui->changed = true;
        }
        break;
    case B_PURGE: {
        size_t n = tasks_purge_done(&ui->list);

        if (n) {
            ui->selected = 0;
            ui->changed = true;
            ksnprintf(ui->status, sizeof(ui->status),
                      "%u erledigte Aufgaben weggeraeumt.", (unsigned)n);
        } else {
            strlcpy(ui->status, "Es ist nichts erledigt.", sizeof(ui->status));
        }
        break;
    }
    case B_FILTER:
        ui->hide_done = !ui->hide_done;
        ui->scroll = 0;
        break;
    default:
        save(ui);
        break;
    }

    clamp(win, ui);
    gui_invalidate();
}

static void type_input(struct tasks_ui *ui, const struct gui_event *ev)
{
    size_t len = strlen(ui->input);

    switch (ev->key) {
    case KEY_LEFT:  if (ui->cursor) ui->cursor--; return;
    case KEY_RIGHT: if (ui->cursor < len) ui->cursor++; return;
    case KEY_HOME:  ui->cursor = 0; return;
    case KEY_END:   ui->cursor = len; return;
    case KEY_BACKSPACE:
        if (ui->cursor) {
            memmove(&ui->input[ui->cursor - 1], &ui->input[ui->cursor],
                    len - ui->cursor + 1);
            ui->cursor--;
        }
        return;
    case KEY_DELETE:
        if (ui->cursor < len)
            memmove(&ui->input[ui->cursor], &ui->input[ui->cursor + 1],
                    len - ui->cursor);
        return;
    default:
        break;
    }

    if ((unsigned char)ev->ascii >= 32 && len + 1 < sizeof(ui->input)) {
        memmove(&ui->input[ui->cursor + 1], &ui->input[ui->cursor],
                len - ui->cursor + 1);
        ui->input[ui->cursor++] = ev->ascii;
    }
}

static void select_offset(struct tasks_ui *ui, int delta)
{
    if (!ui->view_count)
        return;

    size_t index = 0;

    for (size_t i = 0; i < ui->view_count; i++)
        if (ui->view[i]->id == ui->selected)
            index = i;

    int32_t next = (int32_t)index + delta;

    next = CLAMP(next, 0, (int32_t)ui->view_count - 1);
    ui->selected = ui->view[next]->id;
}

static void tasks_event(struct window *win, const struct gui_event *ev)
{
    struct tasks_ui *ui = win->user;

    if (ev->type == EV_SCROLL) {
        ui->scroll -= ev->scroll * 3;
        clamp(win, ui);
        gui_invalidate();
        return;
    }

    if (ev->type == EV_KEY_DOWN) {
        if (ev->key == KEY_ENTER) {
            if (ui->input[0])
                add_input(ui);
            else
                press(win, ui, B_TOGGLE);
            gui_invalidate();
            return;
        }
        if (ev->key == KEY_TAB) {
            ui->input_focus = !ui->input_focus;
            gui_invalidate();
            return;
        }
        if (!ui->input_focus &&
            (ev->key == KEY_UP || ev->key == KEY_DOWN)) {
            select_offset(ui, ev->key == KEY_UP ? -1 : 1);
            gui_invalidate();
            return;
        }
        if (!ui->input_focus && ev->key == KEY_DELETE) {
            press(win, ui, B_DELETE);
            return;
        }

        ui->input_focus = true;
        type_input(ui, ev);
        gui_invalidate();
        return;
    }

    if (ev->type == EV_MOUSE_MOVE) {
        int before = ui->hover;

        ui->hover = -1;
        for (int i = 0; i < B_COUNT; i++)
            if (rect_contains(button_rect(win, i), ev->x, ev->y))
                ui->hover = i;
        if (before != ui->hover)
            gui_invalidate();
        return;
    }

    if (ev->type != EV_MOUSE_DOWN || ev->button != MB_LEFT)
        return;

    ui->input_focus = rect_contains(input_rect(win), ev->x, ev->y);
    if (ui->input_focus)
        ui->cursor = strlen(ui->input);

    for (int i = 0; i < B_COUNT; i++) {
        if (rect_contains(button_rect(win, i), ev->x, ev->y)) {
            press(win, ui, i);
            return;
        }
    }

    struct rect l = list_rect(win);

    if (rect_contains(l, ev->x, ev->y)) {
        size_t index = (size_t)((ev->y - l.y - 1) / ROW_H + ui->scroll);

        if (index < ui->view_count) {
            struct task *t = ui->view[index];

            ui->selected = t->id;
            ui->status[0] = '\0';

            /* Ein Klick auf das Kaestchen setzt gleich den Haken. */
            if (ev->x < l.x + CHECK_W + 4) {
                t->done = !t->done;
                ui->changed = true;
            }
        }
    }
    gui_invalidate();
}

static void tasks_close(struct window *win)
{
    struct tasks_ui *ui = win->user;

    /* Ungespeichertes beim Zumachen sichern: Eine Aufgabenliste, die
     * beim Schliessen verlorengeht, benutzt man kein zweites Mal. */
    if (ui && ui->changed)
        save(ui);

    kfree(win->user);
    win->user = NULL;
    if (win == the_window)
        the_window = NULL;
}

void app_tasks(void)
{
    struct window *existing = gui_find_by_paint(tasks_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct tasks_ui *ui = kzalloc(sizeof(*ui));

    if (!ui)
        return;

    ui->hover = -1;
    ui->input_focus = true;
    tasks_clear(&ui->list);
    pick_path(ui);
    load(ui);

    struct window *win = gui_create_window("Aufgaben", 0, 0, 660, 400,
                                           WF_CENTER | WF_RESIZABLE,
                                           ICON_TASKS);
    if (!win) {
        kfree(ui);
        return;
    }

    win->user     = ui;
    win->on_paint = tasks_paint;
    win->on_event = tasks_event;
    win->on_close = tasks_close;
    win->min_w    = 520;
    win->min_h    = 260;

    the_window = win;
    gui_focus_window(win);
}
