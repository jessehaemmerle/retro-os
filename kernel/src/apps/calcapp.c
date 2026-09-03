/* calcapp.c - das Fenster zum Taschenrechner.
 *
 * Das Rechnen steht in calc.c; hier stehen nur Knoepfe. Jede Taste
 * gibt es zweimal - als Knopf und auf der Tastatur -, und beide gehen
 * durch dieselbe Funktion. Ein Taschenrechner, bei dem die Maus etwas
 * anderes tut als die Tastatur, ist ein Aergernis.
 */

#include "apps.h"
#include "calc.h"
#include "font.h"
#include "gui.h"
#include "kstring.h"
#include "lang.h"
#include "mm.h"
#include "theme.h"
#include "widgets.h"

#define PAD_COLS   4
#define PAD_ROWS   5
#define DISPLAY_H  46
#define GAP        4

struct calc_ui {
    struct calc machine;
    int         hover;     /* Knopf unter dem Zeiger, -1 = keiner */
    int         pressed;
};

/* Die Belegung. Ein leeres Feld gibt es nicht - jede Stelle traegt
 * einen Knopf, sonst wirkt das Gitter kaputt. */
static const struct {
    const char *label;
    char        action;   /* was calc_key() daraus macht */
} pad[PAD_ROWS][PAD_COLS] = {
    { { "C",  'C' }, { "CE", 'E' }, { "<-", 'B' }, { "/", '/' } },
    { { "7",  '7' }, { "8",  '8' }, { "9",  '9' }, { "*", '*' } },
    { { "4",  '4' }, { "5",  '5' }, { "6",  '6' }, { "-", '-' } },
    { { "1",  '1' }, { "2",  '2' }, { "3",  '3' }, { "+", '+' } },
    { { "+/-",'S' }, { "0",  '0' }, { ",",  ',' }, { "=", '=' } },
};

/* Eine Reihe daneben: was seltener gebraucht wird. */
static const struct {
    const char *label;
    char        action;
} extra[] = {
    { "%",    '%' },
    { "Wurzel", 'W' },   /* uebersetzt wird beim Zeichnen */
};

static void calc_key(struct calc_ui *ui, char action)
{
    struct calc *m = &ui->machine;

    if (action >= '0' && action <= '9') {
        calc_digit(m, action - '0');
        return;
    }

    switch (action) {
    case ',':
    case '.': calc_point(m);          break;
    case '+':
    case '-':
    case '*':
    case '/': calc_op(m, action);     break;
    case '=': calc_equals(m);         break;
    case 'C': calc_reset(m);          break;
    case 'E': calc_clear_entry(m);    break;
    case 'B': calc_backspace(m);      break;
    case 'S': calc_sign(m);           break;
    case '%': calc_percent(m);        break;
    case 'W': calc_sqrt(m);           break;
    default:  break;
    }
}

/* ------------------------------------------------------------------ */

static struct rect pad_rect(struct window *win, int row, int col)
{
    int32_t w = gui_client_width(win) - 2 * GAP;
    int32_t top = DISPLAY_H + 2 * GAP + 30;
    int32_t h = gui_client_height(win) - top - GAP;
    int32_t cw = (w - (PAD_COLS - 1) * GAP) / PAD_COLS;
    int32_t ch = (h - (PAD_ROWS - 1) * GAP) / PAD_ROWS;

    return rect_make(GAP + col * (cw + GAP), top + row * (ch + GAP), cw, ch);
}

static struct rect extra_rect(struct window *win, size_t index)
{
    int32_t w = gui_client_width(win) - 2 * GAP;
    int32_t cw = (w - GAP) / 2;

    return rect_make(GAP + (int32_t)index * (cw + GAP), DISPLAY_H + 2 * GAP,
                     cw, 26);
}

/* Ein durchlaufender Index ueber beide Gruppen - damit hover und
 * pressed mit einer einzigen Zahl auskommen. */
static int hit(struct window *win, int32_t x, int32_t y)
{
    for (size_t i = 0; i < ARRAY_LEN(extra); i++)
        if (rect_contains(extra_rect(win, i), x, y))
            return (int)i;

    for (int row = 0; row < PAD_ROWS; row++)
        for (int col = 0; col < PAD_COLS; col++)
            if (rect_contains(pad_rect(win, row, col), x, y))
                return (int)ARRAY_LEN(extra) + row * PAD_COLS + col;
    return -1;
}

