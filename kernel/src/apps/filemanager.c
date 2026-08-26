/* filemanager.c - der Dateimanager von RetroOS.
 *
 * Zeigt einen Ordner als Liste mit Symbol, Name, Groesse und Aenderungs-
 * zeitpunkt. Bedienbar mit Maus (Doppelklick, Kontextmenue) und Tastatur
 * (Pfeiltasten, Eingabe, Ruecktaste, Entf, F2).
 */

#include "apps.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"
#include "theme.h"
#include "widgets.h"

#define TOOLBAR_H   34
#define PATHBAR_H   26
#define STATUS_H    24
#define ROW_H       20
#define MAX_ENTRIES 256
#define HISTORY_MAX 32

enum toolbar_button {
    TB_BACK,
    TB_UP,
    TB_HOME,
    TB_NEW_DIR,
    TB_NEW_FILE,
    TB_RENAME,
    TB_DELETE,
    TB_COUNT
};

enum context_id {
    CTX_OPEN = 1,
    CTX_RENAME,
    CTX_DELETE,
    CTX_NEW_DIR,
    CTX_NEW_FILE,
};

struct fm_state {
    struct fs_node *dir;
    struct fs_node *entries[MAX_ENTRIES];
    size_t          count;

    int32_t selection;
    int32_t scroll;

    struct fs_node *history[HISTORY_MAX];
    size_t          history_len;

    int  pressed_tool;
    bool tool_enabled[TB_COUNT];
};

static void fm_refresh(struct window *win);

/* ------------------------------------------------------------------ */
/* Hilfsfunktionen                                                     */
/* ------------------------------------------------------------------ */

static struct rect list_rect(struct window *win)
{
    return rect_make(0, TOOLBAR_H + PATHBAR_H,
                     gui_client_width(win) - SCROLLBAR_WIDTH,
                     gui_client_height(win) - TOOLBAR_H - PATHBAR_H - STATUS_H);
}

static struct rect scroll_rect(struct window *win)
{
    struct rect l = list_rect(win);

    return rect_make(l.x + l.w, l.y, SCROLLBAR_WIDTH, l.h);
}

static int32_t visible_rows(struct window *win)
{
    return MAX(list_rect(win).h / ROW_H, 1);
}

static struct rect tool_rect(int index)
{
    static const int32_t widths[TB_COUNT] = { 34, 34, 34, 130, 114, 114, 98 };
    int32_t x = 4;

    for (int i = 0; i < index; i++)
        x += widths[i] + (i == TB_HOME ? 12 : 3);

    return rect_make(x, 4, widths[index], TOOLBAR_H - 8);
}

static void fm_set_dir(struct window *win, struct fs_node *dir, bool remember)
{
    struct fm_state *st = win->user;

    if (!dir || dir->type != FS_DIR)
        return;

    if (remember && st->dir && st->history_len < HISTORY_MAX)
        st->history[st->history_len++] = st->dir;

    st->dir       = dir;
    st->selection = st->count ? 0 : -1;
    st->scroll    = 0;

    fm_refresh(win);
}

static void fm_refresh(struct window *win)
{
    struct fm_state *st = win->user;
    char path[FS_PATH_MAX];
    char title[WIN_TITLE_MAX + 1];

    st->count = fs_list(st->dir, st->entries, MAX_ENTRIES);

    if (st->selection >= (int32_t)st->count)
        st->selection = (int32_t)st->count - 1;
    if (st->count == 0)
        st->selection = -1;

    fs_path(st->dir, path, sizeof(path));
    ksnprintf(title, sizeof(title), "Dateimanager - %s", path);
    gui_set_title(win, title);

    struct fs_node *sel = (st->selection >= 0) ? st->entries[st->selection] : NULL;

    st->tool_enabled[TB_BACK]     = st->history_len > 0;
    st->tool_enabled[TB_UP]       = st->dir->parent != NULL;
    st->tool_enabled[TB_HOME]     = st->dir != fs_root();
    st->tool_enabled[TB_NEW_DIR]  = true;
    st->tool_enabled[TB_NEW_FILE] = true;
    st->tool_enabled[TB_RENAME]   = sel && !sel->readonly;
    st->tool_enabled[TB_DELETE]   = sel && !sel->readonly;

    gui_invalidate();
}

