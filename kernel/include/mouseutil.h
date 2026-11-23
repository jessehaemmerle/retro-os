/* mouseutil.h - das Rechnen hinter dem Mauszeiger.
 *
 * Alles, was aus Bytes Bewegung und aus Bewegung Ereignisse macht,
 * ohne den Anschluss selbst anzufassen - und darum auf dem
 * Entwicklungsrechner pruefbar ist. Wo der Zeiger hakt oder ein Klick
 * verlorengeht, liegt es fast nie am Anschluss, sondern an einer
 * Zeile hier: einem Vorzeichen, einem Bit, das als "ungueltig" galt,
 * oder einer Reihenfolge.
 */
#ifndef MOUSEUTIL_H
#define MOUSEUTIL_H

#include "retro.h"

#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

struct ps2_motion {
    int32_t dx, dy;        /* nach rechts und nach unten positiv */
    int8_t  scroll;        /* nach OBEN positiv, wie bei HID     */
    uint8_t buttons;       /* MOUSE_BTN_...                      */
};

/* Die Richtung des Rades.
 *
 * PS/2 und USB zaehlen entgegengesetzt: Bei PS/2 ist ein Schritt
 * "positiv", wenn das Rad zum Benutzer hin gedreht wird, bei HID, wenn es
 * vom Benutzer weg geht. Die Oberflaeche rechnet mit der zweiten
 * Zaehlweise (positiv = nach oben scrollen); also dreht diese Funktion
 * das PS/2-Vorzeichen um. Vorher reichten beide Wege den Wert
 * unveraendert weiter - und mit einer PS/2-Maus, wie VirtualBox sie
 * ohne Tablett anbietet, scrollte jedes Rad verkehrt herum.
 *
 * Entschluesselt ein PS/2-Paket zu drei oder vier Byte. false heisst:
 * Das Paket ist keines - Bit 3 im ersten Byte fehlt, wir sind nicht am
 * Anfang eines Pakets. Alles andere wird uebernommen, auch Pakete mit
 * gesetztem Ueberlaufbit: Sie tragen die groesstmoegliche Bewegung, und
 * vor allem den Zustand der Tasten. Ein Paket wegzuwerfen, nur weil die
 * Hand schnell war, verschluckt dabei auch das Loslassen einer Taste -
 * und die haengt dann, bis jemand wieder klickt. */
bool ps2_decode_packet(const uint8_t *packet, size_t size,
                       struct ps2_motion *out);

/* Ein Schritt in der Folge der Tastenereignisse. */
struct mouse_step {
    uint8_t button;        /* genau ein MOUSE_BTN_... */
    bool    down;          /* true gedrueckt, false losgelassen */
};

/* Am laengsten kann eine Folge sein, wenn jede der drei Tasten
 * losgelassen und wieder gedrueckt und wieder losgelassen wurde. */
#define MOUSE_MAX_STEPS 9

/* Macht aus Zustand und Flanken die Folge, in der die Tasten sich
 * bewegt haben.
 *
 * prev ist der Zustand beim letzten Durchlauf, now der jetzige.
 * pressed und released sind die Flanken, die der Treiber seither
 * mitgezaehlt hat. Warum das nicht nur der Zustand ist: Zwischen zwei
 * Durchlaeufen der Hauptschleife liegt ein ganzes Bild - in einer
 * virtuellen Maschine leicht dreissig Millisekunden. Ein schneller
 * Klick ist kuerzer. Nach dem Zustand allein sieht es aus, als waere
 * nie etwas gewesen.
 *
 * Liefert, wie viele Schritte in out stehen. */
size_t mouse_button_steps(uint8_t prev, uint8_t now, uint8_t pressed,
                          uint8_t released, struct mouse_step *out);

#endif /* MOUSEUTIL_H */
