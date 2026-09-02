/* slidesapp.c - das Fenster fuer Vortraege.
 *
 * Links stehen die Folien untereinander, rechts die gewaehlte in der
 * Groesse, in der sie spaeter an der Wand haengt. Bearbeitet wird
 * unmittelbar dort: Der Rahmen sitzt im Titel oder in einer Zeile, und
 * was getippt wird, steht sofort an seinem Platz. Ein zweites Fenster
 * mit Eingabefeldern braucht es dafuer nicht.
 *
 * Zum Vorfuehren wird dasselbe Fenster gross gezogen und alles
 * weggelassen, was nicht zur Folie gehoert. Die Pfeiltasten blaettern,
 * Escape bringt das Fenster zurueck an seinen Platz.
 */

#include "apps.h"
#include "deck.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"
#include "theme.h"
#include "widgets.h"

#define SL_TOOLBAR_H 34
#define SL_STATUS_H  24
#define SL_STRIP_W   150
#define SL_THUMB_H   62

/* Die Farben der Folie - dunkler Grund, heller Text, wie es sich fuer
 * einen Beamer gehoert. */
#define SL_BACK      RGB(0x10, 0x2A, 0x3C)
#define SL_BACK_2    RGB(0x1C, 0x40, 0x58)
#define SL_TITLE     RGB(0xFF, 0xFF, 0xFF)
#define SL_TEXT      RGB(0xD4, 0xE2, 0xEA)
#define SL_ACCENT    RGB(0xF0, 0xA5, 0x5E)
#define SL_STRIP_BG  RGB(0x9A, 0xA2, 0xAA)
#define SL_THUMB_BG  RGB(0x24, 0x4C, 0x66)

enum sl_button {
    SB_OPEN, SB_SAVE, SB_NEW, SB_DELETE, SB_LAYOUT, SB_SHOW, SB_COUNT
};

/* Wo die Schreibmarke sitzt: -1 ist der Titel, sonst die Zeilennummer. */
#define FIELD_TITLE (-1)

struct sl_state {
    struct deck    *deck;
    struct fs_node *file;
    bool            modified;

    int  slide;
    int  field;
    int  at;                    /* Stelle im Feld */

    bool presenting;
    struct rect saved_frame;
    uint32_t    saved_flags;

    int  strip_scroll;
    bool caret_on;
    int  pressed;
};

/* ------------------------------------------------------------------ */
/* Zugriff auf das gewaehlte Feld                                      */
/* ------------------------------------------------------------------ */

static struct slide *current(struct sl_state *st)
{
    return &st->deck->slides[CLAMP(st->slide, 0, st->deck->count - 1)];
}

static char *field_text(struct sl_state *st)
{
    struct slide *slide = current(st);

    if (st->field == FIELD_TITLE)
        return slide->title;
    if (st->field >= 0 && st->field < slide->line_count)
        return slide->lines[st->field];
    return slide->title;
}

static void clamp_cursor(struct sl_state *st)
{
    struct slide *slide = current(st);

    if (st->field >= slide->line_count)
        st->field = slide->line_count - 1;
    if (st->field < FIELD_TITLE)
        st->field = FIELD_TITLE;

    st->at = CLAMP(st->at, 0, (int)strlen(field_text(st)));
}

/* ------------------------------------------------------------------ */
/* Aufteilung                                                          */
/* ------------------------------------------------------------------ */

static struct rect strip_rect(struct window *win)
{
    return rect_make(0, SL_TOOLBAR_H, SL_STRIP_W,
                     gui_client_height(win) - SL_TOOLBAR_H - SL_STATUS_H);
}

static struct rect stage_rect(struct window *win)
{
    struct sl_state *st = win->user;

    if (st->presenting)
        return rect_make(0, 0, gui_client_width(win),
                         gui_client_height(win));

    struct rect strip = strip_rect(win);
    int32_t left = strip.x + strip.w + 6;
    int32_t width = gui_client_width(win) - left - 6;
    int32_t height = strip.h - 12;

