/* codeapp.c - Programmieren.
 *
 * Ein Editor, der den Quelltext nicht nur haelt, sondern ihn auch
 * ausfuehrt. Der Deuter dafuer ist schon da: derselbe, mit dem der
 * Browser die Skripte einer Seite abarbeitet. Er bekommt hier nur ein
 * eigenes Fenster statt einer Seite drumherum.
 *
 * Drei Dinge unterscheiden das Fenster vom gewoehnlichen Editor:
 *
 *   Zeilennummern    damit eine Fehlermeldung hinfuehrt, wo sie
 *                    hingehoert
 *   Farben           Schluesselwoerter, Zeichenketten, Zahlen und
 *                    Anmerkungen werden beim Zeichnen erkannt; es gibt
 *                    keinen zweiten Durchgang und keinen Baum, nur
 *                    einen Zustand, der von Zeichen zu Zeichen laeuft
 *   Ausgabefenster   unten steht, was console.log gesagt hat - oder
 *                    woran es gescheitert ist
 *
 * F5 fuehrt aus, Strg+S speichert. Der Deuter wird fuer jeden Lauf neu
 * angelegt und danach weggeworfen: So faengt jeder Lauf sauber an, und
 * eine Endlosschleife nimmt nichts mit.
 */

#include "apps.h"
#include "arch.h"
#include "clipboard.h"
#include "font.h"
#include "js.h"
#include "kstring.h"
#include "mm.h"
#include "theme.h"
#include "widgets.h"
#include "lang.h"

#define CO_TOOLBAR_H 34
#define CO_STATUS_H  24
#define CO_LINE_H    16
#define CO_MAX_LINES 4096
#define CO_GUTTER_W  44
#define CO_MARGIN    6
#define CO_OUT_MIN   60
#define CO_OUT_MAX   40000

/* Farben des Quelltextes. Dunkler Grund, damit die Hervorhebung
 * ueberhaupt etwas hervorheben kann. */
#define CO_BG        RGB(0x1E, 0x22, 0x28)
#define CO_GUTTER    RGB(0x17, 0x1A, 0x1F)
#define CO_LINENO    RGB(0x60, 0x6A, 0x76)
#define CO_LINENO_ON RGB(0xC8, 0xD2, 0xDC)
#define CO_PLAIN     RGB(0xDC, 0xE4, 0xEC)
#define CO_KEYWORD   RGB(0xC7, 0x9B, 0xF0)
#define CO_STRING    RGB(0x8F, 0xD4, 0x8A)
#define CO_NUMBER    RGB(0xF0, 0xA5, 0x5E)
#define CO_COMMENT   RGB(0x6B, 0x7A, 0x88)
#define CO_BUILTIN   RGB(0x7F, 0xC8, 0xEE)
#define CO_CARET     RGB(0xF5, 0xC1, 0x4E)
#define CO_CURLINE   RGB(0x26, 0x2C, 0x34)
#define CO_OUT_BG    RGB(0x14, 0x17, 0x1B)
#define CO_OUT_TEXT  RGB(0xC0, 0xCC, 0xD6)
#define CO_OUT_ERR   RGB(0xF0, 0x8A, 0x7A)

enum co_button { CO_OPEN, CO_SAVE, CO_RUN, CO_CLEAR, CO_COUNT };

struct co_state {
    struct fs_node *file;

    char   *text;
    size_t  len;
    size_t  cap;

    size_t  cursor;
    size_t  anchor;
    bool    selecting;
    int32_t scroll;
    bool    modified;
    bool    caret_on;
    int     pressed;

    int32_t line_start[CO_MAX_LINES];
    int32_t line_count;

    /* Ausgabe des letzten Laufs. */
    char   *out;
    size_t  out_len;
    int32_t out_scroll;
    bool    out_failed;
    int32_t out_height;         /* Hoehe des unteren Bereichs */
    bool    dragging_split;

    uint32_t run_ms;
};

/* ------------------------------------------------------------------ */
/* Puffer und Zeilen                                                   */
/* ------------------------------------------------------------------ */

static void co_recount(struct co_state *st)
{
    st->line_count = 0;
    st->line_start[st->line_count++] = 0;

    for (size_t i = 0; i < st->len && st->line_count < CO_MAX_LINES; i++) {
        if (st->text[i] == '\n')
            st->line_start[st->line_count++] = (int32_t)i + 1;
    }
}

static int32_t co_line_of(struct co_state *st, size_t pos)
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

static int32_t co_line_length(struct co_state *st, int32_t line)
{
    int32_t start = st->line_start[line];
    int32_t end   = (line + 1 < st->line_count) ? st->line_start[line + 1] - 1
                                                : (int32_t)st->len;
    return MAX(end - start, 0);
}

