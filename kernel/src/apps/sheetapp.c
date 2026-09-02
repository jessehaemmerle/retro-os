/* sheetapp.c - das Fenster der Tabellenkalkulation.
 *
 * Gerechnet wird in sheet.c; hier wird gezeigt und bedient. Das Gitter
 * scrollt unter feststehenden Kopfzeilen hindurch, und die Zelle unter
 * dem Rahmen ist die, auf die sich alles bezieht: die Bearbeitungszeile
 * oben, die Werkzeuge, die Tastatur.
 *
 * Bearbeitet wird an Ort und Stelle. Wer zu tippen anfaengt, ersetzt
 * den Inhalt; F2 haengt an den vorhandenen an. Eingabe geht eine Zeile
 * hinunter, Tabulator eine Spalte weiter - so, wie man eine Tabelle
 * fuellt, ohne die Maus anzufassen.
 */

#include "apps.h"
#include "clipboard.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"
#include "sheet.h"
#include "theme.h"
#include "widgets.h"

#define TK_TOOLBAR_H 34
#define TK_EDIT_H    26
#define TK_STATUS_H  24
#define TK_HEAD_H    18
#define TK_HEAD_W    42
#define TK_ROW_H     18
#define TK_COL_W     88

#define TK_GRID      RGB(0xC0, 0xC6, 0xCC)
#define TK_HEAD_BG   RGB(0xDA, 0xDE, 0xE2)
#define TK_HEAD_ON   RGB(0xA8, 0xC4, 0xE0)
#define TK_CURSOR    RGB(0x18, 0x60, 0x30)
#define TK_ERROR     RGB(0xB0, 0x20, 0x20)
#define TK_FORMULA   RGB(0x20, 0x50, 0x90)

enum tk_button { TK_OPEN, TK_SAVE, TK_SUM, TK_BOLD, TK_CLEAR, TK_COUNT };

struct tk_state {
    struct sheet   *sh;
    struct fs_node *file;
    bool            modified;

    int row, col;               /* Zelle unter dem Rahmen */
    int scroll_row, scroll_col;

    bool editing;
    char edit[SHEET_TEXT_MAX];
    int  edit_len;
    bool caret_on;

    int  pressed;
};

/* ------------------------------------------------------------------ */
/* Aufteilung                                                          */
/* ------------------------------------------------------------------ */

static struct rect edit_rect(struct window *win)
{
    return rect_make(0, TK_TOOLBAR_H, gui_client_width(win), TK_EDIT_H);
}

static struct rect grid_rect(struct window *win)
{
    int32_t top = TK_TOOLBAR_H + TK_EDIT_H;

    return rect_make(0, top, gui_client_width(win) - SCROLLBAR_WIDTH,
                     gui_client_height(win) - top - TK_STATUS_H);
}

static int visible_rows(struct window *win)
{
    return MAX((grid_rect(win).h - TK_HEAD_H) / TK_ROW_H, 1);
}

static int visible_cols(struct window *win)
{
    return MAX((grid_rect(win).w - TK_HEAD_W) / TK_COL_W, 1);
}

static void ensure_visible(struct window *win)
{
    struct tk_state *st = win->user;
    int rows = visible_rows(win);
    int cols = visible_cols(win);

    if (st->row < st->scroll_row)
        st->scroll_row = st->row;
    else if (st->row >= st->scroll_row + rows)
        st->scroll_row = st->row - rows + 1;

    if (st->col < st->scroll_col)
        st->scroll_col = st->col;
    else if (st->col >= st->scroll_col + cols)
        st->scroll_col = st->col - cols + 1;

    st->scroll_row = CLAMP(st->scroll_row, 0, MAX(SHEET_ROWS - rows, 0));
    st->scroll_col = CLAMP(st->scroll_col, 0, MAX(SHEET_COLS - cols, 0));
}

/* ------------------------------------------------------------------ */
/* Bearbeiten                                                          */
/* ------------------------------------------------------------------ */

