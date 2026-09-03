/* writeapp.c - das Fenster der Textverarbeitung.
 *
 * Der Text steht als Folge von Absaetzen in writedoc.c; hier wird er
 * umbrochen, gezeichnet und bearbeitet. Der Umbruch entsteht bei jeder
 * Aenderung neu und liegt als Liste sichtbarer Zeilen bereit - daraus
 * ergeben sich Zeichnen, Mausklick und die Bewegung mit den
 * Pfeiltasten, alle drei aus derselben Quelle.
 *
 * Die Schreibmarke steht zwischen zwei Zeichen und wird als Paar aus
 * Absatz und Stelle gefuehrt. Eine Auswahl ist ein zweites solches
 * Paar; was dazwischenliegt, laesst sich fett oder unterstrichen
 * machen oder in einem Zug ersetzen.
 */

#include "apps.h"
#include "clipboard.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"
#include "theme.h"
#include "widgets.h"
#include "writedoc.h"
#include "lang.h"

#define WA_TOOLBAR_H 34
#define WA_STATUS_H  24
#define WA_MARGIN    28
#define WA_PAGE_W    620
#define WA_LINES_MAX 2000

#define WA_PAPER     RGB(0xFF, 0xFF, 0xFF)
#define WA_DESK      RGB(0x86, 0x8E, 0x96)
#define WA_QUOTE     RGB(0x50, 0x58, 0x60)
#define WA_RULE      RGB(0xD0, 0xD4, 0xD8)

enum wa_button {
    WB_OPEN, WB_SAVE, WB_STYLE, WB_LIST,
    WB_BOLD, WB_UNDER, WB_LEFT, WB_MID, WB_RIGHT, WB_COUNT
};

/* Eine Zeile, wie sie auf dem Papier steht. */
struct vline {
    int16_t para;
    int16_t start;
    int16_t len;
    int16_t y;
    int16_t height;
};

struct wa_state {
    struct wdoc    *doc;
    struct fs_node *file;
    char            title[64];
    bool            modified;

    int  para, at;              /* Schreibmarke      */
    int  anchor_para, anchor_at;
    bool selecting;

    uint8_t marks;              /* was jetzt getippt wird */

    struct vline *lines;
    int           line_count;
    bool          needs_layout;
    int32_t       layout_width;

    int32_t scroll;             /* in Punkten */
    int32_t height;             /* Hoehe des gesetzten Textes */

    bool caret_on;
    int  pressed;
};

/* ------------------------------------------------------------------ */
/* Masse der Formatvorlagen                                            */
/* ------------------------------------------------------------------ */

static int32_t style_scale(uint8_t style)
{
    return style == STYLE_H1 ? 2 : 1;
}

static bool style_bold(uint8_t style)
{
    return style == STYLE_H1 || style == STYLE_H2;
}

static int32_t style_indent(uint8_t style)
{
    if (style == STYLE_LIST)
        return 18;
    if (style == STYLE_QUOTE)
        return 24;
    return 0;
}

static int32_t style_char_w(uint8_t style)
{
    return FONT_WIDTH * style_scale(style);
}

static int32_t style_line_h(uint8_t style)
{
    switch (style) {
    case STYLE_H1: return FONT_HEIGHT * 2 + 8;
    case STYLE_H2: return FONT_HEIGHT + 8;
    default:       return FONT_HEIGHT + 4;
    }
}

/* Luft vor einem Absatz - Ueberschriften brauchen sie, damit der Text
 * sich gliedert. */
static int32_t style_space_before(uint8_t style, uint8_t previous, bool first)
{
    if (first)
        return 0;
    if (style == STYLE_H1)
        return 14;
    if (style == STYLE_H2)
        return 10;
    if (style == STYLE_LIST && previous == STYLE_LIST)
        return 0;
    return 6;
}

/* ------------------------------------------------------------------ */
/* Umbruch                                                             */
/* ------------------------------------------------------------------ */

static int32_t page_width(struct window *win)
{
    int32_t room = gui_client_width(win) - SCROLLBAR_WIDTH - 2 * WA_MARGIN;

    return CLAMP(room, 200, WA_PAGE_W);
}

static struct rect page_rect(struct window *win)
{
    return rect_make(0, WA_TOOLBAR_H,
                     gui_client_width(win) - SCROLLBAR_WIDTH,
                     gui_client_height(win) - WA_TOOLBAR_H - WA_STATUS_H);
}