static char action_at(int index)
{
    if (index < 0)
        return 0;
    if (index < (int)ARRAY_LEN(extra))
        return extra[index].action;

    index -= (int)ARRAY_LEN(extra);
    if (index >= PAD_ROWS * PAD_COLS)
        return 0;
    return pad[index / PAD_COLS][index % PAD_COLS].action;
}

static void calc_paint(struct window *win, struct canvas *c)
{
    struct calc_ui *ui = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    /* Die Anzeige: rechtsbuendig wie bei jedem Rechner. */
    struct rect display = rect_make(GAP, GAP, local.w - 2 * GAP, DISPLAY_H);
    char text[40];

    gfx_fill(&local, display, COL_FIELD);
    gfx_bevel(&local, display, false);
    calc_display(&ui->machine, text, sizeof(text));

    int32_t tw = gfx_text_width_scaled(text, 2);

    gfx_text_scaled(&local, display.x + display.w - tw - 8,
                    display.y + (display.h - FONT_HEIGHT * 2) / 2, text,
                    ui->machine.error ? COL_ACCENT : COL_TEXT, 2, false);

    /* Was gerade ansteht, klein darueber - sonst weiss man nach drei
     * Tasten nicht mehr, was man angefangen hat. */
    if (ui->machine.op) {
        char pending[32];
        char sign[2] = { ui->machine.op, '\0' };

        calc_format(ui->machine.acc, pending, sizeof(pending));
        ksnprintf(text, sizeof(text), "%s %s", pending, sign);
        gfx_text(&local, display.x + 6, display.y + 3, text, COL_TEXT_DIM);
    }

    for (size_t i = 0; i < ARRAY_LEN(extra); i++) {
        int index = (int)i;

        widget_button(&local, extra_rect(win, i), tr(extra[i].label),
                      ui->pressed == index, true);
        if (ui->hover == index)
            gfx_frame(&local, extra_rect(win, i), COL_SELECT);
    }

    for (int row = 0; row < PAD_ROWS; row++) {
        for (int col = 0; col < PAD_COLS; col++) {
            int index = (int)ARRAY_LEN(extra) + row * PAD_COLS + col;
            struct rect r = pad_rect(win, row, col);

            widget_button(&local, r, pad[row][col].label,
                          ui->pressed == index, true);
            if (ui->hover == index)
                gfx_frame(&local, r, COL_SELECT);
        }
    }
}

static void calc_event(struct window *win, const struct gui_event *ev)
{
    struct calc_ui *ui = win->user;

    switch (ev->type) {
    case EV_MOUSE_MOVE: {
        int before = ui->hover;

        ui->hover = hit(win, ev->x, ev->y);
        if (before != ui->hover)
            gui_invalidate();
        return;
    }
    case EV_MOUSE_DOWN:
    case EV_DOUBLE_CLICK:
        ui->pressed = hit(win, ev->x, ev->y);
        if (ui->pressed >= 0)
            calc_key(ui, action_at(ui->pressed));
        gui_invalidate();
        return;
    case EV_MOUSE_UP:
        ui->pressed = -1;
        gui_invalidate();
        return;
    case EV_KEY_DOWN:
        break;
    default:
        return;
    }

    /* Tastatur: Ziffern und Zeichen unmittelbar, dazu die ueblichen
     * Sondertasten. Komma und Punkt sind dasselbe - auf dem
     * Ziffernblock steht je nach Belegung das eine oder das andere. */
    switch (ev->key) {
    case KEY_ENTER:     calc_key(ui, '='); break;
    case KEY_BACKSPACE: calc_key(ui, 'B'); break;
    case KEY_ESCAPE:    calc_key(ui, 'C'); break;
    case KEY_DELETE:    calc_key(ui, 'E'); break;
    default:
        if (ev->ascii)
            calc_key(ui, ev->ascii);
        else
            return;
        break;
    }
    gui_invalidate();
}

static void calc_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_calculator(void)
{
    struct window *existing = gui_find_by_paint(calc_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct calc_ui *ui = kzalloc(sizeof(*ui));

    if (!ui)
        return;

    calc_reset(&ui->machine);
    ui->hover = -1;
    ui->pressed = -1;

    struct window *win = gui_create_window(tr("Rechner"), 200, 120, 300, 380,
                                           WF_RESIZABLE, ICON_CALC);
    if (!win) {
        kfree(ui);
        return;
    }

    win->user     = ui;
    win->min_w    = 240;
    win->min_h    = 320;
    win->on_paint = calc_paint;
    win->on_event = calc_event;
    win->on_close = calc_close;

    gui_focus_window(win);
}
