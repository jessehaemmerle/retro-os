/* window.c - Fensterverwaltung, Bildaufbau und Ereignisverteilung.
 *
 * Aufbau eines Einzelbildes:
 *   1. Hintergrund und Symbole des Desktops
 *   2. alle sichtbaren Fenster von hinten nach vorne, jeweils Rahmen,
 *      Titelleiste und Inhalt
 *   3. ein eventuell geoeffnetes Menue
 *   4. die Taskleiste
 *   5. der Mauszeiger
 *
 * Neu gezeichnet wird nur, wenn sich etwas geaendert hat. Bewegt sich
 * ausschliesslich die Maus, wird lediglich der Bereich unter dem Zeiger
 * wiederhergestellt und an der neuen Stelle neu gezeichnet - das kostet
 * ein paar hundert Pixel statt eines ganzen Bildschirms.
 */

#include "gui.h"
#include "arch.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"
#include "net.h"
#include "lock.h"
#include "thread.h"
#include "lang.h"
#include "theme.h"

#define CURSOR_W 12
#define CURSOR_H 19
#define DOUBLE_CLICK_MS 400

/* ' ' durchsichtig, '.' schwarze Kontur, 'W' weisse Fuellung */
static const char *cursor_shape[CURSOR_H] = {
    ".           ",
    "..          ",
    ".W.         ",
    ".WW.        ",
    ".WWW.       ",
    ".WWWW.      ",
    ".WWWWW.     ",
    ".WWWWWW.    ",
    ".WWWWWWW.   ",
    ".WWWWWWWW.  ",
    ".WWWWWWWWW. ",
    ".WWWWWW.....",
    ".WWWWWW.    ",
    ".WWW.WWW.   ",
    ".WW. .WWW.  ",
    ".W.   .WWW. ",
    "..     .WWW.",
    ".       .WW.",
    "         ...",
};

static struct window windows[GUI_MAX_WINDOWS];
static struct window *stack[GUI_MAX_WINDOWS];   /* [0] = hinten */
static size_t stack_count;

static bool     dirty = true;
static int32_t  cursor_x, cursor_y;
static int32_t  cursor_saved_x = -1, cursor_saved_y = -1;
static uint32_t cursor_backup[CURSOR_W * CURSOR_H];
static bool     cursor_valid;

/* Zustand des Ziehens von Fenstern */
static struct window *drag_win;
static bool     drag_resize;
static int32_t  drag_off_x, drag_off_y;

/* Zustand des Popup-Menues */
static struct menu_item  menu_items[MENU_MAX_ITEMS];
static size_t            menu_count;
static struct rect       menu_rect;
static menu_select_fn    menu_cb;
static void             *menu_user;
static bool              menu_active;
static int               menu_hover = -1;

static uint64_t last_click_ms;
static int32_t  last_click_x, last_click_y;

/* ------------------------------------------------------------------ */
/* Fensterliste                                                        */
/* ------------------------------------------------------------------ */

size_t gui_window_count(void) { return stack_count; }

struct window *gui_window_at(size_t index)
{
    return index < stack_count ? stack[index] : NULL;
}

struct window *gui_focused(void)
{
    for (size_t i = stack_count; i > 0; i--) {
        if (stack[i - 1]->visible && !stack[i - 1]->minimized)
            return stack[i - 1];
    }
    return NULL;
}

bool gui_window_alive(const struct window *win)
{
    if (!win)
        return false;

    for (size_t i = 0; i < stack_count; i++) {
        if (stack[i] == win)
            return true;
    }
    return false;
}

struct rect gui_client_rect(const struct window *win)
{
    /* Ein rahmenloses Fenster ist ganz Inhalt. */
    if (win->flags & WF_BARE)
        return win->frame;

    return rect_make(win->frame.x + BORDER_WIDTH,
                     win->frame.y + BORDER_WIDTH + TITLEBAR_HEIGHT,
                     win->frame.w - 2 * BORDER_WIDTH,
                     win->frame.h - 2 * BORDER_WIDTH - TITLEBAR_HEIGHT);
}

