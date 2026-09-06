/* searchapp.c - das Suchfenster (Alt+Leertaste).
 *
 * Ein Feld oben, eine Liste darunter, sonst nichts: kein Rahmen, kein
 * Titel, keine Taskleistenschaltflaeche. Es ist kein Programm, das man
 * offen haelt, sondern eine Frage, die man stellt - und die Antwort
 * fuehrt woanders hin.
 *
 * Getippt wird und sofort gesucht; die Pfeiltasten waehlen, Eingabe
 * oeffnet, Escape macht zu. Dass die Liste bei jedem Tastendruck neu
 * entsteht, ist Absicht: So gibt es keinen Zwischenstand, der nicht
 * mehr stimmt.
 */

#include "apps.h"
#include "search.h"
#include "kstring.h"
#include "theme.h"
#include "font.h"
#include "widgets.h"
#include "lang.h"
#include "mm.h"

#define WIN_W       560
#define WIN_H       360
#define FIELD_H     34
#define ROW_H       30
#define QUERY_MAX   63

struct search_ui {
    char   query[QUERY_MAX + 1];
    size_t length;

    struct search_hit hits[SEARCH_MAX_HITS];
    size_t            count;
    size_t            selected;
};

static void refresh(struct search_ui *ui)
{
    ui->count = search_collect(ui->query, ui->hits, SEARCH_MAX_HITS);
    ui->selected = 0;
}

static int visible_rows(struct window *win)
{
    return MAX((gui_client_height(win) - FIELD_H - 16) / ROW_H, 1);
}

static const char *kind_word(enum search_kind kind)
{
    switch (kind) {
    case SEARCH_APP:     return "Programm";
    case SEARCH_SETTING: return "Einstellungen";
    default:             return "Datei";
    }
}

static void search_paint(struct window *win, struct canvas *c)
{
    struct search_ui *ui = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);
    gfx_bevel(&local, rect_make(0, 0, local.w, local.h), true);

    struct rect field = rect_make(10, 10, local.w - 20, FIELD_H);

    gfx_fill(&local, field, COL_FIELD);
    gfx_bevel(&local, field, false);
    icon_draw(&local, field.x + 8, field.y + 9, ICON_SEARCH, 1);

    if (ui->length == 0) {
        gfx_text(&local, field.x + 32, field.y + 10,
                 tr("Programme, Dateien, Einstellungen ..."), COL_TEXT_DIM);
    } else {
        gfx_text(&local, field.x + 32, field.y + 10, ui->query, COL_TEXT);
        gfx_vline(&local, field.x + 32 + gfx_text_width(ui->query) + 1,
                  field.y + 8, FONT_HEIGHT + 4, COL_TEXT);
    }

    int rows = visible_rows(win);

    /* Die Auswahl bleibt sichtbar: Bei einer langen Liste wandert das
     * Fenster mit ihr. */
    size_t first = 0;

    if (ui->selected >= (size_t)rows)
        first = ui->selected - (size_t)rows + 1;

    for (int i = 0; i < rows; i++) {
        size_t index = first + (size_t)i;

        if (index >= ui->count)
            break;

        const struct search_hit *h = &ui->hits[index];
        struct rect r = rect_make(10, 10 + FIELD_H + 6 + i * ROW_H,
                                  local.w - 20, ROW_H);
        bool sel = (index == ui->selected);

        if (sel)
            gfx_fill(&local, r, COL_SELECT);

        icon_draw(&local, r.x + 6, r.y + 7, (enum icon_id)h->icon, 1);
        gfx_text(&local, r.x + 30, r.y + 8,
                 h->kind == SEARCH_FILE ? h->label : tr(h->label),
                 sel ? COL_SELECT_TEXT : COL_TEXT);

        const char *detail = h->kind == SEARCH_FILE ? h->detail
                                                    : tr(kind_word(h->kind));
        int32_t dw = gfx_text_width(detail);

        gfx_set_clip(&local, rect_intersect(local.clip, r));
        gfx_text(&local, r.x + r.w - dw - 8, r.y + 8, detail,
                 sel ? COL_SELECT_TEXT : COL_TEXT_DIM);
        gfx_reset_clip(&local);
    }

    if (ui->length && ui->count == 0)
        gfx_text(&local, 16, 10 + FIELD_H + 14, tr("Nichts gefunden."),
                 COL_TEXT_DIM);
}

static void run_selected(struct window *win)
{
    struct search_ui *ui = win->user;

    if (ui->selected >= ui->count)
        return;

    struct search_hit hit = ui->hits[ui->selected];

    /* Erst zumachen, dann oeffnen: Das neue Fenster soll vorne
     * stehen, nicht hinter der Suche. */
    gui_close_window(win);
    search_run(&hit);
}

static void search_event(struct window *win, const struct gui_event *ev)
{
    struct search_ui *ui = win->user;

    switch (ev->type) {
    case EV_KEY_DOWN:
        if (ev->key == KEY_ESCAPE) {
            gui_close_window(win);
            return;
        }
        if (ev->key == KEY_ENTER) {
            run_selected(win);
            return;
        }
        if (ev->key == KEY_DOWN) {
            if (ui->count && ui->selected + 1 < ui->count)
                ui->selected++;
        } else if (ev->key == KEY_UP) {
            if (ui->selected)
                ui->selected--;
        } else if (ev->key == KEY_BACKSPACE) {
            if (ui->length) {
                ui->query[--ui->length] = '\0';
                refresh(ui);
            }
        } else if (ev->ascii >= ' ' && ui->length < QUERY_MAX) {
            ui->query[ui->length++] = ev->ascii;
            ui->query[ui->length] = '\0';
            refresh(ui);
        } else {
            return;
        }
        gui_invalidate();
        break;
    case EV_MOUSE_DOWN: {
        int rows = visible_rows(win);
        size_t first = 0;

        if (ui->selected >= (size_t)rows)
            first = ui->selected - (size_t)rows + 1;

        int row = (ev->y - (10 + FIELD_H + 6)) / ROW_H;

        if (row >= 0 && row < rows && first + (size_t)row < ui->count) {
            ui->selected = first + (size_t)row;
            run_selected(win);
        }
        break;
    }
    case EV_SCROLL:
        if (ev->scroll > 0 && ui->selected + 1 < ui->count)
            ui->selected++;
        else if (ev->scroll < 0 && ui->selected)
            ui->selected--;
        gui_invalidate();
        break;
    case EV_BLUR:
        /* Wer woanders hinklickt, wollte nicht suchen. */
        gui_close_window(win);
        break;
    default:
        break;
    }
}

static void search_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_search(void)
{
    struct window *existing = gui_find_by_paint(search_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct search_ui *ui = kzalloc(sizeof(*ui));

    if (!ui)
        return;

    struct canvas *screen = gfx_screen();
    int32_t w = MIN(WIN_W, screen->w - 40);
    int32_t h = MIN(WIN_H, desktop_work_height() - 40);

    /* Etwas ueber der Mitte: Dort sucht das Auge, und die Liste
     * waechst nach unten. */
    struct window *win = gui_create_window(tr("Suche"), (screen->w - w) / 2,
                                           desktop_work_height() / 6, w, h,
                                           WF_BARE | WF_NO_TASKBAR, ICON_SEARCH);
    if (!win) {
        kfree(ui);
        return;
    }

    win->user     = ui;
    win->on_paint = search_paint;
    win->on_event = search_event;
    win->on_close = search_close;

    gui_focus_window(win);
    gui_invalidate();
}