static void ensure_visible(struct window *win)
{
    struct fm_state *st = win->user;
    int32_t rows = visible_rows(win);

    if (st->selection < 0)
        return;
    if (st->selection < st->scroll)
        st->scroll = st->selection;
    else if (st->selection >= st->scroll + rows)
        st->scroll = st->selection - rows + 1;
}

static enum icon_id entry_icon(const struct fs_node *node)
{
    if (node == fs_disk_root())
        return ICON_DISK;
    if (node->type == FS_DIR)
        return ICON_FOLDER;
    return fs_is_text(node) ? ICON_FILE_TEXT : ICON_FILE;
}

static void fm_open_selected(struct window *win)
{
    struct fm_state *st = win->user;

    if (st->selection < 0)
        return;

    struct fs_node *node = st->entries[st->selection];

    if (node->type == FS_DIR)
        fm_set_dir(win, node, true);
    else
        editor_open(node);
}

/* ------------------------------------------------------------------ */
/* Aktionen                                                            */
/* ------------------------------------------------------------------ */

static void on_new_dir(const char *name, void *user)
{
    struct window *win = user;

    if (!gui_window_alive(win))
        return;

    struct fm_state *st = win->user;

    if (!fs_create(st->dir, name, FS_DIR))
        dialog_message("Neuer Ordner",
                       "Der Ordner konnte nicht angelegt werden. "
                       "Moeglicherweise gibt es ihn bereits.");
    fm_refresh(win);
}

static void on_new_file(const char *name, void *user)
{
    struct window *win = user;

    if (!gui_window_alive(win))
        return;

    struct fm_state *st = win->user;
    struct fs_node  *f   = fs_create(st->dir, name, FS_FILE);

    if (!f) {
        dialog_message("Neue Datei",
                       "Die Datei konnte nicht angelegt werden. "
                       "Moeglicherweise gibt es sie bereits.");
        return;
    }
    fs_write(f, "", 0);
    fm_refresh(win);
}

static void on_rename(const char *name, void *user)
{
    struct window *win = user;

    if (!gui_window_alive(win))
        return;

    struct fm_state *st = win->user;

    if (st->selection < 0)
        return;

    if (!fs_rename(st->entries[st->selection], name))
        dialog_message("Umbenennen", "Der Name konnte nicht geaendert werden.");
    fm_refresh(win);
}

static void on_delete_confirmed(bool yes, void *user)
{
    struct window *win = user;

    if (!yes || !gui_window_alive(win))
        return;

    struct fm_state *st = win->user;

    if (st->selection < 0)
        return;

    if (!fs_remove(st->entries[st->selection]))
        dialog_message("Loeschen",
                       "Dieser Eintrag gehoert zum System und "
                       "laesst sich nicht loeschen.");
    fm_refresh(win);
}

static void fm_action(struct window *win, int action)
{
    struct fm_state *st = win->user;
    struct fs_node  *sel = (st->selection >= 0) ? st->entries[st->selection] : NULL;

    switch (action) {
    case TB_BACK:
        if (st->history_len > 0)
            fm_set_dir(win, st->history[--st->history_len], false);
        break;

    case TB_UP:
        if (st->dir->parent)
            fm_set_dir(win, st->dir->parent, true);
        break;

    case TB_HOME:
        fm_set_dir(win, fs_root(), true);
        break;

    case TB_NEW_DIR:
        dialog_input("Neuer Ordner", "Name des neuen Ordners:",
                     "Neuer Ordner", on_new_dir, win);
        break;

    case TB_NEW_FILE:
        dialog_input("Neue Datei", "Name der neuen Datei:",
                     "Unbenannt.txt", on_new_file, win);
        break;

    case TB_RENAME:
        if (sel && !sel->readonly)
            dialog_input("Umbenennen", "Neuer Name:", sel->name, on_rename, win);
        break;

    case TB_DELETE:
        if (sel && !sel->readonly) {
            char msg[200];

            ksnprintf(msg, sizeof(msg),
                      sel->type == FS_DIR
                          ? "Ordner \"%s\" mit allen Inhalten loeschen?"
                          : "Datei \"%s\" wirklich loeschen?",
                      sel->name);
            dialog_confirm("Loeschen", msg, on_delete_confirmed, win);
        }
        break;
    }
}