struct canvas gui_client_canvas(const struct window *win, const struct canvas *c)
{
    struct rect client = gui_client_rect(win);
    struct canvas local = *c;

    local.px   = &c->px[client.y * c->stride + client.x];
    local.w    = client.w;
    local.h    = client.h;
    local.clip = rect_make(0, 0, client.w, client.h);

    struct rect vis = rect_intersect(c->clip, client);
    local.clip = rect_intersect(local.clip,
                                rect_make(vis.x - client.x, vis.y - client.y,
                                          vis.w, vis.h));
    return local;
}

int32_t gui_client_width(const struct window *win)
{
    return gui_client_rect(win).w;
}

int32_t gui_client_height(const struct window *win)
{
    return gui_client_rect(win).h;
}

void gui_invalidate(void) { dirty = true; }

struct window *gui_create_window(const char *title, int32_t x, int32_t y,
                                 int32_t w, int32_t h, uint32_t flags,
                                 enum icon_id icon)
{
    if (stack_count >= GUI_MAX_WINDOWS)
        return NULL;

    struct window *win = NULL;
    for (size_t i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!windows[i].used) {
            win = &windows[i];
            break;
        }
    }
    if (!win)
        return NULL;

    memset(win, 0, sizeof(*win));
    strlcpy(win->title, title, sizeof(win->title));

    struct canvas *screen = gfx_screen();
    if (flags & WF_CENTER) {
        x = (screen->w - w) / 2;
        y = (desktop_work_height() - h) / 2;
    }

    win->frame   = rect_make(x, y, w, h);
    win->flags   = flags;
    win->icon    = icon;
    win->used    = true;
    win->visible = true;
    win->min_w   = 200;
    win->min_h   = 120;

    stack[stack_count++] = win;
    dirty = true;
    return win;
}

void gui_close_window(struct window *win)
{
    if (!win || !win->used)
        return;

    if (win->on_close)
        win->on_close(win);

    for (size_t i = 0; i < stack_count; i++) {
        if (stack[i] == win) {
            for (size_t k = i; k + 1 < stack_count; k++)
                stack[k] = stack[k + 1];
            stack_count--;
            break;
        }
    }

    if (drag_win == win)
        drag_win = NULL;

    win->used = false;
    dirty = true;
}

void gui_focus_window(struct window *win)
{
    if (!win || !win->used)
        return;

    win->minimized = false;

    for (size_t i = 0; i < stack_count; i++) {
        if (stack[i] != win)
            continue;

        struct window *old = gui_focused();
        for (size_t k = i; k + 1 < stack_count; k++)
            stack[k] = stack[k + 1];
        stack[stack_count - 1] = win;

        if (old && old != win && old->on_event) {
            struct gui_event ev = { .type = EV_BLUR };
            old->on_event(old, &ev);
        }
        if (win->on_event) {
            struct gui_event ev = { .type = EV_FOCUS };
            win->on_event(win, &ev);
        }
        break;
    }
    dirty = true;
}

void gui_set_title(struct window *win, const char *title)
{
    if (!win)
        return;
    strlcpy(win->title, title, sizeof(win->title));
    dirty = true;
}