static void update_title(struct window *win)
{
    struct tk_state *st = win->user;
    char title[WIN_TITLE_MAX + 1];

    ksnprintf(title, sizeof(title), "Tabelle - %s%s",
              st->file ? st->file->name : "Unbenannt",
              st->modified ? " *" : "");
    gui_set_title(win, title);
}

static void begin_edit(struct window *win, bool keep)
{
    struct tk_state *st = win->user;
    const struct cell *cell = sheet_cell(st->sh, st->row, st->col);

    st->editing = true;
    st->caret_on = true;

    if (keep && cell)
        strlcpy(st->edit, cell->text, sizeof(st->edit));
    else
        st->edit[0] = '\0';

    st->edit_len = (int)strlen(st->edit);
}

static void commit_edit(struct window *win)
{
    struct tk_state *st = win->user;

    if (!st->editing)
        return;

    st->editing = false;
    sheet_set(st->sh, st->row, st->col, st->edit);
    sheet_recalc(st->sh);
    st->modified = true;
    update_title(win);
}

static void cancel_edit(struct tk_state *st)
{
    st->editing = false;
    st->edit[0] = '\0';
    st->edit_len = 0;
}

static void move_to(struct window *win, int row, int col)
{
    struct tk_state *st = win->user;

    commit_edit(win);
    st->row = CLAMP(row, 0, SHEET_ROWS - 1);
    st->col = CLAMP(col, 0, SHEET_COLS - 1);
    ensure_visible(win);
    gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Werkzeuge                                                           */
/* ------------------------------------------------------------------ */

/* Traegt eine Summe ueber die Zahlen unmittelbar darueber ein - der
 * Handgriff, den man in einer Tabelle am haeufigsten braucht. */
static void insert_sum(struct window *win)
{
    struct tk_state *st = win->user;

    if (st->row == 0)
        return;

    int first = st->row - 1;

    while (first > 0 && sheet_is_numeric(st->sh, first - 1, st->col))
        first--;

    if (!sheet_is_numeric(st->sh, st->row - 1, st->col))
        return;

    char from[8], to[8], formula[SHEET_TEXT_MAX];

    sheet_ref_name(first, st->col, from, sizeof(from));
    sheet_ref_name(st->row - 1, st->col, to, sizeof(to));
    ksnprintf(formula, sizeof(formula), "=SUMME(%s:%s)", from, to);

    sheet_set(st->sh, st->row, st->col, formula);
    sheet_recalc(st->sh);
    st->modified = true;
    update_title(win);
    gui_invalidate();
}

static struct fs_node *sheet_dir(void)
{
    struct fs_node *base = fs_disk_root() ? fs_disk_root() : fs_root();
    struct fs_node *dir = fs_find_child(base, "Dokumente");

    return (dir && dir->type == FS_DIR) ? dir : base;
}

static void on_save_as(const char *name, void *user)
{
    struct window *win = user;

    if (!gui_window_alive(win) || !name || !name[0])
        return;

    struct tk_state *st = win->user;
    struct fs_node *dir = sheet_dir();
    struct fs_node *file = fs_find_child(dir, name);

    if (!file)
        file = fs_create(dir, name, FS_FILE);
    if (!file || file->type != FS_FILE || file->readonly) {
        dialog_message("Speichern", "Unter diesem Namen geht es nicht.");
        return;
    }

    size_t room = 64 * 1024;
    char *csv = kmalloc(room);

    if (!csv) {
        dialog_message("Speichern", "Zu wenig Speicher.");
        return;
    }

    size_t n = sheet_to_csv(st->sh, csv, room);
    bool ok = fs_write(file, csv, MIN(n, room - 1));

    kfree(csv);

    if (!ok) {
        dialog_message("Speichern", "Die Datei liess sich nicht schreiben.");
        return;
    }

    st->file = file;
    st->modified = false;
    update_title(win);
    gui_invalidate();
}

static void do_save(struct window *win)
{
    struct tk_state *st = win->user;

    commit_edit(win);

    if (st->file && !fs_node_alive(st->file))
        st->file = NULL;

    if (!st->file || st->file->readonly) {
        dialog_input("Speichern unter", "Dateiname:",
                     st->file ? st->file->name : "tabelle.csv",
                     on_save_as, win);
        return;
    }
    on_save_as(st->file->name, win);
}

static void on_open(const char *path, void *user)
{
    struct window *win = user;

    if (!gui_window_alive(win) || !path || !path[0])
        return;

    struct fs_node *file = fs_lookup(fs_root(), path);

    if (!file)
        file = fs_find_child(sheet_dir(), path);

    if (!file || file->type != FS_FILE || !fs_load(file)) {
        dialog_message("Oeffnen", "Diese Datei gibt es nicht.");
        return;
    }

    struct tk_state *st = win->user;

    sheet_from_csv(st->sh, (const char *)file->data, file->size);
    st->file = file;
    st->modified = false;
    st->row = st->col = 0;
    st->scroll_row = st->scroll_col = 0;
    cancel_edit(st);
    update_title(win);
    gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static struct rect tool_rect(int index)
{
    static const int32_t widths[TK_COUNT] = { 92, 104, 86, 74, 90 };
    int32_t x = 4;

    for (int i = 0; i < index; i++)
        x += widths[i] + 3;

    return rect_make(x, 4, widths[index], TK_TOOLBAR_H - 8);
}

static void paint_toolbar(struct window *win, struct canvas *c)
{
    struct tk_state *st = win->user;
    const struct cell *cell = sheet_cell(st->sh, st->row, st->col);

    widget_toolbar(c, rect_make(0, 0, c->w, TK_TOOLBAR_H));
    widget_icon_button(c, tool_rect(TK_OPEN), ICON_FOLDER_OPEN, "Oeffnen",
                       st->pressed == TK_OPEN, true);
    widget_icon_button(c, tool_rect(TK_SAVE), ICON_SAVE, "Speichern",
                       st->pressed == TK_SAVE, true);
    widget_icon_button(c, tool_rect(TK_SUM), ICON_SUM, "Summe",
                       st->pressed == TK_SUM,
                       st->row > 0 && sheet_is_numeric(st->sh, st->row - 1,
                                                       st->col));
    widget_icon_button(c, tool_rect(TK_BOLD), ICON_BOLD, "Fett",
                       st->pressed == TK_BOLD || (cell && cell->bold), true);
    widget_icon_button(c, tool_rect(TK_CLEAR), ICON_TRASH, "Leeren",
                       st->pressed == TK_CLEAR,
                       cell && cell->kind != CELL_EMPTY);
}

static void paint_editbar(struct window *win, struct canvas *c)
{
    struct tk_state *st = win->user;
    struct rect area = edit_rect(win);
    char name[8];

    gfx_fill(c, area, COL_FACE);
    sheet_ref_name(st->row, st->col, name, sizeof(name));

    struct rect box = rect_make(4, area.y + 3, 56, TK_EDIT_H - 6);

    widget_field(c, box, name, -1, false);

    struct rect field = rect_make(box.x + box.w + 6, box.y,
                                  area.w - box.w - 16, box.h);
    const struct cell *cell = sheet_cell(st->sh, st->row, st->col);
    const char *text = st->editing ? st->edit : (cell ? cell->text : "");

    widget_field(c, field, text, st->editing ? st->edit_len : -1,
                 st->editing && st->caret_on);
}

static void paint_grid(struct window *win, struct canvas *c)
{
    struct tk_state *st = win->user;
    struct rect area = grid_rect(win);
    int rows = visible_rows(win);
    int cols = visible_cols(win);

    struct canvas g = *c;

    gfx_set_clip(&g, rect_intersect(c->clip, area));
    gfx_fill(&g, area, COL_FIELD);

    /* Kopfzeile und Kopfspalte. */
    gfx_fill(&g, rect_make(area.x, area.y, area.w, TK_HEAD_H), TK_HEAD_BG);
    gfx_fill(&g, rect_make(area.x, area.y, TK_HEAD_W, area.h), TK_HEAD_BG);

    for (int i = 0; i < cols; i++) {
        int col = st->scroll_col + i;

        if (col >= SHEET_COLS)
            break;

        struct rect head = rect_make(area.x + TK_HEAD_W + i * TK_COL_W,
                                     area.y, TK_COL_W, TK_HEAD_H);
        char label[4] = { (char)('A' + col), '\0' };

        if (col == st->col)
            gfx_fill(&g, head, TK_HEAD_ON);
        gfx_text(&g, head.x + (TK_COL_W - gfx_text_width(label)) / 2,
                 head.y + 2, label, COL_TEXT);
        gfx_vline(&g, head.x + head.w, area.y, area.h, TK_GRID);
    }

    for (int i = 0; i < rows; i++) {
        int row = st->scroll_row + i;

        if (row >= SHEET_ROWS)
            break;

        struct rect head = rect_make(area.x, area.y + TK_HEAD_H + i * TK_ROW_H,
                                     TK_HEAD_W, TK_ROW_H);
        char label[8];

        ksnprintf(label, sizeof(label), "%d", row + 1);
        if (row == st->row)
            gfx_fill(&g, head, TK_HEAD_ON);
        gfx_text(&g, head.x + TK_HEAD_W - 4 - gfx_text_width(label),
                 head.y + 2, label, COL_TEXT);
        gfx_hline(&g, area.x, head.y + head.h, area.w, TK_GRID);
    }

    gfx_vline(&g, area.x + TK_HEAD_W, area.y, area.h, TK_GRID);
    gfx_hline(&g, area.x, area.y + TK_HEAD_H, area.w, TK_GRID);

    /* Die Zellen selbst. */
    for (int i = 0; i < rows; i++) {
        int row = st->scroll_row + i;

        if (row >= SHEET_ROWS)
            break;

        for (int k = 0; k < cols; k++) {
            int col = st->scroll_col + k;

            if (col >= SHEET_COLS)
                break;

            struct rect box = rect_make(area.x + TK_HEAD_W + k * TK_COL_W,
                                        area.y + TK_HEAD_H + i * TK_ROW_H,
                                        TK_COL_W, TK_ROW_H);
            const struct cell *cell = sheet_cell(st->sh, row, col);
            char text[SHEET_TEXT_MAX];
            bool editing_here = st->editing && row == st->row &&
                                col == st->col;

            if (editing_here)
                strlcpy(text, st->edit, sizeof(text));
            else
                sheet_display(st->sh, row, col, text, sizeof(text));

            uint32_t color = COL_TEXT;

            if (!editing_here && cell) {
                if (cell->kind == CELL_FORMULA && cell->error != CELL_OK)
                    color = TK_ERROR;
                else if (cell->kind == CELL_FORMULA)
                    color = TK_FORMULA;
            }

            struct canvas inner = g;

            gfx_set_clip(&inner, rect_intersect(g.clip,
                                                rect_make(box.x + 1, box.y,
                                                          box.w - 3, box.h)));

            if (editing_here)
                gfx_fill(&inner, box, COL_FIELD);

            int32_t tw = gfx_text_width(text);
            int32_t tx = box.x + 3;

            /* Zahlen stehen rechts, Text links - so liest sich eine
             * Spalte von Betraegen als Spalte. */
            if (!editing_here && sheet_is_numeric(st->sh, row, col))
                tx = box.x + box.w - 4 - tw;

            if (cell && cell->bold)
                gfx_text_bold(&inner, tx, box.y + 2, text, color);
            else
                gfx_text(&inner, tx, box.y + 2, text, color);

            if (editing_here && st->caret_on)
                gfx_fill(&inner, rect_make(box.x + 3 + tw, box.y + 2, 2,
                                           FONT_HEIGHT), COL_ACCENT);
        }
    }

    /* Der Rahmen um die gewaehlte Zelle. */
    if (st->row >= st->scroll_row && st->row < st->scroll_row + rows &&
        st->col >= st->scroll_col && st->col < st->scroll_col + cols) {
        struct rect box = rect_make(
            area.x + TK_HEAD_W + (st->col - st->scroll_col) * TK_COL_W,
            area.y + TK_HEAD_H + (st->row - st->scroll_row) * TK_ROW_H,
            TK_COL_W, TK_ROW_H);

        gfx_frame(&g, box, TK_CURSOR);
        gfx_frame(&g, rect_make(box.x - 1, box.y - 1, box.w + 2, box.h + 2),
                  TK_CURSOR);
    }

    widget_vscroll(c, rect_make(area.x + area.w, area.y,
                                SCROLLBAR_WIDTH, area.h),
                   st->scroll_row, SHEET_ROWS, rows);
}

static void tk_paint(struct window *win, struct canvas *c)
{
    struct tk_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);
    paint_toolbar(win, &local);
    paint_editbar(win, &local);
    paint_grid(win, &local);

    const struct cell *cell = sheet_cell(st->sh, st->row, st->col);
    char left[128], right[64], name[8];

    sheet_ref_name(st->row, st->col, name, sizeof(name));

    if (cell && cell->kind == CELL_FORMULA && cell->error != CELL_OK) {
        ksnprintf(left, sizeof(left), "%s: %s", name,
                  sheet_error_text((enum cell_error)cell->error));
    } else if (cell && cell->kind == CELL_FORMULA) {
        char value[32];

        sheet_format_number(cell->value, value, sizeof(value));
        ksnprintf(left, sizeof(left), "%s = %s", name, value);
    } else {
        ksnprintf(left, sizeof(left), "%s", name);
    }

    ksnprintf(right, sizeof(right), "%u Zellen belegt",
              (unsigned)sheet_used(st->sh));
    widget_statusbar(&local, rect_make(0, local.h - TK_STATUS_H,
                                       local.w, TK_STATUS_H), left, right);
}

/* ------------------------------------------------------------------ */
/* Eingabe                                                             */
/* ------------------------------------------------------------------ */

static void tk_action(struct window *win, int action)
{
    struct tk_state *st = win->user;

    switch (action) {
    case TK_OPEN:
        dialog_input("Oeffnen", "Datei:", "tabelle.csv", on_open, win);
        break;
    case TK_SAVE:
        do_save(win);
        break;
    case TK_SUM:
        commit_edit(win);
        insert_sum(win);
        break;
    case TK_BOLD: {
        struct cell *cell = (struct cell *)sheet_cell(st->sh, st->row, st->col);

        if (cell) {
            cell->bold = !cell->bold;
            st->modified = true;
            update_title(win);
        }
        gui_invalidate();
        break;
    }
    case TK_CLEAR:
        cancel_edit(st);
        sheet_set(st->sh, st->row, st->col, "");
        sheet_recalc(st->sh);
        st->modified = true;
        update_title(win);
        gui_invalidate();
        break;
    }
}

static void tk_key(struct window *win, const struct gui_event *ev)
{
    struct tk_state *st = win->user;

    if (ev->mods & MOD_CTRL) {
        switch (ev->ascii) {
        case 's': case 'S':
            do_save(win);
            return;
        case 'c': case 'C': {
            const struct cell *cell = sheet_cell(st->sh, st->row, st->col);

            if (cell)
                clipboard_set(cell->text, strlen(cell->text));
            return;
        }
        case 'v': case 'V': {
            size_t bytes = 0;
            const char *paste = clipboard_get(&bytes);

            if (paste) {
                char text[SHEET_TEXT_MAX];
                size_t n = MIN(bytes, sizeof(text) - 1);

                memcpy(text, paste, n);
                text[n] = '\0';
                for (size_t i = 0; i < n; i++) {
                    if (text[i] == '\n' || text[i] == '\r')
                        text[i] = '\0';
                }
                cancel_edit(st);
                sheet_set(st->sh, st->row, st->col, text);
                sheet_recalc(st->sh);
                st->modified = true;
                update_title(win);
                gui_invalidate();
            }
            return;
        }
        default:
            break;
        }
    }

    switch (ev->key) {
    case KEY_ESCAPE:
        cancel_edit(st);
        gui_invalidate();
        return;

    case KEY_ENTER:
        commit_edit(win);
        move_to(win, st->row + 1, st->col);
        return;

    case KEY_TAB:
        commit_edit(win);
        move_to(win, st->row, st->col + 1);
        return;

    case KEY_F2:
        if (!st->editing)
            begin_edit(win, true);
        gui_invalidate();
        return;

    case KEY_BACKSPACE:
        if (st->editing) {
            if (st->edit_len > 0)
                st->edit[--st->edit_len] = '\0';
            st->caret_on = true;
            gui_invalidate();
        } else {
            move_to(win, st->row, st->col - 1);
        }
        return;

    case KEY_DELETE:
        if (!st->editing) {
            sheet_set(st->sh, st->row, st->col, "");
            sheet_recalc(st->sh);
            st->modified = true;
            update_title(win);
            gui_invalidate();
        }
        return;

    case KEY_LEFT:   move_to(win, st->row, st->col - 1); return;
    case KEY_RIGHT:  move_to(win, st->row, st->col + 1); return;
    case KEY_UP:     move_to(win, st->row - 1, st->col); return;
    case KEY_DOWN:   move_to(win, st->row + 1, st->col); return;
    case KEY_HOME:   move_to(win, st->row, 0);           return;
    case KEY_END:    move_to(win, st->row, SHEET_COLS - 1); return;
    case KEY_PAGEUP:
        move_to(win, st->row - visible_rows(win), st->col);
        return;
    case KEY_PAGEDOWN:
        move_to(win, st->row + visible_rows(win), st->col);
        return;

    default:
        break;
    }

    if (ev->ascii >= 32 && (unsigned char)ev->ascii != 127) {
        if (!st->editing)
            begin_edit(win, false);
        if (st->edit_len + 1 < (int)sizeof(st->edit)) {
            st->edit[st->edit_len++] = ev->ascii;
            st->edit[st->edit_len] = '\0';
        }
        st->caret_on = true;
        gui_invalidate();
    }
}

/* Welche Zelle liegt unter dem Zeiger? */
static bool cell_at(struct window *win, int32_t x, int32_t y,
                    int *row, int *col)
{
    struct tk_state *st = win->user;
    struct rect area = grid_rect(win);

    if (x < area.x + TK_HEAD_W || y < area.y + TK_HEAD_H)
        return false;
    if (x >= area.x + area.w || y >= area.y + area.h)
        return false;

    *col = st->scroll_col + (x - area.x - TK_HEAD_W) / TK_COL_W;
    *row = st->scroll_row + (y - area.y - TK_HEAD_H) / TK_ROW_H;
    return *row < SHEET_ROWS && *col < SHEET_COLS;
}

static void tk_event(struct window *win, const struct gui_event *ev)
{
    struct tk_state *st = win->user;

    switch (ev->type) {
    case EV_MOUSE_DOWN: {
        struct rect area = grid_rect(win);
        int row, col;

        if (ev->y < TK_TOOLBAR_H) {
            for (int i = 0; i < TK_COUNT; i++) {
                if (rect_contains(tool_rect(i), ev->x, ev->y)) {
                    st->pressed = i;
                    gui_invalidate();
                    break;
                }
            }
        } else if (ev->x >= area.x + area.w && ev->y >= area.y) {
            st->scroll_row = widget_vscroll_click(
                rect_make(area.x + area.w, area.y, SCROLLBAR_WIDTH, area.h),
                ev->y, st->scroll_row, SHEET_ROWS, visible_rows(win));
            gui_invalidate();
        } else if (cell_at(win, ev->x, ev->y, &row, &col)) {
            move_to(win, row, col);
        }
        break;
    }

    case EV_MOUSE_UP: {
        int pressed = st->pressed;

        st->pressed = -1;
        if (pressed >= 0 && rect_contains(tool_rect(pressed), ev->x, ev->y))
            tk_action(win, pressed);
        gui_invalidate();
        break;
    }

    case EV_DOUBLE_CLICK: {
        int row, col;

        if (cell_at(win, ev->x, ev->y, &row, &col)) {
            move_to(win, row, col);
            begin_edit(win, true);
            gui_invalidate();
        }
        break;
    }

    case EV_SCROLL:
        st->scroll_row = CLAMP(st->scroll_row - ev->scroll * 3, 0,
                               MAX(SHEET_ROWS - visible_rows(win), 0));
        gui_invalidate();
        break;

    case EV_KEY_DOWN:
        tk_key(win, ev);
        break;

    case EV_TICK:
        if (st->editing) {
            st->caret_on = !st->caret_on;
            gui_invalidate();
        }
        break;

    case EV_RESIZED:
        ensure_visible(win);
        gui_invalidate();
        break;

    default:
        break;
    }
}

static void tk_close(struct window *win)
{
    struct tk_state *st = win->user;

    if (st) {
        kfree(st->sh);
        kfree(st);
    }
    win->user = NULL;
}

/* Ein Beispiel beim Start: Es zeigt in vier Zeilen, wozu das Fenster
 * da ist - Zahlen, eine Formel und eine Summe. */
static void fill_example(struct sheet *sh)
{
    static const struct {
        const char *ref;
        const char *text;
    } start[] = {
        { "A1", "Artikel" },   { "B1", "Anzahl" }, { "C1", "Preis" },
        { "D1", "Betrag" },
        { "A2", "Schrauben" }, { "B2", "120" },  { "C2", "0,08" },
        { "D2", "=B2*C2" },
        { "A3", "Duebel" },    { "B3", "80" },   { "C3", "0,12" },
        { "D3", "=B3*C3" },
        { "A4", "Winkel" },    { "B4", "16" },   { "C4", "1,45" },
        { "D4", "=B4*C4" },
        { "A6", "Summe" },     { "D6", "=SUMME(D2:D4)" },
        { "A7", "Mittelwert" },{ "D7", "=MITTELWERT(D2:D4)" },
    };

    for (size_t i = 0; i < ARRAY_LEN(start); i++) {
        int row, col;

        if (sheet_parse_ref(start[i].ref, &row, &col))
            sheet_set(sh, row, col, start[i].text);
    }

    for (int c = 0; c < 4; c++)
        sh->cells[0][c].bold = 1;
    sh->cells[5][0].bold = 1;
    sh->cells[5][3].bold = 1;

    sheet_recalc(sh);
}

static void sheet_new(struct fs_node *file)
{
    struct tk_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    st->sh = kzalloc(sizeof(struct sheet));
    if (!st->sh) {
        kfree(st);
        return;
    }

    static int32_t cascade;
    int32_t offset = (cascade++ % 5) * 24;

    struct window *win = gui_create_window("Tabelle", 100 + offset,
                                           60 + offset, 700, 460,
                                           WF_RESIZABLE, ICON_TABLE);
    if (!win) {
        kfree(st->sh);
        kfree(st);
        return;
    }

    st->pressed = -1;
    win->user     = st;
    win->on_paint = tk_paint;
    win->on_event = tk_event;
    win->on_close = tk_close;
    win->min_w    = 460;
    win->min_h    = 280;

    if (file && fs_load(file)) {
        sheet_from_csv(st->sh, (const char *)file->data, file->size);
        st->file = file;
    } else {
        fill_example(st->sh);
    }

    update_title(win);
    gui_focus_window(win);
}

void sheet_open(struct fs_node *file)
{
    for (size_t i = 0; i < gui_window_count(); i++) {
        struct window *win = gui_window_at(i);

        if (win->on_paint == tk_paint) {
            struct tk_state *st = win->user;

            if (st && st->file == file) {
                gui_focus_window(win);
                return;
            }
        }
    }
    sheet_new(file);
}

void app_sheet(void)
{
    sheet_new(NULL);
}