static void relayout(struct window *win)
{
    struct wa_state *st = win->user;
    int32_t width = page_width(win);
    int32_t y = WA_MARGIN;

    st->line_count = 0;
    st->layout_width = width;
    st->needs_layout = false;

    for (int i = 0; i < st->doc->count; i++) {
        const struct paragraph *p = &st->doc->paras[i];
        int32_t indent = style_indent(p->style);
        int32_t char_w = style_char_w(p->style);
        int32_t line_h = style_line_h(p->style);
        int     cols = MAX((width - indent) / char_w, 4);

        y += style_space_before(p->style, i ? st->doc->paras[i - 1].style
                                            : STYLE_BODY, i == 0);

        int at = 0;

        do {
            if (st->line_count >= WA_LINES_MAX)
                break;

            int rest = p->len - at;
            int take = MIN(rest, cols);

            /* An einem Leerzeichen trennen, wenn die Zeile voll ist. */
            if (take < rest) {
                int cut = take;

                while (cut > 0 && p->text[at + cut] != ' ')
                    cut--;
                if (cut > 0)
                    take = cut;
            }

            struct vline *line = &st->lines[st->line_count++];

            line->para   = (int16_t)i;
            line->start  = (int16_t)at;
            line->len    = (int16_t)take;
            line->y      = (int16_t)y;
            line->height = (int16_t)line_h;

            y += line_h;
            at += take;

            /* Das trennende Leerzeichen gehoert nicht in die naechste
             * Zeile. */
            if (at < p->len && p->text[at] == ' ')
                at++;
        } while (at < p->len);
    }

    st->height = y + WA_MARGIN;
}

static int line_of(struct wa_state *st, int para, int at)
{
    int found = 0;

    for (int i = 0; i < st->line_count; i++) {
        const struct vline *line = &st->lines[i];

        if (line->para < para)
            continue;
        if (line->para > para)
            break;

        found = i;
        if (at <= line->start + line->len)
            return i;
    }
    return found;
}

static void ensure_visible(struct window *win)
{
    struct wa_state *st = win->user;

    if (st->needs_layout)
        relayout(win);
    if (st->line_count == 0)
        return;

    const struct vline *line = &st->lines[line_of(st, st->para, st->at)];
    struct rect area = page_rect(win);

    if (line->y - st->scroll < 4)
        st->scroll = line->y - 4;
    else if (line->y + line->height - st->scroll > area.h - 4)
        st->scroll = line->y + line->height - area.h + 4;

    st->scroll = CLAMP(st->scroll, 0, MAX(st->height - area.h, 0));
}

/* ------------------------------------------------------------------ */
/* Auswahl                                                             */
/* ------------------------------------------------------------------ */

static bool has_selection(struct wa_state *st)
{
    return st->selecting &&
           (st->anchor_para != st->para || st->anchor_at != st->at);
}

static void selection_range(struct wa_state *st, int *p1, int *a1,
                            int *p2, int *a2)
{
    bool anchor_first = st->anchor_para < st->para ||
                        (st->anchor_para == st->para &&
                         st->anchor_at <= st->at);

    *p1 = anchor_first ? st->anchor_para : st->para;
    *a1 = anchor_first ? st->anchor_at : st->at;
    *p2 = anchor_first ? st->para : st->anchor_para;
    *a2 = anchor_first ? st->at : st->anchor_at;
}

static bool in_selection(struct wa_state *st, int para, int at)
{
    if (!has_selection(st))
        return false;

    int p1, a1, p2, a2;

    selection_range(st, &p1, &a1, &p2, &a2);

    if (para < p1 || para > p2)
        return false;
    if (para == p1 && at < a1)
        return false;
    if (para == p2 && at >= a2)
        return false;
    return true;
}

static void delete_selection(struct window *win)
{
    struct wa_state *st = win->user;

    if (!has_selection(st))
        return;

    int p1, a1, p2, a2;

    selection_range(st, &p1, &a1, &p2, &a2);

    /* Von hinten nach vorn, damit die Nummern gueltig bleiben. */
    if (p1 == p2) {
        for (int i = a2 - 1; i >= a1; i--)
            wdoc_erase_char(st->doc, p1, i);
    } else {
        for (int i = st->doc->paras[p2].len - 1; i >= a2; i--)
            ;                       /* der Rest hinter a2 bleibt stehen */
        for (int i = a2 - 1; i >= 0; i--)
            wdoc_erase_char(st->doc, p2, i);

        for (int para = p2 - 1; para > p1; para--)
            wdoc_remove(st->doc, para);

        for (int i = st->doc->paras[p1].len - 1; i >= a1; i--)
            wdoc_erase_char(st->doc, p1, i);

        wdoc_join(st->doc, p1 + 1);
    }

    st->para = p1;
    st->at = a1;
    st->selecting = false;
    st->modified = true;
    st->needs_layout = true;
}

