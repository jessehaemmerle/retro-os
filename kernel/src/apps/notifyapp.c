/* notifyapp.c - der Verlauf der Benachrichtigungen.
 *
 * Eine Liste, neueste oben, mit Symbol, Titel, Text und Uhrzeit. Wer
 * sie oeffnet, hat alles gesehen - deshalb gelten die Meldungen beim
 * Oeffnen als gelesen und die Zahl an der Glocke ist weg.
 *
 * Loeschen ist der einzige Knopf. Ein Verlauf, in dem man einzelne
 * Zeilen wegraeumen kann, ist ein Postfach; das ist hier nicht
 * gemeint.
 */

#include "apps.h"
#include "notify.h"
#include "kstring.h"
#include "mm.h"
#include "theme.h"
#include "widgets.h"
#include "lang.h"

#define ROW_H 44

struct notify_ui {
    int  scroll;      /* erste sichtbare Zeile */
    bool clear_hover;
};

static struct rect clear_rect(struct window *win)
{
    return rect_make(gui_client_width(win) - 150, gui_client_height(win) - 38,
                     134, 26);
}

static int32_t list_height(struct window *win)
{
    return gui_client_height(win) - 48;
}

static int visible_rows(struct window *win)
{
    return MAX(list_height(win) / ROW_H, 1);
}

static void notify_paint(struct window *win, struct canvas *c)
{
    struct notify_ui *ui = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    struct rect list = rect_make(8, 8, local.w - 16, list_height(win));

    gfx_fill(&local, list, COL_FIELD);
    gfx_bevel(&local, list, false);

    size_t count = notify_count();

    if (count == 0) {
        const char *text = tr("Keine Benachrichtigungen.");

        gfx_text(&local, list.x + (list.w - gfx_text_width(text)) / 2,
                 list.y + 20, text, COL_TEXT_DIM);
    }

    int rows = visible_rows(win);

    for (int i = 0; i < rows; i++) {
        size_t index = (size_t)(ui->scroll + i);
        const struct notification *n = notify_at(index);

        if (!n)
            break;

        int32_t y = list.y + 4 + i * ROW_H;

        icon_draw(&local, list.x + 8, y + 6, n->icon, 1);
        gfx_text_bold(&local, list.x + 32, y + 2, tr(n->title), COL_TEXT);
        gfx_text(&local, list.x + list.w - gfx_text_width(n->time) - 10, y + 2,
                 n->time, COL_TEXT_DIM);

        gfx_set_clip(&local, rect_intersect(local.clip, list));
        gfx_text_clipped(&local, list.x + 32, y + 18, tr(n->text), COL_TEXT_DIM,
                         list.w - 44);
        gfx_reset_clip(&local);

        if (i + 1 < rows && notify_at(index + 1))
            gfx_hline(&local, list.x + 6, y + ROW_H - 4, list.w - 12,
                      COL_SHADOW);
    }

    char summary[64];

    ksnprintf(summary, sizeof(summary), "%u %s", (unsigned)count,
              count == 1 ? tr("Meldung") : tr("Meldungen"));
    gfx_text(&local, 10, local.h - 32, summary, COL_TEXT_DIM);

    widget_button(&local, clear_rect(win), tr("Alle loeschen"),
                  ui->clear_hover, count > 0);
}

static void notify_event(struct window *win, const struct gui_event *ev)
{
    struct notify_ui *ui = win->user;
    int rows = visible_rows(win);
    int max_scroll = MAX((int)notify_count() - rows, 0);

    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        bool hover = rect_contains(clear_rect(win), ev->x, ev->y);

        if (hover != ui->clear_hover) {
            ui->clear_hover = hover;
            gui_invalidate();
        }
        break;
    }
    case EV_MOUSE_DOWN:
        if (rect_contains(clear_rect(win), ev->x, ev->y)) {
            notify_clear();
            ui->scroll = 0;
            gui_invalidate();
        }
        break;
    case EV_SCROLL:
        ui->scroll = CLAMP(ui->scroll + (ev->scroll > 0 ? 1 : -1), 0, max_scroll);
        gui_invalidate();
        break;
    case EV_KEY_DOWN:
        if (ev->key == KEY_UP)
            ui->scroll = MAX(ui->scroll - 1, 0);
        else if (ev->key == KEY_DOWN)
            ui->scroll = MIN(ui->scroll + 1, max_scroll);
        else
            break;
        gui_invalidate();
        break;
    case EV_FOCUS:
        /* Wer hinsieht, hat gelesen. */
        notify_mark_seen();
        gui_invalidate();
        break;
    default:
        break;
    }
}

static void notify_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_notifications(void)
{
    struct window *existing = gui_find_by_paint(notify_paint);

    if (existing) {
        notify_mark_seen();
        gui_focus_window(existing);
        return;
    }

    struct notify_ui *ui = kzalloc(sizeof(*ui));

    if (!ui)
        return;

    struct window *win = gui_create_window(tr("Benachrichtigungen"), 0, 0,
                                           440, 340, WF_CENTER | WF_RESIZABLE,
                                           ICON_BELL);
    if (!win) {
        kfree(ui);
        return;
    }

    win->user     = ui;
    win->on_paint = notify_paint;
    win->on_event = notify_event;
    win->on_close = notify_close;

    notify_mark_seen();
    gui_focus_window(win);
}
