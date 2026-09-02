/* font.h - eingebaute 8x16-Bitmapschriften (Latin-1).
 *
 * Alle Zeichen sitzen in einer festen Zelle von 8x16 Pixeln. Das ist
 * keine Bequemlichkeit, sondern Absicht: Fenster, Tabellen, Editor und
 * Konsole rechnen ihre Spalten und Zeilen aus FONT_WIDTH und
 * FONT_HEIGHT aus. Eine Schriftart zu wechseln heisst darum nur,
 * andere Punkte in dieselbe Zelle zu setzen - das Layout bleibt, wo es
 * ist, und kein Programm muss davon wissen.
 *
 * Die Auswahl steht in einem Zeiger, den font_glyph() liest. Ein
 * Wechsel kostet also eine Zuweisung und wirkt sofort ueberall.
 */
#ifndef FONT_H
#define FONT_H

#include "retro.h"

#define FONT_WIDTH   8
#define FONT_HEIGHT  16
#define FONT_FIRST   0x20
#define FONT_LAST    0xFF
#define FONT_GLYPHS  (FONT_LAST - FONT_FIRST + 1)
#define FONT_FACES   10

/* Eine Schriftart: Name fuer die Oberflaeche, Lizenz fuer den Nachweis,
 * dazu die Punkte. */
struct font_face {
    const char *name;
    const char *license;
    const uint8_t (*glyphs)[FONT_HEIGHT];
};

extern const struct font_face font_faces[FONT_FACES];

/* Zeigt auf die Punkte der gewaehlten Schrift. */
extern const uint8_t (*font_active)[FONT_HEIGHT];

static inline const uint8_t *font_glyph(unsigned char c)
{
    if (c < FONT_FIRST)
        return font_active[0];
    return font_active[c - FONT_FIRST];
}

size_t      font_count(void);
size_t      font_current(void);
const char *font_name(size_t index);
const char *font_license(size_t index);

/* Waehlt nach Nummer. Ausserhalb des Bereichs bleibt es beim Alten. */
void font_select(size_t index);

/* Waehlt nach Name, Gross- und Kleinschreibung egal. Gibt false zurueck,
 * wenn es die Schrift nicht gibt - dann bleibt die alte stehen. */
bool font_select_by_name(const char *name);

#endif /* FONT_H */