static bool co_reserve(struct co_state *st, size_t need)
{
    if (st->cap >= need)
        return true;

    size_t cap = st->cap ? st->cap : 512;

    while (cap < need)
        cap *= 2;

    char *buf = krealloc(st->text, cap);

    if (!buf)
        return false;

    st->text = buf;
    st->cap  = cap;
    return true;
}

static void co_insert(struct co_state *st, char c)
{
    if (!co_reserve(st, st->len + 2))
        return;

    memmove(&st->text[st->cursor + 1], &st->text[st->cursor],
            st->len - st->cursor + 1);
    st->text[st->cursor++] = c;
    st->len++;
    st->text[st->len] = '\0';
    st->modified = true;
    co_recount(st);
}

static void co_erase(struct co_state *st, size_t pos)
{
    if (pos >= st->len)
        return;

    memmove(&st->text[pos], &st->text[pos + 1], st->len - pos);
    st->len--;
    st->modified = true;
    co_recount(st);
}

static bool co_has_selection(struct co_state *st)
{
    return st->selecting && st->anchor != st->cursor;
}

static void co_delete_selection(struct co_state *st)
{
    if (!co_has_selection(st))
        return;

    size_t from = MIN(st->anchor, st->cursor);
    size_t to   = MAX(st->anchor, st->cursor);

    memmove(&st->text[from], &st->text[to], st->len - to + 1);
    st->len -= to - from;
    st->cursor = from;
    st->selecting = false;
    st->modified = true;
    co_recount(st);
}

/* ------------------------------------------------------------------ */
/* Aufteilung des Fensters                                             */
/* ------------------------------------------------------------------ */

static int32_t co_out_height(struct window *win)
{
    struct co_state *st = win->user;
    int32_t room = gui_client_height(win) - CO_TOOLBAR_H - CO_STATUS_H;

    return CLAMP(st->out_height, CO_OUT_MIN, MAX(room - 80, CO_OUT_MIN));
}

static struct rect co_text_rect(struct window *win)
{
    int32_t top = CO_TOOLBAR_H;
    int32_t h = gui_client_height(win) - CO_TOOLBAR_H - CO_STATUS_H
                - co_out_height(win) - 4;

    return rect_make(0, top, gui_client_width(win) - SCROLLBAR_WIDTH,
                     MAX(h, CO_LINE_H));
}

static struct rect co_split_rect(struct window *win)
{
    struct rect t = co_text_rect(win);

    return rect_make(0, t.y + t.h, gui_client_width(win), 4);
}

static struct rect co_out_rect(struct window *win)
{
    struct rect s = co_split_rect(win);

    return rect_make(0, s.y + s.h, gui_client_width(win),
                     co_out_height(win));
}

static int32_t co_visible_lines(struct window *win)
{
    return MAX(co_text_rect(win).h / CO_LINE_H, 1);
}

static int32_t co_columns(struct window *win)
{
    return MAX((co_text_rect(win).w - CO_GUTTER_W - 2 * CO_MARGIN) / FONT_WIDTH,
               8);
}

static void co_ensure_visible(struct window *win, struct co_state *st)
{
    int32_t line = co_line_of(st, st->cursor);
    int32_t rows = co_visible_lines(win);

    if (line < st->scroll)
        st->scroll = line;
    else if (line >= st->scroll + rows)
        st->scroll = line - rows + 1;
}

/* ------------------------------------------------------------------ */
/* Hervorhebung                                                        */
/* ------------------------------------------------------------------ */

static bool word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$';
}

static bool word_is(const char *text, size_t start, size_t end,
                    const char *word)
{
    size_t n = strlen(word);

    if (end - start != n)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (text[start + i] != word[i])
            return false;
    }
    return true;
}

static uint32_t word_color(const char *text, size_t start, size_t end)
{
    static const char *keywords[] = {
        "var", "let", "const", "function", "return", "if", "else", "for",
        "while", "do", "break", "continue", "new", "delete", "typeof",
        "this", "true", "false", "null", "undefined", "in", "of", "switch",
        "case", "default", "throw", "try", "catch", "finally",
    };
    static const char *builtins[] = {
        "console", "Math", "String", "Number", "Array", "Object", "JSON",
        "document", "window", "parseInt", "parseFloat", "length",
        "setTimeout", "setInterval",
    };

    for (size_t i = 0; i < ARRAY_LEN(keywords); i++) {
        if (word_is(text, start, end, keywords[i]))
            return CO_KEYWORD;
    }
    for (size_t i = 0; i < ARRAY_LEN(builtins); i++) {
        if (word_is(text, start, end, builtins[i]))
            return CO_BUILTIN;
    }
    return CO_PLAIN;
}

