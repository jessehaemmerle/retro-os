/* gui.h - Fenstersystem und Ereignisverteilung. */
#ifndef GUI_H
#define GUI_H

#include "gfx.h"
#include "input.h"
#include "icons.h"

#define GUI_MAX_WINDOWS 24
#define WIN_TITLE_MAX   63

struct window;

enum gui_event_type {
    EV_PAINT,
    EV_KEY_DOWN,
    EV_KEY_UP,
    EV_MOUSE_DOWN,
    EV_MOUSE_UP,
    EV_MOUSE_MOVE,
    EV_MOUSE_DRAG,
    EV_DOUBLE_CLICK,
    EV_SCROLL,
    EV_TICK,
    EV_RESIZED,
    EV_FOCUS,
    EV_BLUR,
};

enum mouse_button {
    MB_LEFT = 1,
    MB_RIGHT = 2,
    MB_MIDDLE = 3,
};

struct gui_event {
    enum gui_event_type type;

    int32_t  x, y;        /* Position im Fensterinhalt (Client-Koordinaten) */
    uint8_t  button;
    int8_t   scroll;

    uint16_t key;
    char     ascii;
    uint8_t  mods;
};

typedef void (*win_paint_fn)(struct window *win, struct canvas *c);
typedef void (*win_event_fn)(struct window *win, const struct gui_event *ev);
typedef void (*win_close_fn)(struct window *win);

enum window_flags {
    WF_RESIZABLE = 1 << 0,
    WF_NO_CLOSE  = 1 << 1,
    WF_NO_MIN    = 1 << 2,
    WF_NO_TASKBAR= 1 << 3,
    WF_CENTER    = 1 << 4,
    /* Ohne Rahmen, Titelleiste und Taskleiste - der Inhalt fuellt das
     * ganze Fenster. Gedacht fuer den Vollbildmodus eines Programms,
     * das den Bildschirm fuer sich braucht. */
    WF_BARE      = 1 << 5,
};

struct window {
    char          title[WIN_TITLE_MAX + 1];
    struct rect   frame;        /* Aussenmasse auf dem Bildschirm */
    uint32_t      flags;
    enum icon_id  icon;

    bool          used;
    bool          visible;
    bool          minimized;

    int32_t       min_w, min_h;

    win_paint_fn  on_paint;
    win_event_fn  on_event;
    win_close_fn  on_close;

    void         *user;         /* Zustand der jeweiligen Anwendung */
};

/* --- Fensterverwaltung --- */
void gui_init(void);
struct window *gui_create_window(const char *title, int32_t x, int32_t y,
                                 int32_t w, int32_t h, uint32_t flags,
                                 enum icon_id icon);
void gui_close_window(struct window *win);
void gui_focus_window(struct window *win);
void gui_set_title(struct window *win, const char *title);
void gui_invalidate(void);                 /* Bildschirm neu zeichnen lassen */

struct window *gui_focused(void);
/* Prueft, ob ein Fensterzeiger noch auf ein offenes Fenster zeigt. Dialoge
 * melden ihr Ergebnis verzoegert; bis dahin kann der Aufrufer weg sein. */
bool gui_window_alive(const struct window *win);
struct rect    gui_client_rect(const struct window *win);
/* Leinwand mit Ursprung in der linken oberen Ecke des Fensterinhalts.
 * Anwendungen rechnen damit in eigenen Koordinaten und muessen die
 * Fensterposition nicht kennen. */
struct canvas  gui_client_canvas(const struct window *win, const struct canvas *c);
int32_t        gui_client_width(const struct window *win);
int32_t        gui_client_height(const struct window *win);

/* Zaehlt und liefert die Fenster in Stapelreihenfolge (unten nach oben). */
size_t gui_window_count(void);
struct window *gui_window_at(size_t index);

/* Findet ein bereits offenes Fenster derselben Anwendung. */
struct window *gui_find_by_paint(win_paint_fn fn);

/* --- Popup-Menue --- */
#define MENU_MAX_ITEMS 24

struct menu_item {
    const char  *label;      /* NULL = Trennlinie */
    enum icon_id icon;
    bool         has_icon;
    bool         enabled;
    int          id;
};

typedef void (*menu_select_fn)(int id, void *user);

void gui_open_menu(int32_t x, int32_t y, const struct menu_item *items,
                   size_t count, menu_select_fn on_select, void *user);
void gui_close_menu(void);
bool gui_menu_open(void);

/* --- Hauptschleife --- */
NORETURN void gui_run(void);

/* Vom Desktop bereitgestellt (desktop.c). */
void desktop_init(void);
/* Der Hintergrund liegt unter den Fenstern, die Taskleiste darueber. */
void desktop_paint_background(struct canvas *c);
void desktop_paint_taskbar(struct canvas *c);
bool desktop_mouse(int32_t x, int32_t y, uint8_t button, bool down, bool dbl);
void desktop_tick(void);
int32_t desktop_work_height(void);   /* Bildschirmhoehe ohne Taskleiste */

#endif /* GUI_H */
