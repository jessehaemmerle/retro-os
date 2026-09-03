/* clockapp.c - Uhr, Stoppuhr und Kurzzeitmesser.
 *
 * Drei Dinge in einem Fenster, weil sie dasselbe brauchen: einen
 * Taktimpuls und eine Anzeige. Die Oberflaeche schickt zehnmal je
 * Sekunde ein EV_TICK; darauf laeuft alles.
 *
 * Der Sekundenzeiger springt also in Zehntelschritten und nicht
 * fluessig. Das ist Absicht: Ein Zeiger, der bei sechzig Bildern je
 * Sekunde gleitet, hiesse sechzigmal ein neues Vollbild - und der
 * Hintergrund wird dabei jedes Mal mitgezeichnet.
 *
 * Gemessen wird nach timer_ms() und nicht durch Zaehlen der Impulse:
 * Wer Impulse zaehlt, misst die Genauigkeit der Oberflaeche und nicht
 * die Zeit.
 */

#include "apps.h"
#include "arch.h"
#include "font.h"
#include "kstring.h"
#include "lang.h"
#include "mm.h"
#include "rtc.h"
#include "theme.h"
#include "trig.h"
#include "widgets.h"

#define TAB_H     30
#define STATUS_H  26

enum clock_tab {
    TAB_CLOCK,
    TAB_STOP,
    TAB_TIMER,
    TAB_COUNT
};

struct clock_state {
    int      tab;
    int      hover;

    /* Stoppuhr */
    bool     running;
    uint64_t started_ms;      /* Zeitpunkt des Starts   */
    uint64_t collected_ms;    /* was vorher schon lief  */
    uint64_t lap_ms;

    /* Kurzzeitmesser */
    uint32_t preset_s;        /* eingestellte Dauer     */
    uint64_t ends_ms;
    bool     counting;
    bool     rang;
};

/* ------------------------------------------------------------------ */

static uint64_t stop_value(const struct clock_state *st)
{
    if (!st->running)
        return st->collected_ms;
    return st->collected_ms + (timer_ms() - st->started_ms);
}

static uint32_t timer_left(const struct clock_state *st)
{
    if (!st->counting)
        return st->preset_s;

    uint64_t now = timer_ms();

    if (now >= st->ends_ms)
        return 0;
    return (uint32_t)((st->ends_ms - now + 999) / 1000);
}

static void format_stop(uint64_t ms, char *out, size_t size)
{
    ksnprintf(out, size, "%u:%02u:%02u,%u",
              (unsigned)(ms / 3600000), (unsigned)(ms / 60000 % 60),
              (unsigned)(ms / 1000 % 60), (unsigned)(ms % 1000 / 100));
}

static void format_clock(uint32_t seconds, char *out, size_t size)
{
    ksnprintf(out, size, "%u:%02u:%02u", (unsigned)(seconds / 3600),
              (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60));
}

/* ------------------------------------------------------------------ */
/* Das Zifferblatt                                                     */
/* ------------------------------------------------------------------ */

/* Ein Zeiger vom Mittelpunkt aus. Der Winkel zaehlt von zwoelf Uhr im
 * Uhrzeigersinn, das Bild zaehlt nach unten positiv - daher das
 * Minus beim Kosinus. */
static void hand(struct canvas *c, int32_t cx, int32_t cy, int32_t degrees,
                 int32_t length, int32_t thickness, uint32_t colour)
{
    int32_t x = cx + sin_deg(degrees) * length / TRIG_ONE;
    int32_t y = cy - cos_deg(degrees) * length / TRIG_ONE;

    gfx_line(c, cx, cy, x, y, colour);

    /* Dickere Zeiger werden aus mehreren Linien gebaut - eine
     * Strichstaerke kennt gfx nicht. */
    for (int32_t i = 1; i < thickness; i++) {
        gfx_line(c, cx + i, cy, x, y, colour);
        gfx_line(c, cx, cy + i, x, y, colour);
    }
}

