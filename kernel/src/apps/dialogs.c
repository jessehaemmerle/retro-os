/* dialogs.c - die kleinen Standardfenster: Eingabe, Rueckfrage, Hinweis.
 *
 * Alle drei sind gewoehnliche Fenster ohne Sonderrechte. Sie legen sich
 * lediglich beim Oeffnen nach vorne und melden ihr Ergebnis ueber eine
 * Rueckruffunktion - so bleibt der Rest des Systems bedienbar.
 */

#include "apps.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"
#include "theme.h"
#include "widgets.h"

#define DLG_INPUT_MAX 96

enum dialog_kind {
    DLG_INPUT,
    DLG_CONFIRM,
    DLG_MESSAGE,
};

struct dialog_state {
    enum dialog_kind kind;

    char  prompt[160];
    char  text[DLG_INPUT_MAX + 1];
    int32_t cursor;
    bool  caret_on;
    bool  preset_intact;   /* Vorgabe wird beim ersten Zeichen ersetzt */
    bool  secret;          /* Passwort - im Feld stehen nur Sternchen  */

    int   pressed_button;        /* -1 = keiner */

    dialog_text_fn    on_text;
    dialog_confirm_fn on_confirm;
    void             *user;
};

/* Zwei Knoepfe rechts unten; Index 0 ist der bestaetigende. */
static struct rect dialog_button_rect(struct window *win, int index)
{
    int32_t w = gui_client_width(win);
    int32_t h = gui_client_height(win);
    int32_t bw = 92, bh = 26;

    return rect_make(w - (index + 1) * (bw + 10) - 2, h - bh - 12, bw, bh);
}

static struct rect dialog_field_rect(struct window *win)
{
    return rect_make(14, 52, gui_client_width(win) - 28, 24);
}

static int dialog_button_count(struct dialog_state *st)
{
    return st->kind == DLG_MESSAGE ? 1 : 2;
}

static const char *dialog_button_label(struct dialog_state *st, int index)
{
    switch (st->kind) {
    case DLG_CONFIRM: return index == 0 ? "Ja" : "Nein";
    case DLG_MESSAGE: return "OK";
    default:          return index == 0 ? "OK" : "Abbrechen";
    }
}

/* Zeilenumbruch an Leerzeichen, damit laengere Meldungen lesbar bleiben. */
static void paint_wrapped(struct canvas *c, int32_t x, int32_t y,
                          int32_t max_w, const char *text)
{
    int32_t per_line = MAX(max_w / FONT_WIDTH, 8);
    char    line[128];

    while (*text) {
        int32_t len = 0, last_space = -1;

        while (text[len] && text[len] != '\n' && len < per_line &&
               len < (int32_t)sizeof(line) - 1) {
            if (text[len] == ' ')
                last_space = len;
            len++;
        }

        if (text[len] && text[len] != '\n' && last_space > 0)
            len = last_space;

        memcpy(line, text, (size_t)len);
        line[len] = '\0';
        gfx_text(c, x, y, line, COL_TEXT);
        y += FONT_HEIGHT + 2;

        text += len;
        while (*text == ' ' || *text == '\n')
            text++;
    }
}

static void dialog_paint(struct window *win, struct canvas *c)
{
    struct dialog_state *st = win->user;
    struct rect   client = gui_client_rect(win);
    struct canvas local  = gui_client_canvas(win, c);

    icon_draw(&local, 14, 14, st->kind == DLG_MESSAGE ? ICON_INFO : win->icon, 2);
    paint_wrapped(&local, 56, 16, client.w - 70, st->prompt);

    if (st->kind == DLG_INPUT) {
        /* Bei einem Passwort steht im Feld nur, wie viele Zeichen schon
         * da sind - genug, um zu sehen, dass die Tastatur ankommt, und
         * zu wenig fuer den, der ueber die Schulter schaut. */
        char shown[DLG_INPUT_MAX + 1];

        if (st->secret) {
            size_t len = strlen(st->text);

            for (size_t i = 0; i < len; i++)
                shown[i] = '*';
            shown[len] = '\0';
        } else {
            strlcpy(shown, st->text, sizeof(shown));
        }

        widget_field(&local, dialog_field_rect(win), shown,
                     st->caret_on ? st->cursor : -1, true);
    }

    for (int i = 0; i < dialog_button_count(st); i++)
        widget_button(&local, dialog_button_rect(win, i),
                      dialog_button_label(st, i), st->pressed_button == i, true);
}

static void dialog_finish(struct window *win, bool accepted)
{
    struct dialog_state *st = win->user;

    dialog_text_fn    on_text    = st->on_text;
    dialog_confirm_fn on_confirm = st->on_confirm;
    void             *user       = st->user;
    enum dialog_kind  kind       = st->kind;

    char text[DLG_INPUT_MAX + 1];
    strlcpy(text, st->text, sizeof(text));

    gui_close_window(win);      /* gibt st frei */

    if (kind == DLG_INPUT && accepted && on_text && text[0])
        on_text(text, user);
    else if (kind == DLG_CONFIRM && on_confirm)
        on_confirm(accepted, user);
}

