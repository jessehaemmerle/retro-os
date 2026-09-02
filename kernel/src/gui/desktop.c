/* desktop.c - Hintergrund, Symbole auf dem Desktop, Taskleiste, Startmenue. */

#include "config.h"
#include "gui.h"
#include "apps.h"
#include "arch.h"
#include "font.h"
#include "kstring.h"
#include "lock.h"
#include "power.h"
#include "user.h"
#include "rtc.h"
#include "theme.h"

#define ICON_CELL_W    104
#define ICON_CELL_H    88
#define ICON_TOP       16
#define ICON_LEFT      12
#define START_WIDTH    92

#define MENU_ID_APPS   100
#define MENU_ID_REBOOT 200
#define MENU_ID_SHUTDOWN 201
#define MENU_ID_LOCK   202
#define MENU_ID_LOGOUT 203
#define MENU_ID_SWITCH 204

static int      selected_icon = -1;
static bool     start_pressed;
static char     clock_text[16];

int32_t desktop_work_height(void)
{
    return gfx_screen()->h - TASKBAR_HEIGHT;
}

/* --- Symbole auf dem Desktop --------------------------------------- */

static size_t desktop_icon_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < app_count; i++) {
        if (app_list[i].on_desktop)
            n++;
    }
    return n;
}

static const struct app_entry *desktop_icon_at_index(size_t index)
{
    size_t n = 0;

    for (size_t i = 0; i < app_count; i++) {
        if (!app_list[i].on_desktop)
            continue;
        if (n++ == index)
            return &app_list[i];
    }
    return NULL;
}

static struct rect desktop_icon_rect(size_t index)
{
    int32_t per_column = (desktop_work_height() - ICON_TOP) / ICON_CELL_H;
    if (per_column < 1)
        per_column = 1;

    int32_t col = (int32_t)index / per_column;
    int32_t row = (int32_t)index % per_column;

    return rect_make(ICON_LEFT + col * ICON_CELL_W,
                     ICON_TOP + row * ICON_CELL_H,
                     ICON_CELL_W - 8, ICON_CELL_H - 8);
}

/* --- Hintergrund ---------------------------------------------------- */

/* Hellt eine Farbe kanalweise auf, ohne dass ein Kanal ueberlaeuft. */
static uint32_t lighten(uint32_t color, int32_t amount)
{
    uint32_t out = 0;

    for (int shift = 0; shift <= 16; shift += 8) {
        int32_t v = (int32_t)((color >> shift) & 0xFF) + amount;
        out |= (uint32_t)CLAMP(v, 0, 255) << shift;
    }
    return out;
}

static void paint_wallpaper(struct canvas *c)
{
    struct rect area = rect_make(0, 0, c->w, desktop_work_height());

    uint32_t top, bottom;

    background_colors(config_current()->background, &top, &bottom);
    gfx_gradient_v(c, area, top, bottom);

    /* Ein dezentes Rautenmuster - kostet kaum Rechenzeit und nimmt der
     * Flaeche die Leere. */
    for (int32_t y = 0; y < area.h; y += 8) {
        for (int32_t x = ((y / 8) & 1) ? 4 : 0; x < area.w; x += 8)
            gfx_pixel(c, x, y, lighten(c->px[y * c->stride + x], 14));
    }
}

/* Bricht eine Beschriftung auf hoechstens zwei Zeilen um. Getrennt wird an
 * einem Leerzeichen oder Bindestrich, sonst hart. */
static int32_t wrap_label(const char *name, int32_t max_w, char out[2][24])
{
    int32_t per_line = MAX(max_w / FONT_WIDTH, 4);
    int32_t len = (int32_t)strlen(name);

    if (len <= per_line) {
        strlcpy(out[0], name, 24);
        return 1;
    }

    int32_t cut = per_line;
    for (int32_t i = per_line; i > per_line / 2; i--) {
        if (name[i] == ' ' || name[i] == '-') {
            cut = i;
            break;
        }
    }

    strlcpy(out[0], name, (size_t)MIN(cut + 1, 24));
    strlcpy(out[1], name + cut + (name[cut] == ' ' ? 1 : 0), 24);
    return 2;
}

