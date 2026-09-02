/* lock.h - der Anmeldebildschirm.
 *
 * Er ist kein Fenster. Ein Fenster koennte man verschieben, hinter ein
 * anderes legen oder mit der Tabulatortaste umgehen - und genau das
 * darf hier nicht passieren. Solange er an ist, malt die Oberflaeche
 * nichts anderes mehr und leitet jede Taste und jeden Klick hierher.
 * Das ist ein Dutzend Zeilen in window.c und dafuer eine Sperre, an der
 * es nichts vorbei gibt.
 */
#ifndef LOCK_H
#define LOCK_H

#include "gfx.h"
#include "input.h"

enum lock_reason {
    LOCK_START,     /* beim Hochfahren                  */
    LOCK_LOCKED,    /* der Benutzer hat gesperrt        */
    LOCK_SWITCH,    /* jemand anders moechte an den Rechner */
    LOCK_LOGOUT,    /* abgemeldet                       */
};

/* Blendet den Anmeldebildschirm ein. Bei LOCK_LOCKED bleibt die
 * Sitzung samt aller Fenster stehen; sonst wird sie beendet. */
void lock_show(enum lock_reason reason);
bool lock_active(void);

void lock_paint(struct canvas *c);
void lock_key(const struct key_event *ke);
void lock_mouse(int32_t x, int32_t y, uint8_t button, bool down);
/* Zehnmal je Sekunde - fuer die blinkende Schreibmarke und die Uhr. */
void lock_tick(void);

#endif /* LOCK_H */