static void apply_marks(struct window *win, uint8_t mark)
{
    struct wa_state *st = win->user;

    if (!has_selection(st)) {
        /* Ohne Auswahl gilt es fuer das, was als naechstes getippt
         * wird. */
        st->marks ^= mark;
        gui_invalidate();
        return;
    }

    int p1, a1, p2, a2;

    selection_range(st, &p1, &a1, &p2, &a2);

    /* Setzen, wenn irgendein Zeichen die Auszeichnung noch nicht hat. */
    bool set = false;

    for (int para = p1; para <= p2 && !set; para++) {
        struct paragraph *p = &st->doc->paras[para];
        int from = para == p1 ? a1 : 0;
        int to = para == p2 ? a2 : p->len;

        for (int i = from; i < to && i < p->len; i++) {
            if (!(p->marks[i] & mark))
                set = true;
        }
    }

    for (int para = p1; para <= p2; para++) {
        struct paragraph *p = &st->doc->paras[para];
        int from = para == p1 ? a1 : 0;
        int to = para == p2 ? a2 : p->len;

        for (int i = from; i < to && i < p->len; i++) {
            if (set)
                p->marks[i] |= mark;
            else
                p->marks[i] &= (uint8_t)~mark;
        }
    }

    st->modified = true;
    gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Dateien                                                             */
/* ------------------------------------------------------------------ */

static void update_title(struct window *win)
{
    struct wa_state *st = win->user;
    char text[WIN_TITLE_MAX + 1];

    ksnprintf(text, sizeof(text), tr("Schreiben - %s%s"),
              st->file ? st->file->name : tr("Unbenannt"),
              st->modified ? " *" : "");
    gui_set_title(win, text);
}

static struct fs_node *docs_dir(void)
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

    struct wa_state *st = win->user;
    struct fs_node *dir = docs_dir();
    struct fs_node *file = fs_find_child(dir, name);

    if (!file)
        file = fs_create(dir, name, FS_FILE);
    if (!file || file->type != FS_FILE || file->readonly) {
        dialog_message(tr("Speichern"), tr("Unter diesem Namen geht es nicht."));
        return;
    }

    size_t room = 128 * 1024;
    char *html = kmalloc(room);

    if (!html) {
        dialog_message(tr("Speichern"), tr("Zu wenig Speicher."));
        return;
    }

    /* Der Titel ist die erste Ueberschrift, sonst der Dateiname. */
    const char *title = st->title[0] ? st->title : name;

    for (int i = 0; i < st->doc->count; i++) {
        if (st->doc->paras[i].style == STYLE_H1 &&
            st->doc->paras[i].len > 0) {
            title = st->doc->paras[i].text;
            break;
        }
    }

    size_t n = wdoc_to_html(st->doc, title, html, room);
    bool ok = fs_write(file, html, MIN(n, room - 1));

    kfree(html);

    if (!ok) {
        dialog_message(tr("Speichern"), tr("Die Datei liess sich nicht schreiben."));
        return;
    }

    st->file = file;
    st->modified = false;
    update_title(win);
    gui_invalidate();
}