/* Der Zustand laeuft ueber Zeilengrenzen hinweg - ein Blockkommentar
 * hoert ja nicht am Zeilenende auf. */
enum scan_state {
    SCAN_CODE,
    SCAN_BLOCK_COMMENT,
};

/* Faerbt eine Zeile ein. Der Zustand kommt herein und geht veraendert
 * wieder hinaus. */
static void colorize(const char *text, size_t start, size_t end,
                     uint32_t *colors, enum scan_state *state)
{
    size_t i = start;

    while (i < end) {
        size_t at = i - start;

        if (*state == SCAN_BLOCK_COMMENT) {
            colors[at] = CO_COMMENT;
            if (text[i] == '*' && i + 1 < end && text[i + 1] == '/') {
                colors[at + 1] = CO_COMMENT;
                *state = SCAN_CODE;
                i += 2;
                continue;
            }
            i++;
            continue;
        }

        /* Zeilenkommentar - der Rest gehoert ihm. */
        if (text[i] == '/' && i + 1 < end && text[i + 1] == '/') {
            for (size_t k = i; k < end; k++)
                colors[k - start] = CO_COMMENT;
            return;
        }
        if (text[i] == '/' && i + 1 < end && text[i + 1] == '*') {
            colors[at] = CO_COMMENT;
            colors[at + 1] = CO_COMMENT;
            *state = SCAN_BLOCK_COMMENT;
            i += 2;
            continue;
        }

        if (text[i] == '"' || text[i] == '\'') {
            char quote = text[i];

            colors[at] = CO_STRING;
            i++;
            while (i < end) {
                colors[i - start] = CO_STRING;
                if (text[i] == '\\' && i + 1 < end) {
                    colors[i + 1 - start] = CO_STRING;
                    i += 2;
                    continue;
                }
                if (text[i] == quote) {
                    i++;
                    break;
                }
                i++;
            }
            continue;
        }

        if (text[i] >= '0' && text[i] <= '9') {
            while (i < end && ((text[i] >= '0' && text[i] <= '9') ||
                               text[i] == '.' || text[i] == 'x' ||
                               (text[i] >= 'a' && text[i] <= 'f') ||
                               (text[i] >= 'A' && text[i] <= 'F'))) {
                colors[i - start] = CO_NUMBER;
                i++;
            }
            continue;
        }

        if (word_char(text[i])) {
            size_t word_start = i;

            while (i < end && word_char(text[i]))
                i++;

            uint32_t color = word_color(text, word_start, i);

            for (size_t k = word_start; k < i; k++)
                colors[k - start] = color;
            continue;
        }

        colors[at] = CO_PLAIN;
        i++;
    }
}

/* Der Zustand am Anfang einer Zeile: Dafuer muss der Text bis dorthin
 * einmal durchlaufen werden. Bei diesen Dateigroessen ist das im
 * Zeichnen nicht zu merken. */
static enum scan_state state_at_line(struct co_state *st, int32_t line)
{
    enum scan_state state = SCAN_CODE;
    size_t stop = (size_t)st->line_start[line];

    for (size_t i = 0; i + 1 < stop; i++) {
        if (state == SCAN_BLOCK_COMMENT) {
            if (st->text[i] == '*' && st->text[i + 1] == '/') {
                state = SCAN_CODE;
                i++;
            }
            continue;
        }
        if (st->text[i] == '/' && st->text[i + 1] == '*') {
            state = SCAN_BLOCK_COMMENT;
            i++;
        } else if (st->text[i] == '/' && st->text[i + 1] == '/') {
            while (i < stop && st->text[i] != '\n')
                i++;
        } else if (st->text[i] == '"' || st->text[i] == '\'') {
            char quote = st->text[i++];

            while (i < stop && st->text[i] != quote && st->text[i] != '\n') {
                if (st->text[i] == '\\')
                    i++;
                i++;
            }
        }
    }
    return state;
}

/* ------------------------------------------------------------------ */
/* Ausfuehren                                                          */
/* ------------------------------------------------------------------ */

static void co_out_clear(struct co_state *st)
{
    if (st->out)
        st->out[0] = '\0';
    st->out_len = 0;
    st->out_scroll = 0;
    st->out_failed = false;
}

static void co_out_add(struct co_state *st, const char *text)
{
    if (!st->out || !text)
        return;

    size_t n = strlen(text);

    if (st->out_len + n + 1 >= CO_OUT_MAX)
        n = CO_OUT_MAX - st->out_len - 1;

    memcpy(st->out + st->out_len, text, n);
    st->out_len += n;
    st->out[st->out_len] = '\0';
}

