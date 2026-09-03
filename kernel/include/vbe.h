/* vbe.h - die Aufloesung zur Laufzeit umstellen.
 *
 * Der Bootloader setzt einen Modus und geht; danach ist Schluss - kein
 * BIOS-Aufruf, kein GOP mehr. Wer die Aufloesung spaeter noch aendern
 * will, muss die Grafikkarte selbst ansprechen.
 *
 * Fuer genau diesen Fall haben Bochs, QEMU und VirtualBox eine
 * gemeinsame kleine Schnittstelle: ein Indexregister auf 0x01CE, ein
 * Datenregister auf 0x01CF, ein Dutzend Register dahinter. Damit
 * lassen sich Breite, Hoehe und Farbtiefe setzen, und der lineare
 * Speicher bleibt, wo er ist. Auf echter Hardware gibt es das nicht -
 * dort bleibt es bei dem, was der Bootloader eingestellt hat, und die
 * Oberflaeche sagt das auch.
 */
#ifndef VBE_H
#define VBE_H

#include "retro.h"

/* Sucht die Grafikkarte und stellt fest, ob sie mitspielt. */
bool vbe_init(void);
bool vbe_available(void);

/* Wie viel Grafikspeicher zur Verfuegung steht - daran haengt, welche
 * Modi ueberhaupt in Frage kommen. 0 heisst unbekannt. */
uint64_t vbe_vram_bytes(void);

/* Adresse des linearen Speichers, wie ihn die Karte meldet. */
uint64_t vbe_framebuffer(void);

/* Setzt einen Modus. pitch kommt in Bytes zurueck. */
bool vbe_set_mode(uint32_t width, uint32_t height, uint64_t *pitch_bytes);

#endif /* VBE_H */
