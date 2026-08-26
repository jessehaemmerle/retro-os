/* editor.c - Texteditor.
 *
 * Der Text liegt als ein einziger wachsender Puffer im Heap. Zeilenanfaenge
 * werden beim Zeichnen bestimmt - bei Dateien dieser Groessenordnung ist das
 * guenstiger als eine staendig gepflegte Zeilenliste.
 */

#include "apps.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"
#include "theme.h"
#include "widgets.h"

#define ED_TOOLBAR_H 34
#define ED_STATUS_H  24
#define ED_LINE_H    16
#define ED_MAX_LINES 4096
#define ED_MARGIN    6

enum ed_button { ED_SAVE, ED_NEW_LINE_INFO, ED_COUNT };

struct ed_state {
    struct fs_node *file;

    char   *text;
    size_t  len;
    size_t  cap;

    size_t  cursor;        /* Byteposition im Puffer */
    int32_t scroll;        /* erste sichtbare Zeile  */
    bool    modified;
    bool    caret_on;
    int     pressed;

    int32_t line_start[ED_MAX_LINES];
    int32_t line_count;
};

static void ed_recount(struct ed_state *st)
{
    st->line_count = 0;
    st->line_start[st->line_count++] = 0;

    for (size_t i = 0; i < st->len && st->line_count < ED_MAX_LINES; i++) {
        if (st->text[i] == '\n')
            st->line_start[st->line_count++] = (int32_t)i + 1;
    }
}