static void co_run(struct window *win)
{
    struct co_state *st = win->user;

    co_out_clear(st);

    if (st->len == 0) {
        co_out_add(st, tr("Es steht noch nichts da.\n"));
        gui_invalidate();
        return;
    }

    struct js_context *ctx = js_create();

    if (!ctx) {
        st->out_failed = true;
        co_out_add(st, "Der Deuter liess sich nicht anlegen - zu wenig "
                       "Speicher.\n");
        gui_invalidate();
        return;
    }

    uint64_t began = timer_ms();
    bool ok = js_run(ctx, st->text, st->len);

    st->run_ms = (uint32_t)(timer_ms() - began);

    const char *console = js_console(ctx);

    if (console && console[0])
        co_out_add(st, console);

    if (!ok) {
        const char *why = js_error(ctx);

        st->out_failed = true;
        if (st->out_len > 0 && st->out[st->out_len - 1] != '\n')
            co_out_add(st, "\n");
        co_out_add(st, tr("Fehler: "));
        co_out_add(st, why && why[0] ? why : tr("unbekannt"));
        co_out_add(st, "\n");
    } else if (st->out_len == 0) {
        co_out_add(st, "Durchgelaufen - aber ohne Ausgabe. "
                       "console.log() sagt etwas.\n");
    }

    /* Der Deuter haelt seinen Speicher am Stueck; ihn wegzuwerfen gibt
     * alles auf einmal frei. */
    js_destroy(ctx);

    st->out_scroll = 0;
    gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Dateien                                                             */
/* ------------------------------------------------------------------ */

static void co_update_title(struct window *win, struct co_state *st)
{
    char title[WIN_TITLE_MAX + 1];

    ksnprintf(title, sizeof(title), tr("Programmieren - %s%s"),
              st->file ? st->file->name : tr("Unbenannt"),
              st->modified ? " *" : "");
    gui_set_title(win, title);
}

/* Wo neue Programme hingehoeren. */
static struct fs_node *code_dir(void)
{
    struct fs_node *base = fs_disk_root() ? fs_disk_root() : fs_root();
    struct fs_node *dir = fs_find_child(base, tr("Programme"));

    if (dir && dir->type == FS_DIR)
        return dir;
    return base;
}

static void on_save_as(const char *name, void *user)
{
    struct window *win = user;

    if (!gui_window_alive(win) || !name || !name[0])
        return;

    struct co_state *st = win->user;
    struct fs_node *dir = code_dir();
    struct fs_node *file = fs_find_child(dir, name);

    if (!file)
        file = fs_create(dir, name, FS_FILE);

    if (!file || file->type != FS_FILE || file->readonly) {
        dialog_message(tr("Speichern"), tr("Unter diesem Namen geht es nicht."));
        return;
    }
    if (!fs_write(file, st->text, st->len)) {
        dialog_message(tr("Speichern"), tr("Die Datei liess sich nicht schreiben."));
        return;
    }

    st->file = file;
    st->modified = false;
    co_update_title(win, st);
    gui_invalidate();
}

static void co_save(struct window *win)
{
    struct co_state *st = win->user;

    if (st->file && !fs_node_alive(st->file))
        st->file = NULL;

    if (!st->file || st->file->readonly) {
        dialog_input(tr("Speichern unter"), tr("Dateiname:"),
                     st->file ? st->file->name : "programm.js",
                     on_save_as, win);
        return;
    }

    if (!fs_write(st->file, st->text, st->len)) {
        dialog_message(tr("Speichern"), tr("Die Datei liess sich nicht schreiben."));
        return;
    }

    st->modified = false;
    co_update_title(win, st);
    gui_invalidate();
}

static void co_set_text(struct window *win, const char *text, size_t len);

static void on_open(const char *path, void *user)
{
    struct window *win = user;

    if (!gui_window_alive(win) || !path || !path[0])
        return;

    struct fs_node *file = fs_lookup(fs_root(), path);

    if (!file) {
        struct fs_node *dir = code_dir();

        file = fs_find_child(dir, path);
    }

    if (!file || file->type != FS_FILE || !fs_load(file)) {
        dialog_message(tr("Oeffnen"), tr("Diese Datei gibt es nicht."));
        return;
    }

    struct co_state *st = win->user;

    co_set_text(win, (const char *)file->data, file->size);
    st->file = file;
    st->modified = false;
    co_out_clear(st);
    co_update_title(win, st);
    gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static struct rect co_button_rect(int index)
{
    static const int32_t widths[CO_COUNT] = { 92, 104, 116, 92 };
    int32_t x = 4;

    for (int i = 0; i < index; i++)
        x += widths[i] + 3;

    return rect_make(x, 4, widths[index], CO_TOOLBAR_H - 8);
}

static void paint_code(struct window *win, struct canvas *c)
{
    struct co_state *st = win->user;
    struct rect area = co_text_rect(win);

    gfx_fill(c, area, CO_BG);
    gfx_fill(c, rect_make(area.x, area.y, CO_GUTTER_W, area.h), CO_GUTTER);
    gfx_vline(c, area.x + CO_GUTTER_W, area.y, area.h, RGB(0x30, 0x36, 0x3E));

    struct canvas text = *c;

    gfx_set_clip(&text, rect_intersect(c->clip, area));

    int32_t rows = co_visible_lines(win);
    int32_t cols = co_columns(win);
    int32_t cur_line = co_line_of(st, st->cursor);
    int32_t cur_col  = (int32_t)st->cursor - st->line_start[cur_line];

    bool   picked = co_has_selection(st);
    size_t sel_from = MIN(st->anchor, st->cursor);
    size_t sel_to   = MAX(st->anchor, st->cursor);

    enum scan_state state = state_at_line(st, MIN(st->scroll,
                                                  st->line_count - 1));
    uint32_t colors[512];

    for (int32_t i = 0; i < rows; i++) {
        int32_t line = st->scroll + i;

        if (line >= st->line_count)
            break;

        int32_t start = st->line_start[line];
        int32_t len   = co_line_length(st, line);
        int32_t y     = area.y + 2 + i * CO_LINE_H;
        int32_t shown = MIN(len, MIN(cols, (int32_t)ARRAY_LEN(colors)));

        if (line == cur_line)
            gfx_fill(&text, rect_make(area.x + CO_GUTTER_W + 1, y - 2,
                                      area.w - CO_GUTTER_W - 1, CO_LINE_H),
                     CO_CURLINE);

        char number[8];

        ksnprintf(number, sizeof(number), "%d", line + 1);
        gfx_text(&text,
                 area.x + CO_GUTTER_W - 6 - gfx_text_width(number), y,
                 number, line == cur_line ? CO_LINENO_ON : CO_LINENO);

        for (int32_t k = 0; k < shown; k++)
            colors[k] = CO_PLAIN;
        colorize(st->text, (size_t)start, (size_t)(start + shown), colors,
                 &state);

        /* Der Zustand muss ueber die ganze Zeile laufen, auch wenn nur
         * ein Teil sichtbar ist. */
        if (shown < len) {
            uint32_t rest[512];
            enum scan_state after = state;

            for (int32_t k = 0; k < MIN(len - shown, 512); k++)
                rest[k] = CO_PLAIN;
            colorize(st->text, (size_t)(start + shown),
                     (size_t)(start + MIN(len, shown + 512)), rest, &after);
            state = after;
        }

        for (int32_t k = 0; k < shown; k++) {
            size_t at = (size_t)(start + k);
            int32_t cx = area.x + CO_GUTTER_W + CO_MARGIN + k * FONT_WIDTH;
            bool in_sel = picked && at >= sel_from && at < sel_to;

            if (in_sel)
                gfx_fill(&text, rect_make(cx, y, FONT_WIDTH, FONT_HEIGHT),
                         COL_SELECT);
            gfx_char(&text, cx, y, (unsigned char)st->text[at],
                     in_sel ? COL_SELECT_TEXT : colors[k], false);
        }

        if (line == cur_line && st->caret_on) {
            int32_t cx = area.x + CO_GUTTER_W + CO_MARGIN +
                         MIN(cur_col, cols) * FONT_WIDTH;

            gfx_fill(&text, rect_make(cx, y, 2, FONT_HEIGHT), CO_CARET);
        }
    }

    widget_vscroll(c, rect_make(area.x + area.w, area.y,
                                SCROLLBAR_WIDTH, area.h),
                   st->scroll, st->line_count, rows);
}

static int32_t out_lines(struct co_state *st)
{
    int32_t n = 1;

    for (size_t i = 0; i < st->out_len; i++) {
        if (st->out[i] == '\n')
            n++;
    }
    return n;
}

static void paint_output(struct window *win, struct canvas *c)
{
    struct co_state *st = win->user;
    struct rect area = co_out_rect(win);
    struct rect split = co_split_rect(win);

    gfx_fill(c, split, COL_FACE);
    gfx_bevel_thin(c, split, true);

    gfx_fill(c, area, CO_OUT_BG);

    struct canvas text = *c;

    gfx_set_clip(&text, rect_intersect(c->clip, area));

    if (st->out_len == 0) {
        gfx_text(&text, area.x + CO_MARGIN, area.y + 4,
                 tr("Ausgabe erscheint hier. F5 fuehrt aus."), CO_LINENO);
        return;
    }

    int32_t rows = MAX(area.h / CO_LINE_H, 1);
    int32_t line = 0;
    int32_t y = area.y + 4;
    size_t at = 0;

    while (at < st->out_len && line < st->out_scroll + rows) {
        char row[256];
        size_t n = 0;

        while (at < st->out_len && st->out[at] != '\n' && n + 1 < sizeof(row))
            row[n++] = st->out[at++];
        row[n] = '\0';
        if (at < st->out_len && st->out[at] == '\n')
            at++;

        if (line >= st->out_scroll) {
            gfx_text(&text, area.x + CO_MARGIN, y, row,
                     st->out_failed ? CO_OUT_ERR : CO_OUT_TEXT);
            y += CO_LINE_H;
        }
        line++;
    }
}

static void co_paint(struct window *win, struct canvas *c)
{
    struct co_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);
    widget_toolbar(&local, rect_make(0, 0, local.w, CO_TOOLBAR_H));

    widget_icon_button(&local, co_button_rect(CO_OPEN), ICON_FOLDER_OPEN,
                       tr("Oeffnen"), st->pressed == CO_OPEN, true);
    widget_icon_button(&local, co_button_rect(CO_SAVE), ICON_SAVE,
                       tr("Speichern"), st->pressed == CO_SAVE, true);
    widget_icon_button(&local, co_button_rect(CO_RUN), ICON_PLAY,
                       tr("Ausfuehren"), st->pressed == CO_RUN, st->len > 0);
    widget_icon_button(&local, co_button_rect(CO_CLEAR), ICON_TRASH,
                       tr("Leeren"), st->pressed == CO_CLEAR, st->out_len > 0);

    paint_code(win, &local);
    paint_output(win, &local);

    int32_t cur_line = co_line_of(st, st->cursor);
    int32_t cur_col  = (int32_t)st->cursor - st->line_start[cur_line];
    char left[96], right[64];

    ksnprintf(left, sizeof(left), tr("Zeile %d von %d, Spalte %d"),
              cur_line + 1, st->line_count, cur_col + 1);
    if (st->run_ms)
        ksnprintf(right, sizeof(right), "%s in %u ms",
                  st->out_failed ? tr("abgebrochen") : tr("gelaufen"),
                  (unsigned)st->run_ms);
    else
        ksnprintf(right, sizeof(right), tr("F5 fuehrt aus"));

    widget_statusbar(&local, rect_make(0, local.h - CO_STATUS_H,
                                       local.w, CO_STATUS_H), left, right);
}

/* ------------------------------------------------------------------ */
/* Eingabe                                                             */
/* ------------------------------------------------------------------ */

static void co_click_text(struct window *win, int32_t x, int32_t y)
{
    struct co_state *st = win->user;
    struct rect area = co_text_rect(win);

    int32_t row  = (y - area.y - 2) / CO_LINE_H;
    int32_t line = CLAMP(st->scroll + row, 0, st->line_count - 1);
    int32_t col  = (x - area.x - CO_GUTTER_W - CO_MARGIN + FONT_WIDTH / 2)
                   / FONT_WIDTH;

    col = CLAMP(col, 0, co_line_length(st, line));
    st->cursor = (size_t)(st->line_start[line] + col);
    st->caret_on = true;
    gui_invalidate();
}

static void co_key(struct window *win, const struct gui_event *ev)
{
    struct co_state *st = win->user;
    int32_t line = co_line_of(st, st->cursor);
    int32_t col  = (int32_t)st->cursor - st->line_start[line];
    bool shifted = (ev->mods & MOD_SHIFT) != 0;

    if (ev->key == KEY_F5) {
        co_run(win);
        return;
    }

    if (ev->mods & MOD_CTRL) {
        switch (ev->ascii) {
        case 's': case 'S':
            co_save(win);
            return;
        case 'r': case 'R':
            co_run(win);
            return;
        case 'c': case 'C':
            if (co_has_selection(st)) {
                size_t from = MIN(st->anchor, st->cursor);

                clipboard_set(st->text + from,
                              MAX(st->anchor, st->cursor) - from);
            }
            return;
        case 'v': case 'V': {
            size_t bytes = 0;
            const char *paste = clipboard_get(&bytes);

            if (paste) {
                co_delete_selection(st);
                for (size_t i = 0; i < bytes && paste[i]; i++) {
                    if (paste[i] == '\r')
                        continue;
                    co_insert(st, paste[i]);
                }
                co_ensure_visible(win, st);
                co_update_title(win, st);
                gui_invalidate();
            }
            return;
        }
        case 'a': case 'A':
            st->anchor = 0;
            st->cursor = st->len;
            st->selecting = true;
            gui_invalidate();
            return;
        default:
            break;
        }
    }

    bool moves = ev->key == KEY_LEFT || ev->key == KEY_RIGHT ||
                 ev->key == KEY_UP || ev->key == KEY_DOWN ||
                 ev->key == KEY_HOME || ev->key == KEY_END ||
                 ev->key == KEY_PAGEUP || ev->key == KEY_PAGEDOWN;

    if (moves && shifted && !st->selecting) {
        st->anchor = st->cursor;
        st->selecting = true;
    } else if (moves && !shifted) {
        st->selecting = false;
    }

    if (!moves && co_has_selection(st)) {
        bool replaces = ev->key == KEY_BACKSPACE || ev->key == KEY_DELETE ||
                        ev->key == KEY_ENTER || ev->key == KEY_TAB ||
                        (ev->ascii >= 32 && (unsigned char)ev->ascii != 127);

        if (replaces) {
            co_delete_selection(st);
            line = co_line_of(st, st->cursor);
            col  = (int32_t)st->cursor - st->line_start[line];

            if (ev->key == KEY_BACKSPACE || ev->key == KEY_DELETE) {
                st->caret_on = true;
                co_ensure_visible(win, st);
                co_update_title(win, st);
                gui_invalidate();
                return;
            }
        }
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
        if (line > 0)
            st->cursor = (size_t)(st->line_start[line - 1] +
                                  MIN(col, co_line_length(st, line - 1)));
        break;
    case KEY_DOWN:
        if (line + 1 < st->line_count)
            st->cursor = (size_t)(st->line_start[line + 1] +
                                  MIN(col, co_line_length(st, line + 1)));
        break;
    case KEY_HOME:
        st->cursor = (size_t)st->line_start[line];
        break;
    case KEY_END:
        st->cursor = (size_t)(st->line_start[line] +
                              co_line_length(st, line));
        break;
    case KEY_PAGEUP:
        st->cursor = (size_t)st->line_start[MAX(line - co_visible_lines(win), 0)];
        break;
    case KEY_PAGEDOWN:
        st->cursor = (size_t)st->line_start[MIN(line + co_visible_lines(win),
                                                st->line_count - 1)];
        break;
    case KEY_BACKSPACE:
        if (st->cursor > 0) {
            st->cursor--;
            co_erase(st, st->cursor);
        }
        break;
    case KEY_DELETE:
        co_erase(st, st->cursor);
        break;
    case KEY_ENTER: {
        /* Die Einrueckung der Zeile mitnehmen - das ist beim
         * Programmieren die Regel, nicht die Ausnahme. */
        int32_t indent = 0;
        int32_t at = st->line_start[line];

        while (at < (int32_t)st->len && st->text[at] == ' ') {
            indent++;
            at++;
        }
        co_insert(st, '\n');
        for (int32_t i = 0; i < indent; i++)
            co_insert(st, ' ');
        break;
    }
    case KEY_TAB:
        for (int i = 0; i < 2; i++)
            co_insert(st, ' ');
        break;
    default:
        if (ev->ascii >= 32 && (unsigned char)ev->ascii != 127)
            co_insert(st, ev->ascii);
        else
            return;
        break;
    }

    st->caret_on = true;
    co_ensure_visible(win, st);
    co_update_title(win, st);
    gui_invalidate();
}

static void co_action(struct window *win, int action)
{
    struct co_state *st = win->user;

    switch (action) {
    case CO_OPEN:
        dialog_input(tr("Oeffnen"), tr("Datei:"), "programm.js", on_open, win);
        break;
    case CO_SAVE:
        co_save(win);
        break;
    case CO_RUN:
        co_run(win);
        break;
    case CO_CLEAR:
        co_out_clear(st);
        st->run_ms = 0;
        gui_invalidate();
        break;
    }
}

static void co_event(struct window *win, const struct gui_event *ev)
{
    struct co_state *st = win->user;

    switch (ev->type) {
    case EV_MOUSE_DOWN: {
        struct rect area = co_text_rect(win);

        if (ev->y < CO_TOOLBAR_H) {
            for (int i = 0; i < CO_COUNT; i++) {
                if (rect_contains(co_button_rect(i), ev->x, ev->y)) {
                    st->pressed = i;
                    gui_invalidate();
                    break;
                }
            }
        } else if (rect_contains(co_split_rect(win), ev->x, ev->y)) {
            st->dragging_split = true;
        } else if (ev->x >= area.x + area.w && ev->y < area.y + area.h) {
            st->scroll = widget_vscroll_click(
                rect_make(area.x + area.w, area.y, SCROLLBAR_WIDTH, area.h),
                ev->y, st->scroll, st->line_count, co_visible_lines(win));
            gui_invalidate();
        } else if (rect_contains(area, ev->x, ev->y)) {
            st->selecting = false;
            co_click_text(win, ev->x, ev->y);
            st->anchor = st->cursor;
        }
        break;
    }

    case EV_MOUSE_DRAG:
        if (st->dragging_split) {
            int32_t bottom = gui_client_height(win) - CO_STATUS_H;

            st->out_height = bottom - ev->y;
            gui_invalidate();
        } else if (rect_contains(co_text_rect(win), ev->x, ev->y)) {
            if (!st->selecting) {
                st->anchor = st->cursor;
                st->selecting = true;
            }
            co_click_text(win, ev->x, ev->y);
        }
        break;

    case EV_MOUSE_UP: {
        int pressed = st->pressed;

        st->pressed = -1;
        st->dragging_split = false;
        if (pressed >= 0 && rect_contains(co_button_rect(pressed),
                                          ev->x, ev->y))
            co_action(win, pressed);
        gui_invalidate();
        break;
    }

    case EV_SCROLL:
        if (rect_contains(co_out_rect(win), ev->x, ev->y)) {
            int32_t rows = MAX(co_out_rect(win).h / CO_LINE_H, 1);

            st->out_scroll = CLAMP(st->out_scroll - ev->scroll * 3, 0,
                                   MAX(out_lines(st) - rows, 0));
        } else {
            st->scroll = CLAMP(st->scroll - ev->scroll * 3, 0,
                               MAX(st->line_count - co_visible_lines(win), 0));
        }
        gui_invalidate();
        break;

    case EV_KEY_DOWN:
        co_key(win, ev);
        break;

    case EV_TICK:
        st->caret_on = !st->caret_on;
        gui_invalidate();
        break;

    case EV_RESIZED:
        co_ensure_visible(win, st);
        gui_invalidate();
        break;

    default:
        break;
    }
}

static void co_close(struct window *win)
{
    struct co_state *st = win->user;

    if (st) {
        kfree(st->text);
        kfree(st->out);
        kfree(st);
    }
    win->user = NULL;
}

static void co_set_text(struct window *win, const char *text, size_t len)
{
    struct co_state *st = win->user;

    if (!co_reserve(st, len + 256))
        return;

    if (text && len)
        memcpy(st->text, text, len);
    st->len = len;
    st->text[len] = '\0';
    st->cursor = 0;
    st->anchor = 0;
    st->selecting = false;
    st->scroll = 0;
    co_recount(st);
}

/* Was im Fenster steht, wenn noch nichts geladen wurde. Ein Beispiel
 * ist die schnellste Erklaerung. */
static const char beispiel[] =
    "// Willkommen. F5 fuehrt aus, die Ausgabe steht unten.\n"
    "\n"
    "function fakultaet(n) {\n"
    "  var ergebnis = 1;\n"
    "  for (var i = 2; i <= n; i++) {\n"
    "    ergebnis = ergebnis * i;\n"
    "  }\n"
    "  return ergebnis;\n"
    "}\n"
    "\n"
    "for (var i = 1; i <= 8; i++) {\n"
    "  console.log(i + \"! = \" + fakultaet(i));\n"
    "}\n"
    "\n"
    "var namen = [\"Ada\", \"Grace\", \"Alan\"];\n"
    "console.log(\"Insgesamt \" + namen.length + \" Namen.\");\n";

/* Legt ein Fenster an. file darf NULL sein - dann steht das Beispiel
 * darin. */
static void code_new(struct fs_node *file)
{
    struct co_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    static int32_t cascade;
    int32_t offset = (cascade++ % 5) * 24;

    struct window *win = gui_create_window("Programmieren",
                                           120 + offset, 70 + offset,
                                           660, 480, WF_RESIZABLE, ICON_CODE);
    if (!win) {
        kfree(st);
        return;
    }

    st->pressed = -1;
    st->caret_on = true;
    st->out_height = 130;
    st->out = kzalloc(CO_OUT_MAX);

    win->user     = st;
    win->on_paint = co_paint;
    win->on_event = co_event;
    win->on_close = co_close;
    win->min_w    = 460;
    win->min_h    = 300;

    if (!st->out || !co_reserve(st, sizeof(beispiel) + 256)) {
        gui_close_window(win);
        return;
    }

    if (file && fs_load(file)) {
        co_set_text(win, (const char *)file->data, file->size);
        st->file = file;
    } else {
        co_set_text(win, beispiel, sizeof(beispiel) - 1);
    }

    co_update_title(win, st);
    gui_focus_window(win);
}

void code_open(struct fs_node *file)
{
    /* Schon offen? Dann nur nach vorne holen. */
    for (size_t i = 0; i < gui_window_count(); i++) {
        struct window *win = gui_window_at(i);

        if (win->on_paint == co_paint) {
            struct co_state *st = win->user;

            if (st && st->file == file) {
                gui_focus_window(win);
                return;
            }
        }
    }

    code_new(file);
}

void app_code(void)
{
    code_new(NULL);
}