static void paint_face(struct canvas *c, struct rect area,
                       const struct datetime *now)
{
    int32_t radius = MIN(area.w, area.h) / 2 - 6;
    int32_t cx = area.x + area.w / 2;
    int32_t cy = area.y + area.h / 2;

    if (radius < 20)
        return;

    /* Der Kreis aus Strichen: alle sechs Grad ein Punkt, jede Stunde
     * ein laengerer. Ein gefuellter Kreis waere hier Verschwendung -
     * das Zifferblatt ist ohnehin nur Rand. */
    for (int32_t deg = 0; deg < 360; deg += 6) {
        bool hour = (deg % 30) == 0;
        int32_t inner = radius - (hour ? 10 : 4);
        int32_t x0 = cx + sin_deg(deg) * inner / TRIG_ONE;
        int32_t y0 = cy - cos_deg(deg) * inner / TRIG_ONE;
        int32_t x1 = cx + sin_deg(deg) * radius / TRIG_ONE;
        int32_t y1 = cy - cos_deg(deg) * radius / TRIG_ONE;

        gfx_line(c, x0, y0, x1, y1, hour ? COL_TEXT : COL_TEXT_DIM);
    }

    /* Stunden- und Minutenzeiger laufen gleitend: Der Stundenzeiger
     * steht um halb zwoelf zwischen elf und zwoelf und nicht auf elf. */
    int32_t minute_deg = now->minute * 6 + now->second / 10;
    int32_t hour_deg = (now->hour % 12) * 30 + now->minute / 2;

    hand(c, cx, cy, hour_deg, radius * 55 / 100, 3, COL_TEXT);
    hand(c, cx, cy, minute_deg, radius * 80 / 100, 2, COL_TEXT);
    hand(c, cx, cy, now->second * 6, radius * 88 / 100, 1, COL_ACCENT);

    gfx_fill(c, rect_make(cx - 2, cy - 2, 5, 5), COL_TEXT);
}

/* ------------------------------------------------------------------ */

static struct rect tab_rect(struct window *win, int index)
{
    int32_t w = gui_client_width(win) / TAB_COUNT;

    return rect_make(index * w, 0, w - 2, TAB_H - 2);
}

static struct rect action_rect(struct window *win, int index)
{
    int32_t y = gui_client_height(win) - STATUS_H - 40;

    return rect_make(12 + index * 116, y, 108, 30);
}

static void paint_big(struct canvas *c, struct rect area, const char *text,
                      uint32_t colour)
{
    int32_t w = gfx_text_width_scaled(text, 3);

    gfx_text_scaled(c, area.x + (area.w - w) / 2,
                    area.y + (area.h - FONT_HEIGHT * 3) / 2, text, colour, 3,
                    false);
}

static void clock_paint(struct window *win, struct canvas *c)
{
    struct clock_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    static const char *const names[TAB_COUNT] = { "Uhr", "Stoppuhr",
                                                  "Kurzzeit" };

    for (int i = 0; i < TAB_COUNT; i++)
        widget_button(&local, tab_rect(win, i), tr(names[i]),
                      st->tab == i, true);

    struct rect body = rect_make(0, TAB_H, local.w,
                                 local.h - TAB_H - STATUS_H);
    char text[40];

    switch (st->tab) {
    case TAB_CLOCK: {
        struct datetime now;

        rtc_read(&now);

        /* Links das Zifferblatt, rechts die Zahlen - beides
         * nebeneinander, weil beides gefragt ist. */
        struct rect face = rect_make(body.x + 8, body.y + 8,
                                     MIN(body.w / 2, body.h) - 16,
                                     body.h - 16);

        paint_face(&local, face, &now);

        ksnprintf(text, sizeof(text), "%02u:%02u:%02u", (unsigned)now.hour,
                  (unsigned)now.minute, (unsigned)now.second);
        paint_big(&local, rect_make(face.x + face.w, body.y,
                                    body.w - face.w - 16, body.h - 30),
                  text, COL_TEXT);

        ksnprintf(text, sizeof(text), "%02u.%02u.%04u", (unsigned)now.day,
                  (unsigned)now.month, (unsigned)now.year);
        gfx_text(&local, face.x + face.w +
                 (body.w - face.w - 16 - gfx_text_width(text)) / 2,
                 body.y + body.h - 26, text, COL_TEXT_DIM);
        break;
    }
    case TAB_STOP:
        format_stop(stop_value(st), text, sizeof(text));
        paint_big(&local, rect_make(body.x, body.y, body.w, body.h - 50),
                  text, COL_TEXT);

        if (st->lap_ms) {
            char lap[40];

            format_stop(st->lap_ms, lap, sizeof(lap));
            ksnprintf(text, sizeof(text), "%s %s", tr("Zwischenzeit"), lap);
            gfx_text(&local, body.x + (body.w - gfx_text_width(text)) / 2,
                     body.y + body.h - 54, text, COL_TEXT_DIM);
        }

        widget_button(&local, action_rect(win, 0),
                      st->running ? tr("Halt") : tr("Start"),
                      st->hover == 10, true);
        widget_button(&local, action_rect(win, 1), tr("Zwischenzeit"),
                      st->hover == 11, st->running);
        widget_button(&local, action_rect(win, 2), tr("Nullstellen"),
                      st->hover == 12, !st->running);
        break;
    case TAB_TIMER: {
        uint32_t left = timer_left(st);

        format_clock(left, text, sizeof(text));
        paint_big(&local, rect_make(body.x, body.y, body.w, body.h - 50),
                  text, st->rang ? COL_ACCENT : COL_TEXT);

        if (st->rang)
            gfx_text(&local, body.x + 12, body.y + body.h - 54,
                     tr("Die Zeit ist um."), COL_ACCENT);

        widget_button(&local, action_rect(win, 0),
                      st->counting ? tr("Halt") : tr("Start"),
                      st->hover == 10, st->preset_s > 0);
        widget_button(&local, action_rect(win, 1), "+1 min",
                      st->hover == 11, !st->counting);
        widget_button(&local, action_rect(win, 2), tr("Nullstellen"),
                      st->hover == 12, true);
        break;
    }
    default:
        break;
    }

    widget_statusbar(&local,
                     rect_make(0, gui_client_height(win) - STATUS_H,
                               gui_client_width(win), STATUS_H),
                     st->tab == TAB_CLOCK
                         ? tr("Die Uhr kommt aus der Batterieuhr des Rechners.")
                         : tr("Gemessen wird nach dem Systemtakt."),
                     "");
}

