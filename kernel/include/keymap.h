/* keymap.h - Tastaturbelegungen.
 *
 * Eine Taste meldet nur, wo sie sitzt - welches Zeichen dabei
 * herauskommt, entscheidet die Belegung. Bis hierher war die deutsche
 * fest eingebaut, und zwar zweimal: einmal fuer den PS/2-Anschluss
 * (Scancode-Satz 1) und einmal fuer USB (HID-Verwendungsnummern).
 *
 * Beide Tabellen stehen jetzt beisammen, und der Benutzer kann
 * umschalten.
 *
 * Tottasten (^ und ¨) geben ihr Zeichen unmittelbar aus, statt auf das
 * naechste zu warten - das waere eine eigene Zustandsmaschine wert,
 * lohnt sich hier aber nicht.
 */
#ifndef KEYMAP_H
#define KEYMAP_H

#include "retro.h"

#define KEYMAP_PS2_CODES  0x60
#define KEYMAP_HID_FIRST  0x04
#define KEYMAP_HID_LAST   0x64
#define KEYMAP_HID_CODES  (KEYMAP_HID_LAST - KEYMAP_HID_FIRST + 1)

struct keymap {
    const char *code;        /* "de", "us", "ch" - so steht es in der Datei */
    const char *name;        /* was im Einstellungsfenster erscheint        */

    const uint16_t *ps2_base;
    const uint16_t *ps2_shift;
    const uint16_t *ps2_altgr;

    const uint16_t *hid_base;
    const uint16_t *hid_shift;
    const uint16_t *hid_altgr;
};

size_t               keymap_count(void);
const struct keymap *keymap_at(size_t index);
const struct keymap *keymap_current(void);
size_t               keymap_current_index(void);

/* Waehlt nach Kuerzel ("de"); unbekannte Kuerzel aendern nichts. */
bool keymap_select(const char *code);
void keymap_select_index(size_t index);

#endif /* KEYMAP_H */