static int32_t ed_line_of(struct ed_state *st, size_t pos)
{
    int32_t lo = 0, hi = st->line_count - 1;

    while (lo < hi) {
        int32_t mid = (lo + hi + 1) / 2;

        if ((size_t)st->line_start[mid] <= pos)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

static int32_t ed_line_length(struct ed_state *st, int32_t line)
{
    int32_t start = st->line_start[line];
    int32_t end   = (line + 1 < st->line_count) ? st->line_start[line + 1] - 1
                                                : (int32_t)st->len;
    return MAX(end - start, 0);
}

static struct rect ed_text_rect(struct window *win)
{
    return rect_make(0, ED_TOOLBAR_H,
                     gui_client_width(win) - SCROLLBAR_WIDTH,
                     gui_client_height(win) - ED_TOOLBAR_H - ED_STATUS_H);
}

static int32_t ed_visible_lines(struct window *win)
{
    return MAX(ed_text_rect(win).h / ED_LINE_H, 1);
}

static bool ed_reserve(struct ed_state *st, size_t need)
{
    if (st->cap >= need)
        return true;

    size_t cap = st->cap ? st->cap : 256;
    while (cap < need)
        cap *= 2;

    char *buf = krealloc(st->text, cap);
    if (!buf)
        return false;

    st->text = buf;
    st->cap  = cap;
    return true;
}

static void ed_insert(struct ed_state *st, char c)
{
    if (!ed_reserve(st, st->len + 2))
        return;

    memmove(&st->text[st->cursor + 1], &st->text[st->cursor],
            st->len - st->cursor + 1);
    st->text[st->cursor++] = c;
    st->len++;
    st->text[st->len] = '\0';
    st->modified = true;
    ed_recount(st);
}

static void ed_erase(struct ed_state *st, size_t pos)
{
    if (pos >= st->len)
        return;

    memmove(&st->text[pos], &st->text[pos + 1], st->len - pos);
    st->len--;
    st->modified = true;
    ed_recount(st);
}

static void ed_ensure_visible(struct window *win, struct ed_state *st)
{
    int32_t line = ed_line_of(st, st->cursor);
    int32_t rows = ed_visible_lines(win);

    if (line < st->scroll)
        st->scroll = line;
    else if (line >= st->scroll + rows)
        st->scroll = line - rows + 1;
}

static void ed_update_title(struct window *win, struct ed_state *st)
{
    char title[WIN_TITLE_MAX + 1];

    ksnprintf(title, sizeof(title), "Editor - %s%s",
              st->file ? st->file->name : "Unbenannt",
              st->modified ? " *" : "");
    gui_set_title(win, title);
}

static void ed_save(struct window *win)
{
    struct ed_state *st = win->user;

    if (st->file && !fs_node_alive(st->file)) {
        st->file = NULL;
        ed_update_title(win, st);
        dialog_message("Speichern",
                       "Die Datei wurde inzwischen geloescht. "
                       "Der Text ist noch da, hat aber kein Ziel mehr.");
        return;
    }

    if (!st->file) {
        dialog_message("Speichern", "Zu diesem Text gehoert keine Datei.");
        return;
    }
    if (st->file->readonly) {
        dialog_message("Speichern",
                       "Diese Datei gehoert zum System und ist schreibgeschuetzt.");
        return;
    }

    if (!fs_write(st->file, st->text, st->len)) {
        dialog_message("Speichern", "Die Datei konnte nicht geschrieben werden.");
        return;
    }

    st->modified = false;
    ed_update_title(win, st);
    gui_invalidate();
}

/* ------------------------------------------------------------------ */

static struct rect ed_button_rect(int index)
{
    return rect_make(4 + index * 106, 4, 102, ED_TOOLBAR_H - 8);
}

static void ed_paint(struct window *win, struct canvas *c)
{
    struct ed_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);
    struct rect area = ed_text_rect(win);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    widget_toolbar(&local, rect_make(0, 0, local.w, ED_TOOLBAR_H));
    widget_icon_button(&local, ed_button_rect(0), ICON_DISK, "Speichern",
                       st->pressed == ED_SAVE, st->file && !st->file->readonly);

    gfx_fill(&local, area, COL_FIELD);
    gfx_bevel_thin(&local, area, false);

    struct canvas text = local;
    gfx_set_clip(&text, rect_intersect(local.clip,
                                       rect_make(area.x + 2, area.y + 2,
                                                 area.w - 4, area.h - 4)));

    int32_t rows = ed_visible_lines(win);
    int32_t cur_line = ed_line_of(st, st->cursor);
    int32_t cur_col  = (int32_t)st->cursor - st->line_start[cur_line];

    for (int32_t i = 0; i < rows; i++) {
        int32_t line = st->scroll + i;

        if (line >= st->line_count)
            break;

        int32_t start = st->line_start[line];
        int32_t len   = ed_line_length(st, line);
        int32_t y     = area.y + 3 + i * ED_LINE_H;
        int32_t max   = (area.w - 2 * ED_MARGIN) / FONT_WIDTH;

        for (int32_t k = 0; k < len && k < max; k++)
            gfx_char(&text, area.x + ED_MARGIN + k * FONT_WIDTH, y,
                     (unsigned char)st->text[start + k], COL_TEXT, false);

        if (line == cur_line && st->caret_on) {
            int32_t cx = area.x + ED_MARGIN + MIN(cur_col, max) * FONT_WIDTH;
            gfx_fill(&text, rect_make(cx, y, 2, FONT_HEIGHT), COL_ACCENT);
        }
    }

    widget_vscroll(&local, rect_make(area.x + area.w, area.y,
                                     SCROLLBAR_WIDTH, area.h),
                   st->scroll, st->line_count, rows);

    char left[96], right[48];
    ksnprintf(left, sizeof(left), "Zeile %d von %d, Spalte %d",
              cur_line + 1, st->line_count, cur_col + 1);
    ksnprintf(right, sizeof(right), "%u Zeichen%s",
              (unsigned)st->len, st->modified ? " (geaendert)" : "");
    widget_statusbar(&local, rect_make(0, local.h - ED_STATUS_H,
                                       local.w, ED_STATUS_H), left, right);
}

static void ed_key(struct window *win, const struct gui_event *ev)
{
    struct ed_state *st = win->user;
    int32_t line = ed_line_of(st, st->cursor);
    int32_t col  = (int32_t)st->cursor - st->line_start[line];

    if ((ev->mods & MOD_CTRL) && (ev->ascii == 's' || ev->ascii == 'S')) {
        ed_save(win);
        return;
    }

    switch (ev->key) {
    case KEY_LEFT:
        if (st->cursor > 0)
            st->cursor--;
        break;
    case KEY_RIGHT:
        if (st->cursor < st->len)
            st->cursor++;
        break;
    case KEY_UP:
        if (line > 0) {
            int32_t target = MIN(col, ed_line_length(st, line - 1));
            st->cursor = (size_t)(st->line_start[line - 1] + target);
        }
        break;
    case KEY_DOWN:
        if (line + 1 < st->line_count) {
            int32_t target = MIN(col, ed_line_length(st, line + 1));
            st->cursor = (size_t)(st->line_start[line + 1] + target);
        }
        break;
    case KEY_HOME:
        st->cursor = (size_t)st->line_start[line];
        break;
    case KEY_END:
        st->cursor = (size_t)(st->line_start[line] + ed_line_length(st, line));
        break;
    case KEY_PAGEUP: {
        int32_t target = MAX(line - ed_visible_lines(win), 0);
        st->cursor = (size_t)st->line_start[target];
        break;
    }
    case KEY_PAGEDOWN: {
        int32_t target = MIN(line + ed_visible_lines(win), st->line_count - 1);
        st->cursor = (size_t)st->line_start[target];
        break;
    }
    case KEY_BACKSPACE:
        if (st->cursor > 0) {
            st->cursor--;
            ed_erase(st, st->cursor);
        }
        break;
    case KEY_DELETE:
        ed_erase(st, st->cursor);
        break;
    case KEY_ENTER:
        ed_insert(st, '\n');
        break;
    case KEY_TAB:
        for (int i = 0; i < 4; i++)
            ed_insert(st, ' ');
        break;
    default:
        if (ev->ascii >= 32 && (unsigned char)ev->ascii != 127)
            ed_insert(st, ev->ascii);
        else
            return;
        break;
    }

    st->caret_on = true;
    ed_ensure_visible(win, st);
    ed_update_title(win, st);
    gui_invalidate();
}

static void ed_click_text(struct window *win, int32_t x, int32_t y)
{
    struct ed_state *st = win->user;
    struct rect area = ed_text_rect(win);

    int32_t row  = (y - area.y - 3) / ED_LINE_H;
    int32_t line = CLAMP(st->scroll + row, 0, st->line_count - 1);
    int32_t col  = (x - area.x - ED_MARGIN + FONT_WIDTH / 2) / FONT_WIDTH;

    col = CLAMP(col, 0, ed_line_length(st, line));
    st->cursor = (size_t)(st->line_start[line] + col);
    st->caret_on = true;
    gui_invalidate();
}

static void ed_event(struct window *win, const struct gui_event *ev)
{
    struct ed_state *st = win->user;

    switch (ev->type) {
    case EV_MOUSE_DOWN: {
        struct rect area = ed_text_rect(win);

        if (ev->y < ED_TOOLBAR_H) {
            if (rect_contains(ed_button_rect(0), ev->x, ev->y)) {
                st->pressed = ED_SAVE;
                gui_invalidate();
            }
        } else if (ev->x >= area.x + area.w) {
            st->scroll = widget_vscroll_click(
                rect_make(area.x + area.w, area.y, SCROLLBAR_WIDTH, area.h),
                ev->y, st->scroll, st->line_count, ed_visible_lines(win));
            gui_invalidate();
        } else if (rect_contains(area, ev->x, ev->y)) {
            ed_click_text(win, ev->x, ev->y);
        }
        break;
    }

    case EV_MOUSE_UP: {
        int pressed = st->pressed;

        st->pressed = -1;
        if (pressed == ED_SAVE && rect_contains(ed_button_rect(0), ev->x, ev->y))
            ed_save(win);
        gui_invalidate();
        break;
    }

    case EV_SCROLL: {
        int32_t max_off = MAX(st->line_count - ed_visible_lines(win), 0);

        st->scroll = CLAMP(st->scroll - ev->scroll * 3, 0, max_off);
        gui_invalidate();
        break;
    }

    case EV_KEY_DOWN:
        ed_key(win, ev);
        break;

    case EV_TICK:
        st->caret_on = !st->caret_on;
        gui_invalidate();
        break;

    case EV_RESIZED:
        ed_ensure_visible(win, st);
        gui_invalidate();
        break;

    default:
        break;
    }
}

static void ed_close(struct window *win)
{
    struct ed_state *st = win->user;

    if (st) {
        kfree(st->text);
        kfree(st);
    }
    win->user = NULL;
}

static struct window *editor_new(struct fs_node *file)
{
    struct ed_state *st = kzalloc(sizeof(*st));

    if (!st)
        return NULL;

    static int32_t cascade;
    int32_t offset = (cascade++ % 5) * 24;

    struct window *win = gui_create_window("Editor", 150 + offset, 90 + offset,
                                           580, 400, WF_RESIZABLE, ICON_EDITOR);
    if (!win) {
        kfree(st);
        return NULL;
    }

    st->file    = file;
    st->pressed = -1;
    st->caret_on = true;

    size_t initial = file ? file->size : 0;
    if (!ed_reserve(st, initial + 256)) {
        gui_close_window(win);
        return NULL;
    }

    if (file && file->data)
        memcpy(st->text, file->data, initial);
    st->len = initial;
    st->text[st->len] = '\0';
    ed_recount(st);

    win->user     = st;
    win->on_paint = ed_paint;
    win->on_event = ed_event;
    win->on_close = ed_close;
    win->min_w    = 360;
    win->min_h    = 220;

    ed_update_title(win, st);
    gui_focus_window(win);
    return win;
}

void editor_open(struct fs_node *file)
{
    /* Ist die Datei bereits offen, nur nach vorne holen. */
    for (size_t i = 0; i < gui_window_count(); i++) {
        struct window *win = gui_window_at(i);

        if (win->on_paint == ed_paint) {
            struct ed_state *st = win->user;

            if (st && st->file == file) {
                gui_focus_window(win);
                return;
            }
        }
    }

    editor_new(file);
}

void app_editor(void)
{
    editor_new(NULL);
}