    /* Eine Folie ist vier zu drei - das war schon so, als die Geraete
     * dafuer noch Diaprojektoren hiessen. */
    if (width * 3 > height * 4)
        width = height * 4 / 3;
    else
        height = width * 3 / 4;

    return rect_make(left + (gui_client_width(win) - left - 6 - width) / 2,
                     strip.y + 6, width, height);
}

/* ------------------------------------------------------------------ */
/* Eine Folie zeichnen                                                 */
/* ------------------------------------------------------------------ */

/* Groesse der Schrift, gemessen an der Breite der Folie: Eine Folie
 * soll gleich aussehen, ob sie als Vorschau oder an der Wand steht. */
static int32_t scale_for(int32_t width, int32_t want)
{
    int32_t scale = width * want / 640;

    return CLAMP(scale, 1, 6);
}

/* Bricht einen Text auf die Breite um und zeichnet ihn; liefert die
 * Hoehe. Mit c == NULL wird nur gemessen. */
static int32_t draw_wrapped(struct canvas *c, int32_t x, int32_t y,
                            int32_t width, const char *text, uint32_t color,
                            int32_t scale, bool bold)
{
    int32_t char_w = FONT_WIDTH * scale;
    int32_t line_h = FONT_HEIGHT * scale + scale * 2;
    int cols = MAX(width / char_w, 4);
    int len = (int)strlen(text);
    int at = 0;
    int32_t used = 0;

    do {
        int rest = len - at;
        int take = MIN(rest, cols);

        if (take < rest) {
            int cut = take;

            while (cut > 0 && text[at + cut] != ' ')
                cut--;
            if (cut > 0)
                take = cut;
        }

        char line[SLIDE_TEXT_MAX];
        int n = MIN(take, (int)sizeof(line) - 1);

        memcpy(line, text + at, (size_t)n);
        line[n] = '\0';

        if (c)
            gfx_text_scaled(c, x, y + used, line, color, scale, bold);
        used += line_h;
        at += take;
        if (at < len && text[at] == ' ')
            at++;
    } while (at < len);

    return used;
}

/* Wie hoch wird eine Aufzaehlung bei dieser Schriftgroesse? */
static int32_t bullets_height(const struct slide *slide, int32_t width,
                              int32_t title_scale, int32_t text_scale)
{
    int32_t bullet = MAX(text_scale * 3, 3);
    int32_t height = FONT_HEIGHT * title_scale + 6 + 2 + 10;

    for (int i = 0; i < slide->line_count; i++) {
        height += draw_wrapped(NULL, 0, 0, width - bullet * 4,
                               slide->lines[i], 0, text_scale, false);
        height += text_scale * 3;
    }
    return height;
}

/* Zeichnet die Folie in ein Rechteck. cursor_field sagt, wo die
 * Schreibmarke steht; -2 heisst "keine". */