static void do_save(struct window *win)
{
    struct wa_state *st = win->user;

    if (st->file && !fs_node_alive(st->file))
        st->file = NULL;

    if (!st->file || st->file->readonly) {
        dialog_input(tr("Speichern unter"), tr("Dateiname:"),
                     st->file ? st->file->name : "dokument.html",
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
        file = fs_find_child(docs_dir(), path);

    if (!file || file->type != FS_FILE || !fs_load(file)) {
        dialog_message(tr("Oeffnen"), tr("Diese Datei gibt es nicht."));
        return;
    }

    struct wa_state *st = win->user;

    wdoc_from_html(st->doc, (const char *)file->data, file->size,
                   st->title, sizeof(st->title));
    st->file = file;
    st->modified = false;
    st->para = st->at = 0;
    st->scroll = 0;
    st->selecting = false;
    st->needs_layout = true;
    update_title(win);
    gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static struct rect tool_rect(int index)
{
    static const int32_t widths[WB_COUNT] = {
        92, 104, 130, 96, 30, 30, 30, 30, 30
    };
    int32_t x = 4;

    for (int i = 0; i < index; i++)
        x += widths[i] + (i == WB_LIST || i == WB_UNDER ? 10 : 3);

    return rect_make(x, 4, widths[index], WA_TOOLBAR_H - 8);
}

static void paint_toolbar(struct window *win, struct canvas *c)
{
    struct wa_state *st = win->user;
    const struct paragraph *p = &st->doc->paras[st->para];

    widget_toolbar(c, rect_make(0, 0, c->w, WA_TOOLBAR_H));
    widget_icon_button(c, tool_rect(WB_OPEN), ICON_FOLDER_OPEN, tr("Oeffnen"),
                       st->pressed == WB_OPEN, true);
    widget_icon_button(c, tool_rect(WB_SAVE), ICON_SAVE, tr("Speichern"),
                       st->pressed == WB_SAVE, true);
    widget_icon_button(c, tool_rect(WB_STYLE), ICON_HEADING,
                       wdoc_style_name(p->style),
                       st->pressed == WB_STYLE, true);
    widget_icon_button(c, tool_rect(WB_LIST), ICON_LIST, tr("Liste"),
                       st->pressed == WB_LIST || p->style == STYLE_LIST, true);

    bool bold_on = has_selection(st) ? false : (st->marks & MARK_BOLD) != 0;
    bool under_on = has_selection(st) ? false : (st->marks & MARK_UNDERLINE) != 0;

    widget_icon_button(c, tool_rect(WB_BOLD), ICON_BOLD, NULL,
                       st->pressed == WB_BOLD || bold_on, true);
    widget_icon_button(c, tool_rect(WB_UNDER), ICON_UNDERLINE, NULL,
                       st->pressed == WB_UNDER || under_on, true);
    widget_icon_button(c, tool_rect(WB_LEFT), ICON_ALIGN_LEFT, NULL,
                       st->pressed == WB_LEFT || p->align == WA_LEFT, true);
    widget_icon_button(c, tool_rect(WB_MID), ICON_ALIGN_MID, NULL,
                       st->pressed == WB_MID || p->align == WA_CENTER, true);
    widget_icon_button(c, tool_rect(WB_RIGHT), ICON_ALIGN_RIGHT, NULL,
                       st->pressed == WB_RIGHT || p->align == WA_RIGHT, true);
}

/* Wo eine Zeile auf dem Papier anfaengt - je nach Ausrichtung. */
static int32_t line_x(struct window *win, const struct vline *line)
{
    struct wa_state *st = win->user;
    const struct paragraph *p = &st->doc->paras[line->para];
    int32_t width = page_width(win);
    int32_t indent = style_indent(p->style);
    int32_t left = (page_rect(win).w - width) / 2 + indent;
    int32_t used = line->len * style_char_w(p->style);

    if (p->align == WA_CENTER)
        return left + (width - indent - used) / 2;
    if (p->align == WA_RIGHT)
        return left + (width - indent - used);
    return left;
}

static void paint_page(struct window *win, struct canvas *c)
{
    struct wa_state *st = win->user;
    struct rect area = page_rect(win);
    int32_t width = page_width(win);
    int32_t left = area.x + (area.w - width) / 2;

    gfx_fill(c, area, WA_DESK);

    struct canvas page = *c;

    gfx_set_clip(&page, rect_intersect(c->clip, area));
    gfx_fill(&page, rect_make(left - WA_MARGIN / 2, area.y,
                              width + WA_MARGIN, area.h), WA_PAPER);
    gfx_vline(&page, left - WA_MARGIN / 2, area.y, area.h, WA_RULE);
    gfx_vline(&page, left + width + WA_MARGIN / 2, area.y, area.h, WA_RULE);

    for (int i = 0; i < st->line_count; i++) {
        const struct vline *line = &st->lines[i];
        int32_t y = area.y + line->y - st->scroll;

        if (y + line->height < area.y)
            continue;
        if (y > area.y + area.h)
            break;

        const struct paragraph *p = &st->doc->paras[line->para];
        int32_t char_w = style_char_w(p->style);
        int32_t scale = style_scale(p->style);
        bool bold = style_bold(p->style);
        int32_t x = area.x + line_x(win, line);
        uint32_t color = p->style == STYLE_QUOTE ? WA_QUOTE : COL_TEXT;

        /* Der Punkt vor einem Listeneintrag steht nur in der ersten
         * Zeile des Absatzes. */
        if (p->style == STYLE_LIST && line->start == 0)
            gfx_fill(&page, rect_make(x - 12, y + FONT_HEIGHT / 2 - 1, 4, 4),
                     COL_TEXT);
        if (p->style == STYLE_QUOTE)
            gfx_fill(&page, rect_make(x - 12, y, 3, line->height), WA_RULE);

        for (int k = 0; k < line->len; k++) {
            int at = line->start + k;
            char ch = p->text[at];
            uint8_t marks = p->marks[at];
            int32_t cx = x + k * char_w;

            if (in_selection(st, line->para, at))
                gfx_fill(&page, rect_make(cx, y, char_w,
                                          FONT_HEIGHT * scale), COL_SELECT);

            uint32_t ink = in_selection(st, line->para, at)
                               ? COL_SELECT_TEXT : color;
            char one[2] = { ch, '\0' };

            if (scale > 1)
                gfx_text_scaled(&page, cx, y, one, ink, scale, bold);
            else if (bold || (marks & MARK_BOLD))
                gfx_text_bold(&page, cx, y, one, ink);
            else
                gfx_text(&page, cx, y, one, ink);

            if (marks & MARK_UNDERLINE)
                gfx_hline(&page, cx, y + FONT_HEIGHT * scale, char_w, ink);
        }

        /* Die Schreibmarke sitzt in dieser Zeile? */
        if (st->caret_on && line->para == st->para &&
            st->at >= line->start &&
            (st->at < line->start + line->len ||
             (st->at == line->start + line->len &&
              (i + 1 >= st->line_count ||
               st->lines[i + 1].para != line->para)))) {
            int32_t cx = x + (st->at - line->start) * char_w;

            gfx_fill(&page, rect_make(cx, y, 2, FONT_HEIGHT * scale),
                     COL_ACCENT);
        }
    }

    widget_vscroll(c, rect_make(area.x + area.w, area.y,
                                SCROLLBAR_WIDTH, area.h),
                   st->scroll / 16, MAX(st->height / 16, 1),
                   MAX(area.h / 16, 1));
}

static void wa_paint(struct window *win, struct canvas *c)
{
    struct wa_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);

    if (st->needs_layout || st->layout_width != page_width(win))
        relayout(win);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);
    paint_toolbar(win, &local);
    paint_page(win, &local);

    char left[128], right[64];
    const struct paragraph *p = &st->doc->paras[st->para];

    ksnprintf(left, sizeof(left), tr("%s - Absatz %d von %d"),
              wdoc_style_name(p->style), st->para + 1, st->doc->count);
    ksnprintf(right, sizeof(right), tr("%u Woerter, %u Zeichen"),
              (unsigned)wdoc_words(st->doc), (unsigned)wdoc_chars(st->doc));
    widget_statusbar(&local, rect_make(0, local.h - WA_STATUS_H,
                                       local.w, WA_STATUS_H), left, right);
}

/* ------------------------------------------------------------------ */
/* Bedienung                                                           */
/* ------------------------------------------------------------------ */

static void wa_action(struct window *win, int action)
{
    struct wa_state *st = win->user;
    struct paragraph *p = &st->doc->paras[st->para];

    switch (action) {
    case WB_OPEN:
        dialog_input(tr("Oeffnen"), tr("Datei:"), "dokument.html", on_open, win);
        return;
    case WB_SAVE:
        do_save(win);
        return;
    case WB_STYLE:
        /* Reihum: Textkoerper, Ueberschrift 1, Ueberschrift 2, Zitat. */
        switch (p->style) {
        case STYLE_BODY:  p->style = STYLE_H1;    break;
        case STYLE_H1:    p->style = STYLE_H2;    break;
        case STYLE_H2:    p->style = STYLE_QUOTE; break;
        default:          p->style = STYLE_BODY;  break;
        }
        break;
    case WB_LIST:
        p->style = p->style == STYLE_LIST ? STYLE_BODY : STYLE_LIST;
        break;
    case WB_BOLD:
        apply_marks(win, MARK_BOLD);
        st->modified = true;
        update_title(win);
        return;
    case WB_UNDER:
        apply_marks(win, MARK_UNDERLINE);
        st->modified = true;
        update_title(win);
        return;
    case WB_LEFT:  p->align = WA_LEFT;   break;
    case WB_MID:   p->align = WA_CENTER; break;
    case WB_RIGHT: p->align = WA_RIGHT;  break;
    default:
        return;
    }

    st->modified = true;
    st->needs_layout = true;
    update_title(win);
    gui_invalidate();
}

/* Bewegt die Schreibmarke eine Zeile hinauf oder hinunter. */
static void move_line(struct window *win, int delta)
{
    struct wa_state *st = win->user;
    int index = line_of(st, st->para, st->at);
    const struct vline *from = &st->lines[index];
    int column = st->at - from->start;
    int target = CLAMP(index + delta, 0, st->line_count - 1);

    if (target == index)
        return;

    const struct vline *to = &st->lines[target];

    st->para = to->para;
    st->at = to->start + MIN(column, to->len);
}

static void wa_key(struct window *win, const struct gui_event *ev)
{
    struct wa_state *st = win->user;
    struct wdoc *doc = st->doc;
    bool shifted = (ev->mods & MOD_SHIFT) != 0;

    if (ev->mods & MOD_CTRL) {
        switch (ev->ascii) {
        case 's': case 'S': do_save(win); return;
        case 'b': case 'B': wa_action(win, WB_BOLD); return;
        case 'u': case 'U': wa_action(win, WB_UNDER); return;
        case 'a': case 'A':
            st->anchor_para = 0;
            st->anchor_at = 0;
            st->para = doc->count - 1;
            st->at = doc->paras[st->para].len;
            st->selecting = true;
            gui_invalidate();
            return;
        case 'c': case 'C':
            if (has_selection(st)) {
                int p1, a1, p2, a2;
                char buffer[1024];
                size_t n = 0;

                selection_range(st, &p1, &a1, &p2, &a2);
                for (int para = p1; para <= p2 && n + 1 < sizeof(buffer);
                     para++) {
                    struct paragraph *p = &doc->paras[para];
                    int from = para == p1 ? a1 : 0;
                    int to = para == p2 ? a2 : p->len;

                    for (int i = from; i < to && n + 1 < sizeof(buffer); i++)
                        buffer[n++] = p->text[i];
                    if (para < p2 && n + 1 < sizeof(buffer))
                        buffer[n++] = '\n';
                }
                clipboard_set(buffer, n);
            }
            return;
        case 'v': case 'V': {
            size_t bytes = 0;
            const char *paste = clipboard_get(&bytes);

            if (paste) {
                delete_selection(win);
                for (size_t i = 0; i < bytes && paste[i]; i++) {
                    if (paste[i] == '\r')
                        continue;
                    if (paste[i] == '\n') {
                        wdoc_split(doc, st->para, st->at);
                        st->para++;
                        st->at = 0;
                        continue;
                    }
                    wdoc_insert_char(doc, st->para, st->at, paste[i],
                                     st->marks);
                    st->at++;
                }
                st->modified = true;
                st->needs_layout = true;
                update_title(win);
                ensure_visible(win);
                gui_invalidate();
            }
            return;
        }
        default:
            break;
        }
    }

    bool moves = ev->key == KEY_LEFT || ev->key == KEY_RIGHT ||
                 ev->key == KEY_UP || ev->key == KEY_DOWN ||
                 ev->key == KEY_HOME || ev->key == KEY_END ||
                 ev->key == KEY_PAGEUP || ev->key == KEY_PAGEDOWN;

    if (moves && shifted && !st->selecting) {
        st->anchor_para = st->para;
        st->anchor_at = st->at;
        st->selecting = true;
    } else if (moves && !shifted) {
        st->selecting = false;
    }

    if (!moves && has_selection(st)) {
        bool replaces = ev->key == KEY_BACKSPACE || ev->key == KEY_DELETE ||
                        ev->key == KEY_ENTER ||
                        (ev->ascii >= 32 && (unsigned char)ev->ascii != 127);

        if (replaces) {
            delete_selection(win);
            if (ev->key == KEY_BACKSPACE || ev->key == KEY_DELETE) {
                update_title(win);
                ensure_visible(win);
                gui_invalidate();
                return;
            }
        }
    }

    switch (ev->key) {
    case KEY_LEFT:
        if (st->at > 0)
            st->at--;
        else if (st->para > 0)
            st->at = doc->paras[--st->para].len;
        break;

    case KEY_RIGHT:
        if (st->at < doc->paras[st->para].len)
            st->at++;
        else if (st->para + 1 < doc->count) {
            st->para++;
            st->at = 0;
        }
        break;

    case KEY_UP:       move_line(win, -1); break;
    case KEY_DOWN:     move_line(win, 1);  break;
    case KEY_PAGEUP:   move_line(win, -12); break;
    case KEY_PAGEDOWN: move_line(win, 12);  break;

    case KEY_HOME: {
        const struct vline *line = &st->lines[line_of(st, st->para, st->at)];

        st->at = line->start;
        break;
    }
    case KEY_END: {
        const struct vline *line = &st->lines[line_of(st, st->para, st->at)];

        st->at = line->start + line->len;
        break;
    }

    case KEY_BACKSPACE:
        if (st->at > 0) {
            wdoc_erase_char(doc, st->para, st->at - 1);
            st->at--;
        } else if (st->para > 0) {
            st->at = wdoc_join(doc, st->para);
            st->para--;
        }
        st->modified = true;
        st->needs_layout = true;
        break;

    case KEY_DELETE:
        if (st->at < doc->paras[st->para].len) {
            wdoc_erase_char(doc, st->para, st->at);
        } else if (st->para + 1 < doc->count) {
            wdoc_join(doc, st->para + 1);
        }
        st->modified = true;
        st->needs_layout = true;
        break;

    case KEY_ENTER:
        wdoc_split(doc, st->para, st->at);
        st->para++;
        st->at = 0;
        st->modified = true;
        st->needs_layout = true;
        break;

    case KEY_TAB:
        for (int i = 0; i < 4; i++) {
            wdoc_insert_char(doc, st->para, st->at, ' ', st->marks);
            st->at++;
        }
        st->modified = true;
        st->needs_layout = true;
        break;

    default:
        if (ev->ascii >= 32 && (unsigned char)ev->ascii != 127) {
            wdoc_insert_char(doc, st->para, st->at, ev->ascii, st->marks);
            st->at++;
            st->modified = true;
            st->needs_layout = true;
        } else {
            return;
        }
        break;
    }

    st->caret_on = true;
    if (st->needs_layout)
        relayout(win);
    ensure_visible(win);
    update_title(win);
    gui_invalidate();
}

/* Welche Stelle im Text liegt unter dem Zeiger? */
static void place_at(struct window *win, int32_t x, int32_t y)
{
    struct wa_state *st = win->user;
    struct rect area = page_rect(win);
    int32_t doc_y = y - area.y + st->scroll;

    if (st->line_count == 0)
        return;

    int index = st->line_count - 1;

    for (int i = 0; i < st->line_count; i++) {
        if (doc_y < st->lines[i].y + st->lines[i].height) {
            index = i;
            break;
        }
    }

    const struct vline *line = &st->lines[index];
    const struct paragraph *p = &st->doc->paras[line->para];
    int32_t char_w = style_char_w(p->style);
    int32_t start = area.x + line_x(win, line);
    int column = (x - start + char_w / 2) / char_w;

    st->para = line->para;
    st->at = line->start + CLAMP(column, 0, line->len);
    st->caret_on = true;
}

static void wa_event(struct window *win, const struct gui_event *ev)
{
    struct wa_state *st = win->user;

    switch (ev->type) {
    case EV_MOUSE_DOWN: {
        struct rect area = page_rect(win);

        if (ev->y < WA_TOOLBAR_H) {
            for (int i = 0; i < WB_COUNT; i++) {
                if (rect_contains(tool_rect(i), ev->x, ev->y)) {
                    st->pressed = i;
                    gui_invalidate();
                    break;
                }
            }
        } else if (ev->x >= area.x + area.w) {
            st->scroll = widget_vscroll_click(
                rect_make(area.x + area.w, area.y, SCROLLBAR_WIDTH, area.h),
                ev->y, st->scroll / 16, MAX(st->height / 16, 1),
                MAX(area.h / 16, 1)) * 16;
            st->scroll = CLAMP(st->scroll, 0, MAX(st->height - area.h, 0));
            gui_invalidate();
        } else if (rect_contains(area, ev->x, ev->y)) {
            st->selecting = false;
            place_at(win, ev->x, ev->y);
            st->anchor_para = st->para;
            st->anchor_at = st->at;
            gui_invalidate();
        }
        break;
    }

    case EV_MOUSE_DRAG:
        if (rect_contains(page_rect(win), ev->x, ev->y)) {
            st->selecting = true;
            place_at(win, ev->x, ev->y);
            gui_invalidate();
        }
        break;

    case EV_MOUSE_UP: {
        int pressed = st->pressed;

        st->pressed = -1;
        if (pressed >= 0 && rect_contains(tool_rect(pressed), ev->x, ev->y))
            wa_action(win, pressed);
        gui_invalidate();
        break;
    }

    case EV_SCROLL: {
        struct rect area = page_rect(win);

        st->scroll = CLAMP(st->scroll - ev->scroll * 3 * FONT_HEIGHT, 0,
                           MAX(st->height - area.h, 0));
        gui_invalidate();
        break;
    }

    case EV_KEY_DOWN:
        wa_key(win, ev);
        break;

    case EV_TICK:
        st->caret_on = !st->caret_on;
        gui_invalidate();
        break;

    case EV_RESIZED:
        st->needs_layout = true;
        gui_invalidate();
        break;

    default:
        break;
    }
}

static void wa_close(struct window *win)
{
    struct wa_state *st = win->user;

    if (st) {
        kfree(st->doc);
        kfree(st->lines);
        kfree(st);
    }
    win->user = NULL;
}

/* Ein kurzer Text beim Start - er zeigt die Formatvorlagen an sich
 * selbst. */
static void fill_example(struct wdoc *doc)
{
    static const struct {
        uint8_t style;
        uint8_t align;
        const char *text;
    } start[] = {
        { STYLE_H1,    WA_CENTER, "Schreiben" },
        { STYLE_BODY,  WA_LEFT,
          "Dieses Fenster setzt Text in Absaetzen. Jeder Absatz hat eine "
          "Formatvorlage; einzelne Woerter lassen sich fett oder "
          "unterstrichen machen." },
        { STYLE_H2,    WA_LEFT,  "Was geht" },
        { STYLE_LIST,  WA_LEFT,  "Ueberschriften, Aufzaehlungen und Zitate" },
        { STYLE_LIST,  WA_LEFT,  "Ausrichtung links, mittig oder rechts" },
        { STYLE_LIST,  WA_LEFT,  "Auswaehlen mit der Maus oder Umschalt" },
        { STYLE_QUOTE, WA_LEFT,
          "Gespeichert wird HTML - der Browser dieses Systems zeigt das "
          "Ergebnis unveraendert an." },
    };

    wdoc_clear(doc);

    for (size_t i = 0; i < ARRAY_LEN(start); i++) {
        int para = (int)i;

        if (i > 0)
            para = wdoc_insert(doc, (int)i - 1, start[i].style);

        doc->paras[para].style = start[i].style;
        doc->paras[para].align = start[i].align;

        for (const char *p = start[i].text; *p; p++)
            wdoc_insert_char(doc, para, doc->paras[para].len, *p, 0);
    }
}

static void write_new(struct fs_node *file)
{
    struct wa_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    st->doc = kzalloc(sizeof(struct wdoc));
    st->lines = kzalloc(sizeof(struct vline) * WA_LINES_MAX);

    if (!st->doc || !st->lines) {
        kfree(st->doc);
        kfree(st->lines);
        kfree(st);
        return;
    }

    static int32_t cascade;
    int32_t offset = (cascade++ % 5) * 24;

    struct window *win = gui_create_window("Schreiben", 110 + offset,
                                           50 + offset, 720, 520,
                                           WF_RESIZABLE, ICON_DOCUMENT);
    if (!win) {
        kfree(st->doc);
        kfree(st->lines);
        kfree(st);
        return;
    }

    st->pressed = -1;
    st->caret_on = true;
    st->needs_layout = true;

    win->user     = st;
    win->on_paint = wa_paint;
    win->on_event = wa_event;
    win->on_close = wa_close;
    win->min_w    = 420;
    win->min_h    = 300;

    if (file && fs_load(file)) {
        wdoc_from_html(st->doc, (const char *)file->data, file->size,
                       st->title, sizeof(st->title));
        st->file = file;
    } else {
        fill_example(st->doc);
    }

    update_title(win);
    gui_focus_window(win);
}

void write_open(struct fs_node *file)
{
    for (size_t i = 0; i < gui_window_count(); i++) {
        struct window *win = gui_window_at(i);

        if (win->on_paint == wa_paint) {
            struct wa_state *st = win->user;

            if (st && st->file == file) {
                gui_focus_window(win);
                return;
            }
        }
    }
    write_new(file);
}

void app_write(void)
{
    write_new(NULL);
}
