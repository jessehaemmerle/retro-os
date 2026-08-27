/* uiapi.c - Fenster fuer Programme in Ring 3. */

#include "uiapi.h"
#include "arch.h"
#include "font.h"
#include "gui.h"
#include "kstring.h"
#include "mm.h"
#include "process.h"
#include "theme.h"
#include "thread.h"

#define UI_QUEUE 64
#define UI_MAX_W 1024
#define UI_MAX_H 768

struct user_window {
    struct window  *win;
    struct process *proc;
    int32_t         handle;

    uint32_t *pixels;
    int32_t   w, h;

    struct user_event queue[UI_QUEUE];
    volatile uint32_t head, tail;

    bool closed;          /* der Benutzer hat das Fenster zugemacht */
};

/* Eine Leinwand ueber den Bildpuffer des Programms. */
static struct canvas window_canvas(struct user_window *uw)
{
    struct canvas c;

    c.px = uw->pixels;
    c.w = uw->w;
    c.h = uw->h;
    c.stride = uw->w;
    c.clip = rect_make(0, 0, uw->w, uw->h);
    return c;
}

static void push_event(struct user_window *uw, const struct user_event *ev)
{
    uint32_t next = (uw->head + 1) % UI_QUEUE;

    if (next == uw->tail)
        return;             /* voll - das aelteste bleibt stehen */

    uw->queue[uw->head] = *ev;
    uw->head = next;
}

/* ------------------------------------------------------------------ */
/* Rueckrufe des Fenstersystems                                        */
/* ------------------------------------------------------------------ */

static void uw_paint(struct window *win, struct canvas *c)
{
    struct user_window *uw = win->user;
    struct canvas local = gui_client_canvas(win, c);

    if (!uw || !uw->pixels) {
        gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);
        return;
    }

    /* Der Bildpuffer des Programms wird eins zu eins uebertragen; was
     * darueber hinausgeht, bleibt Hintergrund. */
    for (int32_t y = 0; y < local.h; y++) {
        for (int32_t x = 0; x < local.w; x++) {
            uint32_t color = (x < uw->w && y < uw->h)
                             ? uw->pixels[(size_t)y * uw->w + x]
                             : COL_FACE;
            gfx_pixel(&local, x, y, color);
        }
    }
}

static void uw_event(struct window *win, const struct gui_event *ev)
{
    struct user_window *uw = win->user;
    struct user_event out;

    if (!uw)
        return;

    memset(&out, 0, sizeof(out));
    out.x = ev->x;
    out.y = ev->y;

    switch (ev->type) {
    case EV_KEY_DOWN:
        out.type = UEV_KEY;
        out.key = ev->key;
        out.ascii = ev->ascii;
        out.mods = ev->mods;
        break;
    case EV_MOUSE_DOWN:
        out.type = UEV_MOUSE_DOWN;
        out.button = ev->button;
        break;
    case EV_MOUSE_UP:
        out.type = UEV_MOUSE_UP;
        out.button = ev->button;
        break;
    case EV_MOUSE_MOVE:
    case EV_MOUSE_DRAG:
        out.type = UEV_MOUSE_MOVE;
        out.button = ev->button;
        break;
    default:
        return;
    }

    push_event(uw, &out);
}

/* Der Benutzer hat auf das Kreuz geklickt. Das Fenster verschwindet,
 * der Bildpuffer bleibt noch - das Programm liest gleich sein
 * Schliessereignis und beendet sich selbst. */
static void uw_close(struct window *win)
{
    struct user_window *uw = win->user;
    struct user_event out;

    if (!uw)
        return;

    memset(&out, 0, sizeof(out));
    out.type = UEV_CLOSE;
    push_event(uw, &out);

    uw->closed = true;
    uw->win = NULL;
    win->user = NULL;
}

/* ------------------------------------------------------------------ */
/* Die Aufrufe                                                         */
/* ------------------------------------------------------------------ */

static struct user_window *lookup(struct process *proc, int32_t handle)
{
    if (!proc || handle < 0 || handle >= PROCESS_WINDOWS_MAX)
        return NULL;
    return proc->windows[handle];
}