static void draw_slide(struct canvas *c, struct rect box,
                       const struct slide *slide, int cursor_field,
                       int cursor_at, bool caret_on)
{
    gfx_gradient_v(c, box, SL_BACK_2, SL_BACK);

    struct canvas inner = *c;

    gfx_set_clip(&inner, rect_intersect(c->clip, box));

    int32_t pad = MAX(box.w / 16, 6);
    int32_t width = box.w - 2 * pad;
    int32_t title_scale = scale_for(box.w, slide->layout == LAYOUT_TITLE
                                               ? 4 : 3);
    int32_t text_scale = scale_for(box.w, 2);
    int32_t y;

    /* Lieber kleiner schreiben, als unten abzuschneiden: Was nicht auf
     * die Folie passt, hat auf einer Folie nichts verloren - aber
     * abgeschnitten waere es noch schlimmer. */
    if (slide->layout == LAYOUT_BULLETS) {
        while (text_scale > 1 &&
               bullets_height(slide, width, title_scale, text_scale) >
                   box.h - 2 * pad)
            text_scale--;

        while (title_scale > 1 &&
               bullets_height(slide, width, title_scale, text_scale) >
                   box.h - 2 * pad)
            title_scale--;
    }

    if (slide->layout == LAYOUT_TITLE) {
        /* Titel in die Mitte, Zeilen als Untertitel darunter. */
        int32_t block = FONT_HEIGHT * title_scale + 12;

        for (int i = 0; i < slide->line_count; i++)
            block += FONT_HEIGHT * text_scale + text_scale * 2;

        y = box.y + (box.h - block) / 2;

        int32_t tw = gfx_text_width_scaled(slide->title, title_scale);

        gfx_text_scaled(&inner, box.x + (box.w - tw) / 2, y, slide->title,
                        SL_TITLE, title_scale, true);

        if (cursor_field == FIELD_TITLE && caret_on) {
            int32_t cx = box.x + (box.w - tw) / 2 +
                         cursor_at * FONT_WIDTH * title_scale;

            gfx_fill(&inner, rect_make(cx, y, 2,
                                       FONT_HEIGHT * title_scale), SL_ACCENT);
        }

        y += FONT_HEIGHT * title_scale + 12;
        gfx_fill(&inner, rect_make(box.x + box.w / 4, y - 6, box.w / 2, 2),
                 SL_ACCENT);

        for (int i = 0; i < slide->line_count; i++) {
            int32_t lw = gfx_text_width_scaled(slide->lines[i], text_scale);
            int32_t lx = box.x + (box.w - lw) / 2;

            gfx_text_scaled(&inner, lx, y, slide->lines[i], SL_TEXT,
                            text_scale, false);
            if (cursor_field == i && caret_on)
                gfx_fill(&inner,
                         rect_make(lx + cursor_at * FONT_WIDTH * text_scale,
                                   y, 2, FONT_HEIGHT * text_scale), SL_ACCENT);
            y += FONT_HEIGHT * text_scale + text_scale * 2;
        }
        return;
    }

    if (slide->layout == LAYOUT_QUOTE) {
        int32_t block = 0;

        for (int i = 0; i < slide->line_count; i++)
            block += FONT_HEIGHT * text_scale + text_scale * 4;

        y = box.y + (box.h - block) / 2 - FONT_HEIGHT * text_scale;
        if (y < box.y + pad)
            y = box.y + pad;

        gfx_fill(&inner, rect_make(box.x + pad, y, 3, block), SL_ACCENT);

        for (int i = 0; i < slide->line_count; i++) {
            int32_t lx = box.x + pad + 14;

            gfx_text_scaled(&inner, lx, y, slide->lines[i], SL_TEXT,
                            text_scale, false);
            if (cursor_field == i && caret_on)
                gfx_fill(&inner,
                         rect_make(lx + cursor_at * FONT_WIDTH * text_scale,
                                   y, 2, FONT_HEIGHT * text_scale), SL_ACCENT);
            y += FONT_HEIGHT * text_scale + text_scale * 4;
        }

        /* Der Titel steht als Herkunft darunter. */
        int32_t tw = gfx_text_width_scaled(slide->title, text_scale);
        int32_t tx = box.x + box.w - pad - tw;

        y += FONT_HEIGHT * text_scale;
        gfx_text_scaled(&inner, tx, y, slide->title, SL_ACCENT, text_scale,
                        false);
        if (cursor_field == FIELD_TITLE && caret_on)
            gfx_fill(&inner,
                     rect_make(tx + cursor_at * FONT_WIDTH * text_scale, y,
                               2, FONT_HEIGHT * text_scale), SL_ACCENT);
        return;
    }

    /* Aufzaehlung: Titel oben, Punkte darunter. */
    y = box.y + pad;
    gfx_text_scaled(&inner, box.x + pad, y, slide->title, SL_TITLE,
                    title_scale, true);
    if (cursor_field == FIELD_TITLE && caret_on)
        gfx_fill(&inner,
                 rect_make(box.x + pad + cursor_at * FONT_WIDTH * title_scale,
                           y, 2, FONT_HEIGHT * title_scale), SL_ACCENT);

    y += FONT_HEIGHT * title_scale + 6;
    gfx_fill(&inner, rect_make(box.x + pad, y, width, 2), SL_ACCENT);
    y += 10;

    int32_t bullet = MAX(text_scale * 3, 3);

    for (int i = 0; i < slide->line_count; i++) {
        int32_t lx = box.x + pad + bullet * 4;

        gfx_fill(&inner, rect_make(box.x + pad + bullet,
                                   y + FONT_HEIGHT * text_scale / 2 - bullet / 2,
                                   bullet, bullet), SL_ACCENT);

        int32_t used = draw_wrapped(&inner, lx, y, width - bullet * 4,
                                    slide->lines[i], SL_TEXT, text_scale,
                                    false);

        if (cursor_field == i && caret_on)
            gfx_fill(&inner,
                     rect_make(lx + cursor_at * FONT_WIDTH * text_scale, y,
                               2, FONT_HEIGHT * text_scale), SL_ACCENT);

        y += used + text_scale * 3;
    }
}