struct window *gui_find_by_paint(win_paint_fn fn)
{
    for (size_t i = 0; i < stack_count; i++) {
        if (stack[i]->on_paint == fn)
            return stack[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Menue                                                               */
/* ------------------------------------------------------------------ */

#define MENU_ITEM_H 20
#define MENU_SEP_H  7

void gui_open_menu(int32_t x, int32_t y, const struct menu_item *items,
                   size_t count, menu_select_fn on_select, void *user)
{
    if (count > MENU_MAX_ITEMS)
        count = MENU_MAX_ITEMS;

    memcpy(menu_items, items, count * sizeof(struct menu_item));
    menu_count = count;
    menu_cb    = on_select;
    menu_user  = user;
    menu_hover = -1;

    int32_t width = 120, height = 8;
    for (size_t i = 0; i < count; i++) {
        if (!items[i].label) {
            height += MENU_SEP_H;
            continue;
        }
        height += MENU_ITEM_H;
        int32_t w = gfx_text_width(tr(items[i].label)) + 56;
        if (w > width)
            width = w;
    }

    struct canvas *screen = gfx_screen();
    if (x + width > screen->w)
        x = screen->w - width;
    if (y + height > screen->h)
        y = screen->h - height;

    menu_rect   = rect_make(MAX(x, 0), MAX(y, 0), width, height);
    menu_active = true;
    dirty = true;
}

void gui_close_menu(void)
{
    if (menu_active) {
        menu_active = false;
        dirty = true;
    }
}

bool gui_menu_open(void) { return menu_active; }

static int menu_item_at(int32_t y)
{
    int32_t row = menu_rect.y + 4;

    for (size_t i = 0; i < menu_count; i++) {
        int32_t h = menu_items[i].label ? MENU_ITEM_H : MENU_SEP_H;

        if (y >= row && y < row + h)
            return menu_items[i].label ? (int)i : -1;
        row += h;
    }
    return -1;
}

static void menu_paint(struct canvas *c)
{
    /* Schlagschatten */
    gfx_fill(c, rect_make(menu_rect.x + 4, menu_rect.y + menu_rect.h,
                          menu_rect.w, 4), RGB(0x30, 0x40, 0x48));
    gfx_fill(c, rect_make(menu_rect.x + menu_rect.w, menu_rect.y + 4,
                          4, menu_rect.h), RGB(0x30, 0x40, 0x48));

    gfx_fill(c, menu_rect, COL_FACE);
    gfx_bevel(c, menu_rect, true);

    int32_t row = menu_rect.y + 4;
    for (size_t i = 0; i < menu_count; i++) {
        const struct menu_item *it = &menu_items[i];

        if (!it->label) {
            gfx_hline(c, menu_rect.x + 6, row + 3, menu_rect.w - 12, COL_SHADOW);
            gfx_hline(c, menu_rect.x + 6, row + 4, menu_rect.w - 12, COL_HILIGHT);
            row += MENU_SEP_H;
            continue;
        }

        bool hovered = ((int)i == menu_hover) && it->enabled;
        if (hovered)
            gfx_fill(c, rect_make(menu_rect.x + 3, row, menu_rect.w - 6, MENU_ITEM_H),
                     COL_SELECT);

        if (it->has_icon)
            icon_draw(c, menu_rect.x + 6, row + 2, it->icon, 1);

        uint32_t color = !it->enabled ? COL_TEXT_DIM
                       : hovered      ? COL_SELECT_TEXT
                                      : COL_TEXT;
        gfx_text(c, menu_rect.x + 28, row + 2, tr(it->label), color);
        row += MENU_ITEM_H;
    }
}

/* ------------------------------------------------------------------ */
/* Fenster zeichnen                                                    */
/* ------------------------------------------------------------------ */

static struct rect button_rect(const struct window *win, int index)
{
    int32_t size = TITLEBAR_HEIGHT - 6;
    int32_t x = win->frame.x + win->frame.w - BORDER_WIDTH - 2
              - (index + 1) * (size + 2);

    return rect_make(x, win->frame.y + BORDER_WIDTH + 3, size, size);
}

static void draw_close_glyph(struct canvas *c, struct rect r)
{
    int32_t cx = r.x + r.w / 2, cy = r.y + r.h / 2;

    for (int32_t i = -3; i <= 3; i++) {
        gfx_pixel(c, cx + i, cy + i, COL_BLACK);
        gfx_pixel(c, cx + i + 1, cy + i, COL_BLACK);
        gfx_pixel(c, cx + i, cy - i, COL_BLACK);
        gfx_pixel(c, cx + i + 1, cy - i, COL_BLACK);
    }
}

static void draw_minimize_glyph(struct canvas *c, struct rect r)
{
    gfx_fill(c, rect_make(r.x + 4, r.y + r.h - 7, r.w - 8, 2), COL_BLACK);
}

static void window_paint(struct canvas *c, struct window *win, bool focused)
{
    struct rect f = win->frame;

    /* Rahmenlos: nur der Inhalt, sonst nichts. */
    if (win->flags & WF_BARE) {
        if (win->on_paint) {
            struct canvas inner = *c;

            gfx_set_clip(&inner, rect_intersect(c->clip, f));
            win->on_paint(win, &inner);
        }
        return;
    }

    /* Rahmen */
    gfx_fill(c, f, COL_FACE);
    gfx_bevel(c, f, true);

    /* Titelleiste */
    struct rect title = rect_make(f.x + BORDER_WIDTH, f.y + BORDER_WIDTH,
                                  f.w - 2 * BORDER_WIDTH, TITLEBAR_HEIGHT);
    if (focused)
        gfx_gradient_h(c, title, COL_TITLE_A1, COL_TITLE_A2);
    else
        gfx_gradient_h(c, title, COL_TITLE_I1, COL_TITLE_I2);

    icon_draw(c, title.x + 3, title.y + 3, win->icon, 1);

    int32_t buttons = 0;
    if (!(win->flags & WF_NO_CLOSE)) buttons++;
    if (!(win->flags & WF_NO_MIN))   buttons++;

    int32_t text_room = title.w - 26 - buttons * (TITLEBAR_HEIGHT - 4) - 8;
    gfx_set_clip(c, rect_intersect(c->clip, title));
    gfx_text_clipped(c, title.x + 24, title.y + 3, tr(win->title),
                     COL_TITLE_TEXT, text_room);
    gfx_reset_clip(c);

    int index = 0;
    if (!(win->flags & WF_NO_CLOSE)) {
        struct rect r = button_rect(win, index++);
        gfx_fill(c, r, COL_FACE);
        gfx_bevel_thin(c, r, true);
        draw_close_glyph(c, r);
    }
    if (!(win->flags & WF_NO_MIN)) {
        struct rect r = button_rect(win, index++);
        gfx_fill(c, r, COL_FACE);
        gfx_bevel_thin(c, r, true);
        draw_minimize_glyph(c, r);
    }

    /* Inhalt */
    struct rect client = gui_client_rect(win);
    gfx_fill(c, client, COL_FACE);
    gfx_bevel_thin(c, rect_make(client.x - 1, client.y - 1,
                                client.w + 2, client.h + 2), false);

    if (win->on_paint) {
        struct canvas inner = *c;
        gfx_set_clip(&inner, rect_intersect(c->clip, client));
        win->on_paint(win, &inner);
    }

    /* Anfasser zum Vergroessern */
    if (win->flags & WF_RESIZABLE) {
        int32_t gx = f.x + f.w - 14, gy = f.y + f.h - 14;
        for (int32_t i = 0; i < 3; i++) {
            int32_t o = i * 4;
            gfx_line(c, gx + o + 8, gy + 11, gx + 11, gy + o + 8, COL_HILIGHT);
            gfx_line(c, gx + o + 9, gy + 11, gx + 11, gy + o + 9, COL_SHADOW);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Mauszeiger                                                          */
/* ------------------------------------------------------------------ */

static void cursor_save(struct canvas *c, int32_t x, int32_t y)
{
    for (int32_t row = 0; row < CURSOR_H; row++) {
        for (int32_t col = 0; col < CURSOR_W; col++) {
            int32_t px = x + col, py = y + row;

            cursor_backup[row * CURSOR_W + col] =
                (px >= 0 && px < c->w && py >= 0 && py < c->h)
                    ? c->px[py * c->stride + px] : 0;
        }
    }
    cursor_saved_x = x;
    cursor_saved_y = y;
    cursor_valid = true;
}

static void cursor_restore(struct canvas *c)
{
    if (!cursor_valid)
        return;

    for (int32_t row = 0; row < CURSOR_H; row++) {
        for (int32_t col = 0; col < CURSOR_W; col++) {
            int32_t px = cursor_saved_x + col, py = cursor_saved_y + row;

            if (px >= 0 && px < c->w && py >= 0 && py < c->h)
                c->px[py * c->stride + px] = cursor_backup[row * CURSOR_W + col];
        }
    }
    cursor_valid = false;
}

static void cursor_draw(struct canvas *c, int32_t x, int32_t y)
{
    for (int32_t row = 0; row < CURSOR_H; row++) {
        const char *line = cursor_shape[row];

        for (int32_t col = 0; col < CURSOR_W && line[col]; col++) {
            if (line[col] == '.')
                gfx_pixel(c, x + col, y + row, COL_BLACK);
            else if (line[col] == 'W')
                gfx_pixel(c, x + col, y + row, COL_WHITE);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Bildaufbau                                                          */
/* ------------------------------------------------------------------ */

static void compose(void)
{
    struct canvas *c = gfx_screen();

    cursor_valid = false;
    gfx_reset_clip(c);

    /* Solange gesperrt ist, gibt es nichts anderes zu sehen. Die Fenster
     * bleiben unter der Sperre stehen, aber niemand kommt an sie heran. */
    if (lock_active()) {
        lock_paint(c);
        return;
    }

    desktop_paint_background(c);

    struct window *focus = gui_focused();
    for (size_t i = 0; i < stack_count; i++) {
        struct window *win = stack[i];

        if (win->visible && !win->minimized)
            window_paint(c, win, win == focus);
    }

    /* Die Taskleiste bleibt sichtbar - es sei denn, ein rahmenloses
     * Fenster liegt ganz oben und beansprucht den Bildschirm. */
    struct window *top = stack_count ? stack[stack_count - 1] : NULL;
    bool fullscreen = top && top->visible && !top->minimized &&
                      (top->flags & WF_BARE);

    if (!fullscreen)
        desktop_paint_taskbar(c);

    if (menu_active)
        menu_paint(c);
}

static void present(void)
{
    struct canvas *c = gfx_screen();

    if (dirty) {
        compose();
        cursor_save(c, cursor_x, cursor_y);
        cursor_draw(c, cursor_x, cursor_y);
        gfx_flush();
        dirty = false;
        return;
    }

    /* Nur der Zeiger hat sich bewegt. */
    if (cursor_valid && cursor_saved_x == cursor_x && cursor_saved_y == cursor_y)
        return;

    struct rect old = rect_make(cursor_saved_x, cursor_saved_y, CURSOR_W, CURSOR_H);
    bool had_old = cursor_valid;

    cursor_restore(c);
    cursor_save(c, cursor_x, cursor_y);
    cursor_draw(c, cursor_x, cursor_y);

    if (had_old)
        gfx_flush_rect(old);
    gfx_flush_rect(rect_make(cursor_x, cursor_y, CURSOR_W, CURSOR_H));
}

/* ------------------------------------------------------------------ */
/* Ereignisse                                                          */
/* ------------------------------------------------------------------ */

static struct window *window_at(int32_t x, int32_t y)
{
    for (size_t i = stack_count; i > 0; i--) {
        struct window *win = stack[i - 1];

        if (win->visible && !win->minimized && rect_contains(win->frame, x, y))
            return win;
    }
    return NULL;
}

static void send(struct window *win, struct gui_event *ev)
{
    if (win && win->on_event)
        win->on_event(win, ev);
}

static void to_client(const struct window *win, struct gui_event *ev,
                      int32_t x, int32_t y)
{
    struct rect client = gui_client_rect(win);

    ev->x = x - client.x;
    ev->y = y - client.y;
}

static void handle_mouse_down(int32_t x, int32_t y, uint8_t button, bool dbl)
{
    if (lock_active()) {
        lock_mouse(x, y, button, true);
        return;
    }

    if (menu_active) {
        if (rect_contains(menu_rect, x, y)) {
            int idx = menu_item_at(y);

            if (idx >= 0 && menu_items[idx].enabled) {
                int id = menu_items[idx].id;
                menu_select_fn cb = menu_cb;
                void *user = menu_user;

                gui_close_menu();
                if (cb)
                    cb(id, user);
            }
            return;
        }
        gui_close_menu();
        /* Klick ausserhalb schliesst das Menue und wirkt sonst nicht. */
        return;
    }

    struct window *win = window_at(x, y);

    if (!win) {
        desktop_mouse(x, y, button, true, dbl);
        return;
    }

    if (win != gui_focused())
        gui_focus_window(win);

    struct rect f = win->frame;

    /* Ein rahmenloses Fenster hat weder Leiste noch Knoepfe - jeder
     * Klick geht an den Inhalt. */
    if (win->flags & WF_BARE) {
        struct gui_event ev = {
            .type = dbl ? EV_DOUBLE_CLICK : EV_MOUSE_DOWN,
            .button = button,
        };

        to_client(win, &ev, x, y);
        send(win, &ev);
        return;
    }

    struct rect title = rect_make(f.x + BORDER_WIDTH, f.y + BORDER_WIDTH,
                                  f.w - 2 * BORDER_WIDTH, TITLEBAR_HEIGHT);

    /* Knoepfe der Titelleiste */
    int index = 0;
    if (!(win->flags & WF_NO_CLOSE)) {
        if (rect_contains(button_rect(win, index), x, y)) {
            gui_close_window(win);
            return;
        }
        index++;
    }
    if (!(win->flags & WF_NO_MIN)) {
        if (rect_contains(button_rect(win, index), x, y)) {
            win->minimized = true;
            dirty = true;
            return;
        }
    }

    if (rect_contains(title, x, y)) {
        drag_win    = win;
        drag_resize = false;
        drag_off_x  = x - f.x;
        drag_off_y  = y - f.y;
        return;
    }

    if ((win->flags & WF_RESIZABLE) &&
        x >= f.x + f.w - 16 && y >= f.y + f.h - 16) {
        drag_win    = win;
        drag_resize = true;
        drag_off_x  = f.x + f.w - x;
        drag_off_y  = f.y + f.h - y;
        return;
    }

    struct rect client = gui_client_rect(win);
    if (rect_contains(client, x, y)) {
        struct gui_event ev = {
            .type = dbl ? EV_DOUBLE_CLICK : EV_MOUSE_DOWN,
            .button = button,
        };
        to_client(win, &ev, x, y);
        send(win, &ev);
    }
}

static void handle_mouse_up(int32_t x, int32_t y, uint8_t button)
{
    if (lock_active()) {
        lock_mouse(x, y, button, false);
        return;
    }

    if (drag_win) {
        drag_win = NULL;
        return;
    }

    struct window *win = window_at(x, y);
    if (!win) {
        desktop_mouse(x, y, button, false, false);
        return;
    }

    struct gui_event ev = { .type = EV_MOUSE_UP, .button = button };
    to_client(win, &ev, x, y);
    send(win, &ev);
}

static void handle_mouse_move(int32_t x, int32_t y, bool pressed)
{
    if (lock_active()) {
        lock_mouse(x, y, 0, false);
        return;
    }

    if (drag_win) {
        struct canvas *screen = gfx_screen();

        if (drag_resize) {
            int32_t w = x + drag_off_x - drag_win->frame.x;
            int32_t h = y + drag_off_y - drag_win->frame.y;

            drag_win->frame.w = CLAMP(w, drag_win->min_w, screen->w);
            drag_win->frame.h = CLAMP(h, drag_win->min_h, screen->h);

            struct gui_event ev = { .type = EV_RESIZED };
            send(drag_win, &ev);
        } else {
            drag_win->frame.x = x - drag_off_x;
            drag_win->frame.y = CLAMP(y - drag_off_y, 0,
                                      desktop_work_height() - TITLEBAR_HEIGHT);
        }
        dirty = true;
        return;
    }

    if (menu_active) {
        int idx = rect_contains(menu_rect, x, y) ? menu_item_at(y) : -1;

        if (idx != menu_hover) {
            menu_hover = idx;
            dirty = true;
        }
        return;
    }

    struct window *win = window_at(x, y);
    if (!win)
        return;

    struct rect client = gui_client_rect(win);
    if (!rect_contains(client, x, y))
        return;

    struct gui_event ev = { .type = pressed ? EV_MOUSE_DRAG : EV_MOUSE_MOVE };
    to_client(win, &ev, x, y);
    send(win, &ev);
}

static void handle_key(struct key_event *ke)
{
    if (lock_active()) {
        lock_key(ke);
        return;
    }

    if (menu_active && ke->pressed && ke->key == KEY_ESCAPE) {
        gui_close_menu();
        return;
    }

    struct window *win = gui_focused();
    if (!win)
        return;

    struct gui_event ev = {
        .type  = ke->pressed ? EV_KEY_DOWN : EV_KEY_UP,
        .key   = ke->key,
        .ascii = ke->ascii,
        .mods  = ke->mods,
    };
    send(win, &ev);
}

void gui_init(void)
{
    struct canvas *screen = gfx_screen();

    memset(windows, 0, sizeof(windows));
    stack_count = 0;

    cursor_x = screen->w / 2;
    cursor_y = screen->h / 2;
    mouse_set_position(cursor_x, cursor_y);

    desktop_init();
    dirty = true;
}

NORETURN void gui_run(void)
{
    struct mouse_state ms;
    struct key_event   ke;
    bool     prev_left = false, prev_right = false;
    uint64_t last_tick = 0;

    for (;;) {
        bool activity = false;

        while (keyboard_poll(&ke)) {
            handle_key(&ke);
            activity = true;
        }

        if (mouse_poll(&ms)) {
            activity = true;

            if (ms.x != cursor_x || ms.y != cursor_y) {
                cursor_x = ms.x;
                cursor_y = ms.y;
                handle_mouse_move(cursor_x, cursor_y, prev_left);
            }

            if (ms.left && !prev_left) {
                uint64_t now = timer_ms();
                bool dbl = (now - last_click_ms) < DOUBLE_CLICK_MS &&
                           MAX(cursor_x - last_click_x, last_click_x - cursor_x) < 6 &&
                           MAX(cursor_y - last_click_y, last_click_y - cursor_y) < 6;

                last_click_ms = dbl ? 0 : now;
                last_click_x  = cursor_x;
                last_click_y  = cursor_y;
                handle_mouse_down(cursor_x, cursor_y, MB_LEFT, dbl);
            } else if (!ms.left && prev_left) {
                handle_mouse_up(cursor_x, cursor_y, MB_LEFT);
            }

            if (ms.right && !prev_right)
                handle_mouse_down(cursor_x, cursor_y, MB_RIGHT, false);
            else if (!ms.right && prev_right)
                handle_mouse_up(cursor_x, cursor_y, MB_RIGHT);

            if (ms.scroll && !lock_active()) {
                struct window *win = window_at(cursor_x, cursor_y);

                if (win) {
                    struct gui_event ev = { .type = EV_SCROLL, .scroll = ms.scroll };
                    to_client(win, &ev, cursor_x, cursor_y);
                    send(win, &ev);
                }
            }

            prev_left  = ms.left;
            prev_right = ms.right;
        }

        /* Zehn Mal pro Sekunde bekommt das aktive Fenster einen Taktimpuls -
         * damit blinken Textmarken und laeuft die Uhr weiter. */
        uint64_t now = timer_ms();
        if (now - last_tick >= 100) {
            last_tick = now;

            if (lock_active()) {
                lock_tick();
            } else {
                desktop_tick();

                struct window *win = gui_focused();
                if (win) {
                    struct gui_event ev = { .type = EV_TICK };
                    send(win, &ev);
                }
            }
        }

        present();


        /* Kurz schlafen statt anhalten: so kommen die Threads im
         * Hintergrund zum Zug, ohne dass die Bedienung traege wirkt. */
        if (!activity)
            thread_sleep(4);
    }
}