static void paint_desktop_icons(struct canvas *c)
{
    size_t count = desktop_icon_count();

    for (size_t i = 0; i < count; i++) {
        const struct app_entry *app = desktop_icon_at_index(i);
        struct rect cell = desktop_icon_rect(i);
        bool sel = ((int)i == selected_icon);

        int32_t ix = cell.x + (cell.w - 32) / 2;

        icon_draw(c, ix, cell.y,
                  app->icon_now ? app->icon_now() : app->icon, 2);

        char    line[2][24];
        int32_t lines = wrap_label(app->name, cell.w, line);
        int32_t ty = cell.y + 38;

        for (int32_t l = 0; l < lines; l++) {
            int32_t tw = gfx_text_width(line[l]);
            int32_t tx = cell.x + (cell.w - tw) / 2;
            int32_t ly = ty + l * (FONT_HEIGHT + 1);

            if (sel) {
                gfx_fill(c, rect_make(tx - 3, ly - 2, tw + 6, FONT_HEIGHT + 3),
                         COL_SELECT);
                gfx_text(c, tx, ly, line[l], COL_SELECT_TEXT);
            } else {
                /* Schatten macht die Beschriftung auf jedem Untergrund lesbar. */
                gfx_text(c, tx + 1, ly + 1, line[l], RGB(0x08, 0x18, 0x1C));
                gfx_text(c, tx, ly, line[l], COL_WHITE);
            }
        }
    }
}

/* --- Taskleiste ----------------------------------------------------- */

static struct rect start_button_rect(void)
{
    struct canvas *c = gfx_screen();

    return rect_make(3, c->h - TASKBAR_HEIGHT + 3, START_WIDTH, TASKBAR_HEIGHT - 6);
}

static struct rect clock_rect(void)
{
    struct canvas *c = gfx_screen();

    return rect_make(c->w - 78, c->h - TASKBAR_HEIGHT + 3, 75, TASKBAR_HEIGHT - 6);
}

/* Wer angemeldet ist, steht neben der Uhr. Auf einem Rechner, an dem
 * mehrere arbeiten, ist das die wichtigste Auskunft der Leiste. */
static struct rect user_rect(void)
{
    struct canvas *c = gfx_screen();
    struct user   *u = session_user();
    int32_t        w = 26 + (u ? gfx_text_width(u->name) : 0);

    return rect_make(c->w - 84 - w, c->h - TASKBAR_HEIGHT + 3, w,
                     TASKBAR_HEIGHT - 6);
}

static size_t taskbar_button_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < gui_window_count(); i++) {
        struct window *w = gui_window_at(i);

        if (w->visible && !(w->flags & WF_NO_TASKBAR))
            n++;
    }
    return n;
}

static struct window *taskbar_window_at_index(size_t index)
{
    size_t n = 0;

    for (size_t i = 0; i < gui_window_count(); i++) {
        struct window *w = gui_window_at(i);

        if (!w->visible || (w->flags & WF_NO_TASKBAR))
            continue;
        if (n++ == index)
            return w;
    }
    return NULL;
}

static struct rect taskbar_button_rect(size_t index)
{
    struct canvas *c = gfx_screen();
    int32_t first = START_WIDTH + 12;
    int32_t room  = clock_rect().x - first - 8;
    size_t  count = taskbar_button_count();

    if (count == 0)
        return rect_make(0, 0, 0, 0);

    int32_t width = room / (int32_t)count;
    width = CLAMP(width, 40, 180);

    return rect_make(first + (int32_t)index * (width + 2),
                     c->h - TASKBAR_HEIGHT + 3, width, TASKBAR_HEIGHT - 6);
}