/* ------------------------------------------------------------------ */
/* Vorfuehren                                                          */
/* ------------------------------------------------------------------ */

static void start_show(struct window *win)
{
    struct sl_state *st = win->user;
    struct canvas *screen = gfx_screen();

    if (st->presenting)
        return;

    st->saved_frame = win->frame;
    st->saved_flags = win->flags;
    st->presenting = true;

    /* Ueber den ganzen Bildschirm, ohne Rahmen: kein Titelbalken, kein
     * Anfasser, keine Taskleiste - nur die Folie. */
    win->frame = rect_make(0, 0, screen->w, screen->h);
    win->flags = WF_BARE;
    gui_focus_window(win);
    gui_invalidate();
}

static void stop_show(struct window *win)
{
    struct sl_state *st = win->user;

    if (!st->presenting)
        return;

    st->presenting = false;
    win->frame = st->saved_frame;
    win->flags = st->saved_flags;
    gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Dateien                                                             */
/* ------------------------------------------------------------------ */

static void update_title(struct window *win)
{
    struct sl_state *st = win->user;
    char text[WIN_TITLE_MAX + 1];

    ksnprintf(text, sizeof(text), "Vortrag - %s%s",
              st->file ? st->file->name : "Unbenannt",
              st->modified ? " *" : "");
    gui_set_title(win, text);
}

static struct fs_node *deck_dir(void)
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

    struct sl_state *st = win->user;
    struct fs_node *dir = deck_dir();
    struct fs_node *file = fs_find_child(dir, name);

    if (!file)
        file = fs_create(dir, name, FS_FILE);
    if (!file || file->type != FS_FILE || file->readonly) {
        dialog_message("Speichern", "Unter diesem Namen geht es nicht.");
        return;
    }

    size_t room = 64 * 1024;
    char *text = kmalloc(room);

    if (!text) {
        dialog_message("Speichern", "Zu wenig Speicher.");
        return;
    }

    size_t n = deck_to_text(st->deck, text, room);
    bool ok = fs_write(file, text, MIN(n, room - 1));

    kfree(text);

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
    struct sl_state *st = win->user;

    if (st->file && !fs_node_alive(st->file))
        st->file = NULL;

    if (!st->file || st->file->readonly) {
        dialog_input("Speichern unter", "Dateiname:",
                     st->file ? st->file->name : "vortrag.folien",
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
        file = fs_find_child(deck_dir(), path);

    if (!file || file->type != FS_FILE || !fs_load(file)) {
        dialog_message("Oeffnen", "Diese Datei gibt es nicht.");
        return;
    }

    struct sl_state *st = win->user;

    deck_from_text(st->deck, (const char *)file->data, file->size);
    st->file = file;
    st->modified = false;
    st->slide = 0;
    st->field = FIELD_TITLE;
    st->at = 0;
    st->strip_scroll = 0;
    update_title(win);
    gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

static struct rect tool_rect(int index)
{
    static const int32_t widths[SB_COUNT] = { 92, 104, 106, 96, 130, 112 };
    int32_t x = 4;

    for (int i = 0; i < index; i++)
        x += widths[i] + (i == SB_SAVE || i == SB_LAYOUT ? 10 : 3);

    return rect_make(x, 4, widths[index], SL_TOOLBAR_H - 8);
}

static void paint_strip(struct window *win, struct canvas *c)
{
    struct sl_state *st = win->user;
    struct rect area = strip_rect(win);

    gfx_fill(c, area, SL_STRIP_BG);
    gfx_bevel_thin(c, area, false);

    struct canvas strip = *c;

    gfx_set_clip(&strip, rect_intersect(c->clip, area));

    for (int i = 0; i < st->deck->count; i++) {
        int32_t y = area.y + 6 + i * SL_THUMB_H - st->strip_scroll;

        if (y + SL_THUMB_H < area.y)
            continue;
        if (y > area.y + area.h)
            break;

        struct rect box = rect_make(area.x + 26, y, area.w - 34,
                                    SL_THUMB_H - 8);
        char number[8];

        ksnprintf(number, sizeof(number), "%d", i + 1);
        gfx_text(&strip, area.x + 6, y + 10, number,
                 i == st->slide ? COL_TEXT : COL_TEXT_DIM);

        gfx_fill(&strip, box, SL_THUMB_BG);
        draw_slide(&strip, box, &st->deck->slides[i], -2, 0, false);

        if (i == st->slide) {
            gfx_frame(&strip, box, SL_ACCENT);
            gfx_frame(&strip, rect_make(box.x - 1, box.y - 1, box.w + 2,
                                        box.h + 2), SL_ACCENT);
        } else {
            gfx_frame(&strip, box, COL_DARK);
        }
    }
}

static void sl_paint(struct window *win, struct canvas *c)
{
    struct sl_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);
    struct slide *slide = current(st);

    if (st->presenting) {
        draw_slide(&local, rect_make(0, 0, local.w, local.h), slide, -2, 0,
                   false);

        char hint[64];

        ksnprintf(hint, sizeof(hint), "%d / %d", st->slide + 1,
                  st->deck->count);
        gfx_text(&local, local.w - gfx_text_width(hint) - 12,
                 local.h - FONT_HEIGHT - 8, hint, SL_TEXT);
        return;
    }

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    widget_toolbar(&local, rect_make(0, 0, local.w, SL_TOOLBAR_H));
    widget_icon_button(&local, tool_rect(SB_OPEN), ICON_FOLDER_OPEN,
                       "Oeffnen", st->pressed == SB_OPEN, true);
    widget_icon_button(&local, tool_rect(SB_SAVE), ICON_SAVE, "Speichern",
                       st->pressed == SB_SAVE, true);
    widget_icon_button(&local, tool_rect(SB_NEW), ICON_PLUS, "Folie",
                       st->pressed == SB_NEW,
                       st->deck->count < DECK_SLIDES_MAX);
    widget_icon_button(&local, tool_rect(SB_DELETE), ICON_TRASH, "Weg",
                       st->pressed == SB_DELETE, st->deck->count > 1);
    widget_icon_button(&local, tool_rect(SB_LAYOUT), ICON_SLIDES,
                       deck_layout_name(slide->layout),
                       st->pressed == SB_LAYOUT, true);
    widget_icon_button(&local, tool_rect(SB_SHOW), ICON_PRESENT, "Vorfuehren",
                       st->pressed == SB_SHOW, true);

    paint_strip(win, &local);

    struct rect stage = stage_rect(win);

    gfx_fill(&local, rect_make(stage.x - 2, stage.y - 2, stage.w + 4,
                               stage.h + 4), COL_DARK);
    draw_slide(&local, stage, slide, st->field, st->at, st->caret_on);

    char left[128], right[64];

    ksnprintf(left, sizeof(left), "Folie %d von %d - %s",
              st->slide + 1, st->deck->count,
              deck_layout_name(slide->layout));
    ksnprintf(right, sizeof(right), "%s",
              st->field == FIELD_TITLE ? "Titel" : "Zeile");
    widget_statusbar(&local, rect_make(0, local.h - SL_STATUS_H,
                                       local.w, SL_STATUS_H), left, right);
}

/* ------------------------------------------------------------------ */
/* Bedienung                                                           */
/* ------------------------------------------------------------------ */

static void sl_action(struct window *win, int action)
{
    struct sl_state *st = win->user;

    switch (action) {
    case SB_OPEN:
        dialog_input("Oeffnen", "Datei:", "vortrag.folien", on_open, win);
        return;
    case SB_SAVE:
        do_save(win);
        return;
    case SB_NEW:
        st->slide = deck_insert(st->deck, st->slide);
        st->field = FIELD_TITLE;
        st->at = 0;
        break;
    case SB_DELETE:
        deck_remove(st->deck, st->slide);
        st->slide = CLAMP(st->slide, 0, st->deck->count - 1);
        st->field = FIELD_TITLE;
        st->at = 0;
        break;
    case SB_LAYOUT: {
        struct slide *slide = current(st);

        slide->layout = (uint8_t)((slide->layout + 1) % LAYOUT_COUNT);
        break;
    }
    case SB_SHOW:
        start_show(win);
        return;
    default:
        return;
    }

    st->modified = true;
    update_title(win);
    gui_invalidate();
}

static void insert_char(struct sl_state *st, char ch)
{
    char *text = field_text(st);
    int len = (int)strlen(text);

    if (len + 1 >= SLIDE_TEXT_MAX)
        return;

    for (int i = len; i > st->at; i--)
        text[i] = text[i - 1];
    text[st->at] = ch;
    text[len + 1] = '\0';
    st->at++;
    st->modified = true;
}

static void erase_char(struct sl_state *st, int at)
{
    char *text = field_text(st);
    int len = (int)strlen(text);

    if (at < 0 || at >= len)
        return;

    for (int i = at; i < len; i++)
        text[i] = text[i + 1];
    st->modified = true;
}

static void show_key(struct window *win, const struct gui_event *ev)
{
    struct sl_state *st = win->user;

    switch (ev->key) {
    case KEY_ESCAPE:
        stop_show(win);
        return;
    case KEY_RIGHT:
    case KEY_DOWN:
    case KEY_PAGEDOWN:
    case KEY_ENTER:
        if (st->slide + 1 < st->deck->count)
            st->slide++;
        break;
    case KEY_LEFT:
    case KEY_UP:
    case KEY_PAGEUP:
    case KEY_BACKSPACE:
        if (st->slide > 0)
            st->slide--;
        break;
    case KEY_HOME:
        st->slide = 0;
        break;
    case KEY_END:
        st->slide = st->deck->count - 1;
        break;
    default:
        if (ev->ascii == ' ') {
            if (st->slide + 1 < st->deck->count)
                st->slide++;
            break;
        }
        return;
    }

    clamp_cursor(st);
    gui_invalidate();
}

static void sl_key(struct window *win, const struct gui_event *ev)
{
    struct sl_state *st = win->user;
    struct slide *slide = current(st);

    if (st->presenting) {
        show_key(win, ev);
        return;
    }

    if (ev->mods & MOD_CTRL) {
        switch (ev->ascii) {
        case 's': case 'S': do_save(win); return;
        default: break;
        }
    }

    if (ev->key == KEY_F5) {
        start_show(win);
        return;
    }

    /* Mit Strg wandert die Folie in der Reihenfolge. */
    if ((ev->mods & MOD_CTRL) &&
        (ev->key == KEY_UP || ev->key == KEY_DOWN)) {
        int direction = ev->key == KEY_UP ? -1 : 1;

        deck_move(st->deck, st->slide, direction);
        st->slide = CLAMP(st->slide + direction, 0, st->deck->count - 1);
        st->modified = true;
        update_title(win);
        gui_invalidate();
        return;
    }

    char *text = field_text(st);
    int len = (int)strlen(text);

    switch (ev->key) {
    case KEY_LEFT:
        if (st->at > 0)
            st->at--;
        break;

    case KEY_RIGHT:
        if (st->at < len)
            st->at++;
        break;

    case KEY_UP:
        if (st->field > FIELD_TITLE)
            st->field--;
        else if (st->slide > 0) {
            st->slide--;
            st->field = FIELD_TITLE;
        }
        st->at = 0;
        break;

    case KEY_DOWN:
        if (st->field + 1 < slide->line_count)
            st->field++;
        else if (st->slide + 1 < st->deck->count) {
            st->slide++;
            st->field = FIELD_TITLE;
        }
        st->at = 0;
        break;

    case KEY_PAGEUP:
        if (st->slide > 0)
            st->slide--;
        st->field = FIELD_TITLE;
        st->at = 0;
        break;

    case KEY_PAGEDOWN:
        if (st->slide + 1 < st->deck->count)
            st->slide++;
        st->field = FIELD_TITLE;
        st->at = 0;
        break;

    case KEY_HOME: st->at = 0;   break;
    case KEY_END:  st->at = len; break;

    case KEY_ENTER:
        /* Aus dem Titel geht es in die erste Zeile, aus einer Zeile in
         * eine neue darunter. */
        if (st->field == FIELD_TITLE && slide->line_count == 0) {
            st->field = slide_insert_line(slide, -1);
        } else if (st->field == FIELD_TITLE) {
            st->field = 0;
        } else {
            st->field = slide_insert_line(slide, st->field);
        }
        st->at = 0;
        st->modified = true;
        break;

    case KEY_TAB:
        st->field = st->field == FIELD_TITLE ? 0 : st->field + 1;
        clamp_cursor(st);
        st->at = 0;
        break;

    case KEY_BACKSPACE:
        if (st->at > 0) {
            erase_char(st, st->at - 1);
            st->at--;
        } else if (st->field >= 0) {
            /* Eine leere Zeile am Anfang loeschen. */
            if (len == 0) {
                slide_remove_line(slide, st->field);
                st->field--;
                st->at = (int)strlen(field_text(st));
                st->modified = true;
            }
        }
        break;

    case KEY_DELETE:
        erase_char(st, st->at);
        break;

    default:
        if (ev->ascii >= 32 && (unsigned char)ev->ascii != 127)
            insert_char(st, ev->ascii);
        else
            return;
        break;
    }

    clamp_cursor(st);
    st->caret_on = true;
    update_title(win);
    gui_invalidate();
}

static void sl_event(struct window *win, const struct gui_event *ev)
{
    struct sl_state *st = win->user;

    switch (ev->type) {
    case EV_MOUSE_DOWN: {
        if (st->presenting) {
            if (st->slide + 1 < st->deck->count)
                st->slide++;
            else
                stop_show(win);
            gui_invalidate();
            break;
        }

        struct rect strip = strip_rect(win);

        if (ev->y < SL_TOOLBAR_H) {
            for (int i = 0; i < SB_COUNT; i++) {
                if (rect_contains(tool_rect(i), ev->x, ev->y)) {
                    st->pressed = i;
                    gui_invalidate();
                    break;
                }
            }
        } else if (rect_contains(strip, ev->x, ev->y)) {
            int index = (ev->y - strip.y - 6 + st->strip_scroll) / SL_THUMB_H;

            if (index >= 0 && index < st->deck->count) {
                st->slide = index;
                st->field = FIELD_TITLE;
                st->at = 0;
                clamp_cursor(st);
                gui_invalidate();
            }
        }
        break;
    }

    case EV_MOUSE_UP: {
        int pressed = st->pressed;

        st->pressed = -1;
        if (pressed >= 0 && rect_contains(tool_rect(pressed), ev->x, ev->y))
            sl_action(win, pressed);
        gui_invalidate();
        break;
    }

    case EV_SCROLL: {
        struct rect strip = strip_rect(win);
        int32_t total = st->deck->count * SL_THUMB_H + 12;

        st->strip_scroll = CLAMP(st->strip_scroll - ev->scroll * SL_THUMB_H,
                                 0, MAX(total - strip.h, 0));
        gui_invalidate();
        break;
    }

    case EV_KEY_DOWN:
        sl_key(win, ev);
        break;

    case EV_TICK:
        st->caret_on = !st->caret_on;
        if (!st->presenting)
            gui_invalidate();
        break;

    case EV_RESIZED:
        gui_invalidate();
        break;

    default:
        break;
    }
}

static void sl_close(struct window *win)
{
    struct sl_state *st = win->user;

    if (st) {
        kfree(st->deck);
        kfree(st);
    }
    win->user = NULL;
}

static void fill_example(struct deck *deck)
{
    static const char beispiel[] =
        "# RetroOS\n"
        "! Titelfolie\n"
        "- Ein Betriebssystem von Grund auf\n"
        "\n"
        "# Was dazugehoert\n"
        "! Aufzaehlung\n"
        "- Eigener Kernel fuer x86-64, praeemptiv und auf mehreren Kernen\n"
        "- Treiber fuer Platte, Netz, USB und Grafik\n"
        "- Fenstersystem, Browser mit TLS, Editor und Konsole\n"
        "- Seit heute auch Tabelle, Schreiben und dieser Vortrag\n"
        "\n"
        "# Bedienung\n"
        "! Aufzaehlung\n"
        "- Links die Folien, rechts die gewaehlte\n"
        "- Tippen aendert Titel und Zeilen unmittelbar\n"
        "- Eingabe legt eine neue Zeile an\n"
        "- F5 fuehrt vor, Escape kommt zurueck\n"
        "\n"
        "# Alan Kay\n"
        "! Zitat\n"
        "- Wer es mit Software ernst meint,\n"
        "- sollte seine eigene Hardware bauen.\n";

    deck_from_text(deck, beispiel, sizeof(beispiel) - 1);
}

static void slides_new(struct fs_node *file)
{
    struct sl_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    st->deck = kzalloc(sizeof(struct deck));
    if (!st->deck) {
        kfree(st);
        return;
    }

    static int32_t cascade;
    int32_t offset = (cascade++ % 5) * 24;

    struct window *win = gui_create_window("Vortrag", 90 + offset,
                                           50 + offset, 780, 520,
                                           WF_RESIZABLE, ICON_SLIDES);
    if (!win) {
        kfree(st->deck);
        kfree(st);
        return;
    }

    st->pressed = -1;
    st->field = FIELD_TITLE;
    st->caret_on = true;

    win->user     = st;
    win->on_paint = sl_paint;
    win->on_event = sl_event;
    win->on_close = sl_close;
    win->min_w    = 520;
    win->min_h    = 320;

    if (file && fs_load(file)) {
        deck_from_text(st->deck, (const char *)file->data, file->size);
        st->file = file;
    } else {
        fill_example(st->deck);
    }

    update_title(win);
    gui_focus_window(win);
}

void slides_open(struct fs_node *file)
{
    for (size_t i = 0; i < gui_window_count(); i++) {
        struct window *win = gui_window_at(i);

        if (win->on_paint == sl_paint) {
            struct sl_state *st = win->user;

            if (st && st->file == file) {
                gui_focus_window(win);
                return;
            }
        }
    }
    slides_new(file);
}

void app_slides(void)
{
    slides_new(NULL);
}