static void dialog_key(struct window *win, const struct gui_event *ev)
{
    struct dialog_state *st = win->user;

    if (ev->key == KEY_LEFT || ev->key == KEY_RIGHT || ev->key == KEY_HOME ||
        ev->key == KEY_END || ev->key == KEY_BACKSPACE || ev->key == KEY_DELETE)
        st->preset_intact = false;

    switch (ev->key) {
    case KEY_ENTER:
        dialog_finish(win, true);
        return;
    case KEY_ESCAPE:
        dialog_finish(win, false);
        return;
    case KEY_LEFT:
        if (st->cursor > 0)
            st->cursor--;
        break;
    case KEY_RIGHT:
        if (st->text[st->cursor])
            st->cursor++;
        break;
    case KEY_HOME:
        st->cursor = 0;
        break;
    case KEY_END:
        st->cursor = (int32_t)strlen(st->text);
        break;
    case KEY_BACKSPACE:
        if (st->cursor > 0) {
            memmove(&st->text[st->cursor - 1], &st->text[st->cursor],
                    strlen(&st->text[st->cursor]) + 1);
            st->cursor--;
        }
        break;
    case KEY_DELETE:
        if (st->text[st->cursor])
            memmove(&st->text[st->cursor], &st->text[st->cursor + 1],
                    strlen(&st->text[st->cursor + 1]) + 1);
        break;
    default:
        if (st->kind == DLG_INPUT && ev->ascii >= 32 &&
            (unsigned char)ev->ascii != 127) {
            if (st->preset_intact) {
                st->text[0] = '\0';
                st->cursor = 0;
                st->preset_intact = false;
            }

            size_t len = strlen(st->text);

            if (len < DLG_INPUT_MAX) {
                memmove(&st->text[st->cursor + 1], &st->text[st->cursor],
                        len - (size_t)st->cursor + 1);
                st->text[st->cursor++] = ev->ascii;
            }
        }
        break;
    }
    gui_invalidate();
}

static void dialog_event(struct window *win, const struct gui_event *ev)
{
    struct dialog_state *st = win->user;

    switch (ev->type) {
    case EV_KEY_DOWN:
        dialog_key(win, ev);
        break;

    case EV_MOUSE_DOWN:
        for (int i = 0; i < dialog_button_count(st); i++) {
            if (rect_contains(dialog_button_rect(win, i), ev->x, ev->y)) {
                st->pressed_button = i;
                gui_invalidate();
                break;
            }
        }
        break;

    case EV_MOUSE_UP: {
        int pressed = st->pressed_button;

        st->pressed_button = -1;
        if (pressed >= 0 && rect_contains(dialog_button_rect(win, pressed),
                                          ev->x, ev->y)) {
            dialog_finish(win, pressed == 0);
            return;
        }
        gui_invalidate();
        break;
    }

    case EV_TICK:
        if (st->kind == DLG_INPUT) {
            st->caret_on = !st->caret_on;
            gui_invalidate();
        }
        break;

    default:
        break;
    }
}

static void dialog_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

static struct window *dialog_create(const char *title, enum dialog_kind kind,
                                    const char *prompt, enum icon_id icon,
                                    int32_t height)
{
    struct dialog_state *st = kzalloc(sizeof(*st));

    if (!st)
        return NULL;

    st->kind = kind;
    st->pressed_button = -1;
    st->caret_on = true;
    strlcpy(st->prompt, prompt, sizeof(st->prompt));

    struct window *win = gui_create_window(title, 0, 0, 400, height,
                                           WF_CENTER | WF_NO_MIN | WF_NO_TASKBAR,
                                           icon);
    if (!win) {
        kfree(st);
        return NULL;
    }

    win->user     = st;
    win->on_paint = dialog_paint;
    win->on_event = dialog_event;
    win->on_close = dialog_close;

    gui_focus_window(win);
    return win;
}

void dialog_input(const char *title, const char *prompt, const char *preset,
                  dialog_text_fn on_ok, void *user)
{
    struct window *win = dialog_create(title, DLG_INPUT, prompt, ICON_EDITOR, 168);

    if (!win)
        return;

    struct dialog_state *st = win->user;
    if (preset)
        strlcpy(st->text, preset, sizeof(st->text));
    st->preset_intact = preset && preset[0];
    st->cursor  = (int32_t)strlen(st->text);
    st->on_text = on_ok;
    st->user    = user;
}

void dialog_password(const char *title, const char *prompt,
                     dialog_text_fn on_ok, void *user)
{
    struct window *win = dialog_create(title, DLG_INPUT, prompt, ICON_KEY, 168);

    if (!win)
        return;

    struct dialog_state *st = win->user;

    st->secret  = true;
    st->cursor  = 0;
    st->on_text = on_ok;
    st->user    = user;
}

void dialog_confirm(const char *title, const char *message,
                    dialog_confirm_fn on_answer, void *user)
{
    struct window *win = dialog_create(title, DLG_CONFIRM, message, ICON_TRASH, 150);

    if (!win)
        return;

    struct dialog_state *st = win->user;
    st->on_confirm = on_answer;
    st->user       = user;
}

void dialog_message(const char *title, const char *message)
{
    dialog_create(title, DLG_MESSAGE, message, ICON_INFO, 150);
}