static void paint_start_button(struct canvas *c)
{
    struct rect r = start_button_rect();

    gfx_fill(c, r, COL_FACE);
    gfx_bevel(c, r, !start_pressed);

    int32_t off = start_pressed ? 1 : 0;

    /* Kleines Logo: vier bunte Quadrate. */
    int32_t lx = r.x + 8 + off, ly = r.y + 5 + off;
    gfx_fill(c, rect_make(lx,     ly,     6, 6), RGB(0xE0, 0x50, 0x40));
    gfx_fill(c, rect_make(lx + 7, ly,     6, 6), RGB(0x50, 0xB0, 0x60));
    gfx_fill(c, rect_make(lx,     ly + 7, 6, 6), RGB(0x40, 0x80, 0xD0));
    gfx_fill(c, rect_make(lx + 7, ly + 7, 6, 6), RGB(0xE8, 0xC0, 0x40));

    gfx_text_bold(c, r.x + 30 + off, r.y + 4 + off, "Start", COL_TEXT);
}

static void paint_taskbar(struct canvas *c)
{
    struct canvas *screen = gfx_screen();
    struct rect bar = rect_make(0, screen->h - TASKBAR_HEIGHT,
                                screen->w, TASKBAR_HEIGHT);

    gfx_fill(c, bar, COL_TASKBAR);
    gfx_hline(c, 0, bar.y, bar.w, COL_HILIGHT);

    paint_start_button(c);

    struct window *focus = gui_focused();
    size_t count = taskbar_button_count();

    for (size_t i = 0; i < count; i++) {
        struct window *w = taskbar_window_at_index(i);
        struct rect r = taskbar_button_rect(i);

        if (r.w < 24)
            break;

        bool active = (w == focus) && !w->minimized;

        gfx_fill(c, r, active ? COL_FACE_LIGHT : COL_FACE);
        gfx_bevel(c, r, !active);

        int32_t off = active ? 1 : 0;
        icon_draw(c, r.x + 4 + off, r.y + 3 + off, w->icon, 1);
        gfx_set_clip(c, rect_intersect(c->clip, r));
        gfx_text_clipped(c, r.x + 24 + off, r.y + 4 + off, w->title,
                         COL_TEXT, r.w - 30);
        gfx_reset_clip(c);
    }

    struct user *me = session_user();

    if (me) {
        struct rect ur = user_rect();

        gfx_bevel_thin(c, ur, false);
        icon_draw(c, ur.x + 4, ur.y + 2, user_is_admin(me) ? ICON_SHIELD : ICON_USER, 1);
        gfx_text(c, ur.x + 24, ur.y + 4, me->name, COL_TEXT);
    }

    struct rect ck = clock_rect();
    gfx_bevel_thin(c, ck, false);
    int32_t tw = gfx_text_width(clock_text);
    gfx_text(c, ck.x + (ck.w - tw) / 2, ck.y + 4, clock_text, COL_TEXT);
}

/* --- Startmenue ------------------------------------------------------ */

static void start_menu_selected(int id, void *user)
{
    UNUSED(user);
    start_pressed = false;

    if (id == MENU_ID_REBOOT)
        power_reboot();
    if (id == MENU_ID_SHUTDOWN)
        power_shutdown();
    if (id == MENU_ID_LOCK) {
        lock_show(LOCK_LOCKED);
        return;
    }
    if (id == MENU_ID_LOGOUT) {
        /* Abmelden heisst: alles zumachen, was diesem Benutzer gehoert.
         * Eine Sitzung, die nach dem Abmelden weiterlaeuft, waere keine. */
        while (gui_window_count())
            gui_close_window(gui_window_at(0));
        lock_show(LOCK_LOGOUT);
        return;
    }
    if (id == MENU_ID_SWITCH) {
        while (gui_window_count())
            gui_close_window(gui_window_at(0));
        lock_show(LOCK_SWITCH);
        return;
    }

    size_t index = (size_t)(id - MENU_ID_APPS);
    if (index < app_count && app_list[index].launch)
        app_list[index].launch();

    gui_invalidate();
}