int64_t uiapi_open(struct process *proc, const char *title,
                   int32_t width, int32_t height)
{
    if (width < 80 || height < 60 || width > UI_MAX_W || height > UI_MAX_H)
        return SYS_ERR_INVAL;

    int32_t handle = -1;

    for (int32_t i = 0; i < PROCESS_WINDOWS_MAX; i++) {
        if (!proc->windows[i]) {
            handle = i;
            break;
        }
    }
    if (handle < 0)
        return SYS_ERR_INVAL;

    struct user_window *uw = kzalloc(sizeof(*uw));

    if (!uw)
        return SYS_ERR_NOMEM;

    uw->pixels = kzalloc((size_t)width * height * sizeof(uint32_t));
    if (!uw->pixels) {
        kfree(uw);
        return SYS_ERR_NOMEM;
    }

    uw->proc = proc;
    uw->handle = handle;
    uw->w = width;
    uw->h = height;

    /* Anfangs grau - so sieht das Fenster nicht kaputt aus, bevor das
     * Programm zum ersten Mal gezeichnet hat. */
    for (size_t i = 0; i < (size_t)width * height; i++)
        uw->pixels[i] = COL_FACE;

    struct window *win = gui_create_window(title && title[0] ? title
                                                             : "Programm",
                                           0, 0, width, height,
                                           WF_CENTER, ICON_TERMINAL);
    if (!win) {
        kfree(uw->pixels);
        kfree(uw);
        return SYS_ERR_NOMEM;
    }

    win->user     = uw;
    win->on_paint = uw_paint;
    win->on_event = uw_event;
    win->on_close = uw_close;
    uw->win = win;

    proc->windows[handle] = uw;
    gui_focus_window(win);
    return handle;
}

int64_t uiapi_draw(struct process *proc, int32_t handle,
                   const struct user_draw_cmd *cmds, size_t count)
{
    struct user_window *uw = lookup(proc, handle);

    if (!uw)
        return SYS_ERR_BADFD;
    if (uw->closed)
        return SYS_ERR_BADFD;

    struct canvas c = window_canvas(uw);

    for (size_t i = 0; i < count; i++) {
        const struct user_draw_cmd *cmd = &cmds[i];
        struct rect r = rect_make(cmd->x, cmd->y, cmd->w, cmd->h);
        char text[USER_DRAW_TEXT_MAX + 1];

        switch (cmd->op) {
        case UDRAW_CLEAR:
            gfx_fill(&c, rect_make(0, 0, uw->w, uw->h), cmd->color);
            break;
        case UDRAW_FILL:
            gfx_fill(&c, r, cmd->color);
            break;
        case UDRAW_RECT:
            gfx_frame(&c, r, cmd->color);
            break;
        case UDRAW_LINE:
            gfx_line(&c, cmd->x, cmd->y, cmd->w, cmd->h, cmd->color);
            break;
        case UDRAW_PIXEL:
            gfx_pixel(&c, cmd->x, cmd->y, cmd->color);
            break;
        case UDRAW_TEXT:
            memcpy(text, cmd->text, USER_DRAW_TEXT_MAX);
            text[USER_DRAW_TEXT_MAX] = '\0';
            gfx_text(&c, cmd->x, cmd->y, text, cmd->color);
            break;
        default:
            return SYS_ERR_INVAL;
        }
    }

    gui_invalidate();
    return 0;
}

int64_t uiapi_event(struct process *proc, int32_t handle,
                    struct user_event *out, uint32_t timeout_ms)
{
    struct user_window *uw = lookup(proc, handle);

    if (!uw)
        return SYS_ERR_BADFD;

    uint64_t deadline = timer_ms() + timeout_ms;

    for (;;) {
        if (uw->tail != uw->head) {
            *out = uw->queue[uw->tail];
            uw->tail = (uw->tail + 1) % UI_QUEUE;
            return 1;
        }
        if (timeout_ms == 0 || timer_ms() >= deadline)
            return 0;

        thread_sleep(5);
    }
}

static void destroy(struct user_window *uw)
{
    if (uw->win) {
        uw->win->user = NULL;      /* uw_close soll nichts mehr tun */
        gui_close_window(uw->win);
        uw->win = NULL;
    }
    kfree(uw->pixels);
    kfree(uw);
}

int64_t uiapi_close(struct process *proc, int32_t handle)
{
    struct user_window *uw = lookup(proc, handle);

    if (!uw)
        return SYS_ERR_BADFD;

    proc->windows[handle] = NULL;
    destroy(uw);
    gui_invalidate();
    return 0;
}

void uiapi_release(struct process *proc)
{
    for (int32_t i = 0; i < PROCESS_WINDOWS_MAX; i++) {
        if (proc->windows[i]) {
            destroy(proc->windows[i]);
            proc->windows[i] = NULL;
        }
    }
    gui_invalidate();
}