static void action(struct window *win, int index)
{
    struct clock_state *st = win->user;

    if (st->tab == TAB_STOP) {
        switch (index) {
        case 0:
            if (st->running) {
                st->collected_ms = stop_value(st);
                st->running = false;
            } else {
                st->started_ms = timer_ms();
                st->running = true;
            }
            break;
        case 1:
            if (st->running)
                st->lap_ms = stop_value(st);
            break;
        default:
            if (!st->running) {
                st->collected_ms = 0;
                st->lap_ms = 0;
            }
            break;
        }
        return;
    }

    if (st->tab != TAB_TIMER)
        return;

    switch (index) {
    case 0:
        if (st->counting) {
            /* Anhalten heisst: den Rest als neue Vorgabe merken. */
            st->preset_s = timer_left(st);
            st->counting = false;
        } else if (st->preset_s > 0) {
            st->ends_ms = timer_ms() + (uint64_t)st->preset_s * 1000;
            st->counting = true;
            st->rang = false;
        }
        break;
    case 1:
        if (!st->counting && st->preset_s < 24 * 3600)
            st->preset_s += 60;
        break;
    default:
        st->counting = false;
        st->rang = false;
        st->preset_s = 0;
        break;
    }
}

static void clock_event(struct window *win, const struct gui_event *ev)
{
    struct clock_state *st = win->user;

    switch (ev->type) {
    case EV_TICK:
        /* Der Kurzzeitmesser meldet sich genau einmal. */
        if (st->counting && timer_left(st) == 0) {
            st->counting = false;
            st->rang = true;
            st->preset_s = 0;
        }
        gui_invalidate();
        return;
    case EV_MOUSE_MOVE: {
        int before = st->hover;

        st->hover = -1;
        for (int i = 0; i < TAB_COUNT; i++)
            if (rect_contains(tab_rect(win, i), ev->x, ev->y))
                st->hover = i;
        for (int i = 0; i < 3; i++)
            if (rect_contains(action_rect(win, i), ev->x, ev->y))
                st->hover = 10 + i;
        if (before != st->hover)
            gui_invalidate();
        return;
    }
    case EV_MOUSE_DOWN:
        for (int i = 0; i < TAB_COUNT; i++) {
            if (rect_contains(tab_rect(win, i), ev->x, ev->y)) {
                st->tab = i;
                gui_invalidate();
                return;
            }
        }
        if (st->tab == TAB_CLOCK)
            return;
        for (int i = 0; i < 3; i++) {
            if (rect_contains(action_rect(win, i), ev->x, ev->y)) {
                action(win, i);
                gui_invalidate();
                return;
            }
        }
        return;
    case EV_KEY_DOWN:
        if (ev->key == KEY_TAB) {
            st->tab = (st->tab + 1) % TAB_COUNT;
        } else if (ev->ascii == ' ') {
            action(win, 0);
        } else {
            return;
        }
        gui_invalidate();
        return;
    default:
        return;
    }
}

static void clock_close(struct window *win)
{
    kfree(win->user);
    win->user = NULL;
}

void app_clock(void)
{
    struct window *existing = gui_find_by_paint(clock_paint);

    if (existing) {
        gui_focus_window(existing);
        return;
    }

    struct clock_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    st->hover = -1;

    struct window *win = gui_create_window(tr("Uhr"), 180, 110, 520, 320,
                                           WF_RESIZABLE, ICON_ALARM);
    if (!win) {
        kfree(st);
        return;
    }

    win->user     = st;
    win->min_w    = 420;
    win->min_h    = 260;
    win->on_paint = clock_paint;
    win->on_event = clock_event;
    win->on_close = clock_close;

    gui_focus_window(win);
}
