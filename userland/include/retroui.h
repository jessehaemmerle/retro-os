/* retroui.h - Fenster und Netz fuer Benutzerprogramme.
 *
 * Bis hierher konnte ein Programm in Ring 3 nur Text ausgeben. Mit
 * diesen Aufrufen bekommt es ein eigenes Fenster auf dem Desktop und
 * eine Verbindung ins Netz - beides ueber den Kernel, der die Fenster
 * verwaltet und den Netzstapel besitzt.
 *
 * Gezeichnet wird nicht Pixel fuer Pixel ueber die Grenze, sondern mit
 * einer Liste von Befehlen: Das spart Systemaufrufe und haelt den
 * Kernel in der Lage, jeden einzelnen zu pruefen.
 */
#ifndef RETROUI_H
#define RETROUI_H

#include "retroos.h"

/* --- Zeichenbefehle --- */
enum ui_op {
    UI_CLEAR = 0,   /* Farbe                        */
    UI_FILL,        /* Rechteck fuellen             */
    UI_RECT,        /* Rechteck umranden            */
    UI_LINE,        /* Linie von (x,y) nach (w,h)   */
    UI_TEXT,        /* Text bei (x,y)               */
    UI_PIXEL,
};

#define UI_TEXT_MAX 48

struct ui_cmd {
    unsigned int op;
    int          x, y, w, h;
    unsigned int color;
    char         text[UI_TEXT_MAX];
};

/* --- Ereignisse --- */
enum ui_event_type {
    UI_EV_NONE = 0,
    UI_EV_KEY,          /* Taste gedrueckt          */
    UI_EV_MOUSE_DOWN,
    UI_EV_MOUSE_UP,
    UI_EV_MOUSE_MOVE,
    UI_EV_CLOSE,        /* Fenster wurde geschlossen */
};

struct ui_event {
    unsigned int type;
    int          x, y;
    unsigned int key;
    char         ascii;
    unsigned char mods, button;
};

/* Farbe aus Rot, Gruen, Blau. */
static inline unsigned int ui_rgb(unsigned char r, unsigned char g,
                                  unsigned char b)
{
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
}

/* --- Fenster --- */

/* Oeffnet ein Fenster. Gibt eine Nummer zurueck oder einen Fehler. */
int ui_open(const char *title, int width, int height);

/* Schickt Zeichenbefehle und zeigt das Ergebnis an. */
int sys_ui_draw(int window, const struct ui_cmd *cmds, int count);
static inline int ui_draw(int window, const struct ui_cmd *cmds, int count)
{
    return sys_ui_draw(window, cmds, count);
}

/* Holt das naechste Ereignis. timeout_ms == 0 heisst "nicht warten".
 * Gibt 1 zurueck, wenn eines da war, sonst 0. */
int sys_ui_event(int window, struct ui_event *out, int timeout_ms);
static inline int ui_event(int window, struct ui_event *out, int timeout_ms)
{
    return sys_ui_event(window, out, timeout_ms);
}

int ui_close(int window);

/* --- Netz --- */

/* Verbindet sich mit einem Rechner. Der Name darf ein Name oder eine
 * Adresse sein - der Kernel loest ihn auf. */
int net_connect(const char *host, int port);
int net_send(int socket, const void *data, unsigned int length);
/* Liest, was da ist; wartet hoechstens timeout_ms. */
int sys_net_recv(int socket, void *buffer, unsigned int length, int timeout_ms);
static inline int net_recv(int socket, void *buffer, unsigned int length,
                           int timeout_ms)
{
    return sys_net_recv(socket, buffer, length, timeout_ms);
}
int net_close(int socket);

/* --- Dienste anbieten ---
 * net_listen nimmt einen Port in Beschlag (ab 1024). net_accept holt
 * die naechste Verbindung; ein negativer Wert heisst "in der Zeit kam
 * keine" und ist kein Fehler. */
int net_listen(int port);
int sys_net_accept(int listener, int timeout_ms);
static inline int net_accept(int listener, int timeout_ms)
{
    return sys_net_accept(listener, timeout_ms);
}

/* --- Abspalten ---
 *
 * sys_fork verdoppelt das laufende Programm. Beide laufen hinter dem
 * Aufruf weiter; das Kind bekommt 0 zurueck, der Elternteil die Nummer
 * des Kindes. Der Speicher wird dabei nicht kopiert - erst wenn einer
 * von beiden hineinschreibt, entsteht seine eigene Fassung der Seite.
 *
 * sys_wait holt den Ausgang eines beendeten Kindes ab und gibt dessen
 * Steckplatz frei. pid 0 heisst "irgendeines". Der Rueckgabewert ist
 * die Nummer des abgeholten Kindes, 0 wenn in der Wartezeit keines
 * fertig wurde, und ein negativer Wert, wenn es gar keine Kinder gibt.
 * Mit timeout_ms 0 fragt man nur nach, ohne zu warten. */
int sys_fork(void);
int sys_wait(int pid, int *code, int timeout_ms);

static inline int fork_process(void)  { return sys_fork(); }

#endif /* RETROUI_H */