static void context_selected(int id, void *user)
{
    struct window *win = user;

    switch (id) {
    case CTX_OPEN:     fm_open_selected(win);        break;
    case CTX_RENAME:   fm_action(win, TB_RENAME);    break;
    case CTX_DELETE:   fm_action(win, TB_DELETE);    break;
    case CTX_NEW_DIR:  fm_action(win, TB_NEW_DIR);   break;
    case CTX_NEW_FILE: fm_action(win, TB_NEW_FILE);  break;
    }
}

static void open_context_menu(struct window *win, int32_t sx, int32_t sy)
{
    struct fm_state *st = win->user;
    struct fs_node *sel = (st->selection >= 0) ? st->entries[st->selection] : NULL;
    bool editable = sel && !sel->readonly;

    struct menu_item items[] = {
        { "Oeffnen",      ICON_FOLDER_OPEN, true, sel != NULL, CTX_OPEN },
        { "Umbenennen",   ICON_EDITOR,      true, editable,    CTX_RENAME },
        { "Loeschen",     ICON_TRASH,       true, editable,    CTX_DELETE },
        { NULL,           ICON_FILE,        false, false,      0 },
        { "Neuer Ordner", ICON_NEW_FOLDER,  true, true,        CTX_NEW_DIR },
        { "Neue Datei",   ICON_NEW_FILE,    true, true,        CTX_NEW_FILE },
    };

    gui_open_menu(sx, sy, items, ARRAY_LEN(items), context_selected, win);
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static void paint_toolbar(struct window *win, struct canvas *c)
{
    struct fm_state *st = win->user;
    static const char *labels[TB_COUNT] = {
        "", "", "", "Neuer Ordner", "Neue Datei", "Umbenennen", "Loeschen"
    };
    static const enum icon_id icons[TB_COUNT] = {
        ICON_BACK, ICON_UP, ICON_HOME,
        ICON_NEW_FOLDER, ICON_NEW_FILE, ICON_EDITOR, ICON_TRASH
    };

    widget_toolbar(c, rect_make(0, 0, c->w, TOOLBAR_H));

    for (int i = 0; i < TB_COUNT; i++) {
        struct rect r = tool_rect(i);

        if (r.x + r.w > c->w)
            break;
        widget_icon_button(c, r, icons[i], labels[i],
                           st->pressed_tool == i, st->tool_enabled[i]);
    }
}

static void paint_pathbar(struct window *win, struct canvas *c)
{
    struct fm_state *st = win->user;
    char path[FS_PATH_MAX];

    fs_path(st->dir, path, sizeof(path));

    struct rect r = rect_make(4, TOOLBAR_H + 3, c->w - 8, PATHBAR_H - 6);

    gfx_fill(c, rect_make(0, TOOLBAR_H, c->w, PATHBAR_H), COL_FACE);
    gfx_fill(c, r, COL_FIELD);
    gfx_bevel_thin(c, r, false);

    icon_draw(c, r.x + 3, r.y + 2, ICON_FOLDER_OPEN, 1);
    struct canvas clipped = *c;
    gfx_set_clip(&clipped, rect_intersect(c->clip, r));
    gfx_text(&clipped, r.x + 24, r.y + 3, path, COL_TEXT);
}

static void paint_list(struct window *win, struct canvas *c)
{
    struct fm_state *st = win->user;
    struct rect area = list_rect(win);
    int32_t rows = visible_rows(win);

    gfx_fill(c, area, COL_FIELD);

    struct canvas clipped = *c;
    gfx_set_clip(&clipped, rect_intersect(c->clip, area));

    int32_t col_size = area.w - 210;
    int32_t col_date = area.w - 130;

    if (st->count == 0) {
        gfx_text(&clipped, area.x + 12, area.y + 10,
                 "Dieser Ordner ist leer.", COL_TEXT_DIM);
    }

    for (int32_t i = 0; i < rows; i++) {
        int32_t index = st->scroll + i;

        if (index >= (int32_t)st->count)
            break;

        struct fs_node *node = st->entries[index];
        int32_t y = area.y + i * ROW_H;
        bool    sel = (index == st->selection);

        if (sel)
            gfx_fill(&clipped, rect_make(area.x, y, area.w, ROW_H), COL_SELECT);

        uint32_t color = sel ? COL_SELECT_TEXT : COL_TEXT;
        uint32_t dim   = sel ? COL_SELECT_TEXT : COL_TEXT_DIM;

        icon_draw(&clipped, area.x + 3, y + 2, entry_icon(node), 1);
        gfx_text_clipped(&clipped, area.x + 24, y + 2, node->name, color,
                         col_size - 30);

        if (col_size > 120) {
            char size[24];

            if (node->type == FS_DIR) {
                size_t n = fs_child_count(node);
                ksnprintf(size, sizeof(size), "%u Eintr.", (unsigned)n);
            } else {
                fs_format_size(size, sizeof(size), node->size);
            }
            gfx_text(&clipped, area.x + col_size, y + 2, size, dim);

            char date[24];
            ksnprintf(date, sizeof(date), "%02u.%02u. %02u:%02u",
                      node->mtime_day, node->mtime_month,
                      node->mtime_hour, node->mtime_min);
            gfx_text(&clipped, area.x + col_date, y + 2, date, dim);
        }
    }

    gfx_bevel_thin(c, area, false);
}

static void paint_status(struct window *win, struct canvas *c)
{
    struct fm_state *st = win->user;
    char left[96], right[48];
    size_t dirs = 0, files = 0;

    for (size_t i = 0; i < st->count; i++) {
        if (st->entries[i]->type == FS_DIR)
            dirs++;
        else
            files++;
    }

    ksnprintf(left, sizeof(left), "%u Ordner, %u Dateien",
              (unsigned)dirs, (unsigned)files);

    char total[24];
    fs_format_size(total, sizeof(total), fs_total_size(st->dir));
    ksnprintf(right, sizeof(right), "%s", total);

    widget_statusbar(c, rect_make(0, c->h - STATUS_H, c->w, STATUS_H), left, right);
}

static void fm_paint(struct window *win, struct canvas *c)
{
    struct fm_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    paint_toolbar(win, &local);
    paint_pathbar(win, &local);
    paint_list(win, &local);
    widget_vscroll(&local, scroll_rect(win), st->scroll,
                   (int32_t)st->count, visible_rows(win));
    paint_status(win, &local);
}

/* ------------------------------------------------------------------ */
/* Ereignisse                                                          */
/* ------------------------------------------------------------------ */

static int32_t row_at(struct window *win, int32_t y)
{
    struct fm_state *st = win->user;
    struct rect area = list_rect(win);

    if (y < area.y || y >= area.y + area.h)
        return -1;

    int32_t index = st->scroll + (y - area.y) / ROW_H;
    return (index < (int32_t)st->count) ? index : -1;
}

static void fm_mouse_down(struct window *win, const struct gui_event *ev)
{
    struct fm_state *st = win->user;

    if (ev->y < TOOLBAR_H) {
        for (int i = 0; i < TB_COUNT; i++) {
            if (rect_contains(tool_rect(i), ev->x, ev->y) && st->tool_enabled[i]) {
                st->pressed_tool = i;
                gui_invalidate();
                return;
            }
        }
        return;
    }

    int32_t index = row_at(win, ev->y);
    struct rect sb = scroll_rect(win);

    if (rect_contains(sb, ev->x, ev->y)) {
        st->scroll = widget_vscroll_click(sb, ev->y, st->scroll,
                                          (int32_t)st->count, visible_rows(win));
        gui_invalidate();
        return;
    }

    if (index >= 0) {
        st->selection = index;
        fm_refresh(win);
    }

    if (ev->button == MB_RIGHT) {
        struct rect client = gui_client_rect(win);
        open_context_menu(win, client.x + ev->x, client.y + ev->y);
    }
}

static void fm_key(struct window *win, const struct gui_event *ev)
{
    struct fm_state *st = win->user;
    int32_t rows = visible_rows(win);

    switch (ev->key) {
    case KEY_UP:
        if (st->selection > 0)
            st->selection--;
        break;
    case KEY_DOWN:
        if (st->selection + 1 < (int32_t)st->count)
            st->selection++;
        break;
    case KEY_PAGEUP:
        st->selection = MAX(st->selection - rows, 0);
        break;
    case KEY_PAGEDOWN:
        st->selection = MIN(st->selection + rows, (int32_t)st->count - 1);
        break;
    case KEY_HOME:
        st->selection = st->count ? 0 : -1;
        break;
    case KEY_END:
        st->selection = (int32_t)st->count - 1;
        break;
    case KEY_ENTER:
        fm_open_selected(win);
        return;
    case KEY_BACKSPACE:
        fm_action(win, TB_UP);
        return;
    case KEY_DELETE:
        fm_action(win, TB_DELETE);
        return;
    case KEY_F2:
        fm_action(win, TB_RENAME);
        return;
    case KEY_F5:
        fm_refresh(win);
        return;
    default:
        return;
    }

    ensure_visible(win);
    fm_refresh(win);
}

static void fm_event(struct window *win, const struct gui_event *ev)
{
    struct fm_state *st = win->user;

    switch (ev->type) {
    case EV_MOUSE_DOWN:
        fm_mouse_down(win, ev);
        break;

    case EV_MOUSE_UP: {
        int tool = st->pressed_tool;

        st->pressed_tool = -1;
        if (tool >= 0 && rect_contains(tool_rect(tool), ev->x, ev->y))
            fm_action(win, tool);
        gui_invalidate();
        break;
    }

    case EV_DOUBLE_CLICK:
        if (row_at(win, ev->y) >= 0) {
            st->selection = row_at(win, ev->y);
            fm_open_selected(win);
        }
        break;

    case EV_SCROLL: {
        int32_t max_off = MAX((int32_t)st->count - visible_rows(win), 0);

        st->scroll = CLAMP(st->scroll - ev->scroll * 3, 0, max_off);
        gui_invalidate();
        break;
    }

    case EV_KEY_DOWN:
        fm_key(win, ev);
        break;

    case EV_RESIZED:
        ensure_visible(win);
        gui_invalidate();
        break;

    default:
        break;
    }
}

static void fm_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_filemanager(void)
{
    struct fm_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    static int32_t cascade;
    int32_t offset = (cascade++ % 5) * 24;

    struct window *win = gui_create_window("Dateimanager",
                                           90 + offset, 60 + offset, 640, 420,
                                           WF_RESIZABLE, ICON_FOLDER_OPEN);
    if (!win) {
        kfree(st);
        return;
    }

    st->dir          = fs_root();
    st->selection    = 0;
    st->pressed_tool = -1;

    win->user     = st;
    win->on_paint = fm_paint;
    win->on_event = fm_event;
    win->on_close = fm_close;
    win->min_w    = 420;
    win->min_h    = 240;

    fm_refresh(win);
    gui_focus_window(win);
}