static void open_start_menu(void)
{
    struct menu_item items[MENU_MAX_ITEMS];
    size_t n = 0;

    for (size_t i = 0; i < app_count && n < MENU_MAX_ITEMS - 7; i++) {
        items[n].label    = app_list[i].name;
        items[n].icon     = app_list[i].icon;
        items[n].has_icon = true;
        items[n].enabled  = true;
        items[n].id       = MENU_ID_APPS + (int)i;
        n++;
    }

    /* Die Sitzung steht zwischen den Programmen und dem Ausschalten -
     * dort sucht man sie, und dort tut ein Fehlgriff am wenigsten weh. */
    items[n++] = (struct menu_item){ .label = NULL };
    items[n++] = (struct menu_item){ .label = "Sperren", .icon = ICON_LOCK,
                                     .has_icon = true,
                                     .enabled = session_user() != NULL,
                                     .id = MENU_ID_LOCK };
    items[n++] = (struct menu_item){ .label = "Benutzer wechseln", .icon = ICON_USERS,
                                     .has_icon = true,
                                     .enabled = user_store_exists(),
                                     .id = MENU_ID_SWITCH };
    items[n++] = (struct menu_item){ .label = "Abmelden", .icon = ICON_LOGOUT,
                                     .has_icon = true,
                                     .enabled = user_store_exists(),
                                     .id = MENU_ID_LOGOUT };

    items[n++] = (struct menu_item){ .label = NULL };
    items[n++] = (struct menu_item){ .label = "Neu starten", .icon = ICON_SETTINGS,
                                     .has_icon = true, .enabled = true,
                                     .id = MENU_ID_REBOOT };
    items[n++] = (struct menu_item){ .label = "Herunterfahren", .icon = ICON_COMPUTER,
                                     .has_icon = true, .enabled = true,
                                     .id = MENU_ID_SHUTDOWN };

    struct rect btn = start_button_rect();
    int32_t height = 0;
    for (size_t i = 0; i < n; i++)
        height += items[i].label ? 20 : 7;

    start_pressed = true;
    gui_open_menu(btn.x, btn.y - height - 8, items, n, start_menu_selected, NULL);
}

/* --- Schnittstelle nach aussen --------------------------------------- */

void desktop_init(void)
{
    rtc_format_time(clock_text, sizeof(clock_text));
    selected_icon = -1;
}

void desktop_paint_background(struct canvas *c)
{
    paint_wallpaper(c);
    paint_desktop_icons(c);
}

void desktop_paint_taskbar(struct canvas *c)
{
    paint_taskbar(c);
}

void desktop_tick(void)
{
    char now[16];

    rtc_format_time(now, sizeof(now));
    if (strcmp(now, clock_text) != 0) {
        strlcpy(clock_text, now, sizeof(clock_text));
        gui_invalidate();
    }

    if (start_pressed && !gui_menu_open()) {
        start_pressed = false;
        gui_invalidate();
    }
}

bool desktop_mouse(int32_t x, int32_t y, uint8_t button, bool down, bool dbl)
{
    struct canvas *screen = gfx_screen();

    if (!down)
        return false;

    /* Taskleiste */
    if (y >= screen->h - TASKBAR_HEIGHT) {
        if (rect_contains(start_button_rect(), x, y)) {
            if (gui_menu_open()) {
                gui_close_menu();
                start_pressed = false;
            } else {
                open_start_menu();
            }
            gui_invalidate();
            return true;
        }

        size_t count = taskbar_button_count();
        for (size_t i = 0; i < count; i++) {
            if (!rect_contains(taskbar_button_rect(i), x, y))
                continue;

            struct window *w = taskbar_window_at_index(i);
            if (w == gui_focused() && !w->minimized)
                w->minimized = true;
            else
                gui_focus_window(w);

            gui_invalidate();
            return true;
        }
        return true;
    }

    /* Symbole auf dem Desktop */
    size_t count = desktop_icon_count();
    for (size_t i = 0; i < count; i++) {
        if (!rect_contains(desktop_icon_rect(i), x, y))
            continue;

        selected_icon = (int)i;
        gui_invalidate();

        if (dbl && button == MB_LEFT) {
            const struct app_entry *app = desktop_icon_at_index(i);
            if (app && app->launch)
                app->launch();
        }
        return true;
    }

    if (selected_icon != -1) {
        selected_icon = -1;
        gui_invalidate();
    }
    return false;
}
