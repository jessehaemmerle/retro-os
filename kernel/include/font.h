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

/* Fuer die Kantenglaettung liegt jedes Zeichen ein zweites Mal da:
 * dreifach abgetastet in der Breite, ein Bit je Subpixel. Eine Zeile
 * sind 24 Bit in drei Bytes, das hoechstwertige Bit links. */
#define FONT_SUB_BYTES (FONT_HEIGHT * 3)

/* Eine Schriftart: Name fuer die Oberflaeche, Lizenz fuer den Nachweis,
 * dazu die Punkte. */
struct font_face {
    const char *name;
    const char *license;
    const uint8_t (*glyphs)[FONT_HEIGHT];
    const uint8_t (*subpixels)[FONT_SUB_BYTES];
};

extern const struct font_face font_faces[FONT_FACES];

/* Zeigt auf die Punkte der gewaehlten Schrift. */
extern const uint8_t (*font_active)[FONT_HEIGHT];
extern const uint8_t (*font_active_sub)[FONT_SUB_BYTES];

static inline const uint8_t *font_glyph(unsigned char c)
{
    if (c < FONT_FIRST)
        return font_active[0];
    return font_active[c - FONT_FIRST];
}

/* Die dreifach abgetastete Fassung desselben Zeichens: 48 Bytes,
 * je Zeile drei. */
static inline const uint8_t *font_glyph_sub(unsigned char c)
{
    if (c < FONT_FIRST)
        return font_active_sub[0];
    return font_active_sub[c - FONT_FIRST];
}

/* Wie weich die Kanten der Schrift sein sollen.
 *
 * FONT_SHARP ist der alte Zustand: ein Bit je Punkt, entweder ganz
 * oder gar nicht. FONT_GRAY mittelt die drei Abtastungen eines
 * Punktes zu einem Grauwert. FONT_RGB und FONT_BGR steuern die drei
 * Leuchtpunkte eines LCD einzeln an - das Verfahren, das unter dem
 * Namen ClearType bekannt ist. Welches von beiden gilt, haengt an der
 * Bauart des Bildschirms; auf einem Roehrenmonitor oder in einem
 * vergroesserten Bild taugt keines davon. */
enum font_smoothing {
    FONT_SHARP,
    FONT_GRAY,
    FONT_RGB,
    FONT_BGR,
};

enum font_smoothing font_smoothing(void);
void                font_set_smoothing(enum font_smoothing mode);

/* Name fuer die Oberflaeche, Schluessel fuer die Einstellungsdatei. */
const char *font_smoothing_name(enum font_smoothing mode);
const char *font_smoothing_key(enum font_smoothing mode);
bool        font_smoothing_parse(const char *text, enum font_smoothing *out);

/* Wie stark ein Subpixel gedeckt ist: der Fuenf-Punkte-Filter
 * (1-2-3-2-1) ueber die Abtastungen einer Zeile. row traegt 24 Bit,
 * das hoechstwertige links; sub zaehlt die Abtastungen von links.
 * Liefert 0 bis 255. */
uint8_t font_coverage(uint32_t row, int32_t sub);

/* Die drei Deckungen eines Punktes, schon in der Reihenfolge des
 * Bildschirms. Bei FONT_GRAY sind alle drei gleich. */
void font_pixel_coverage(uint32_t row, int32_t x, enum font_smoothing mode,
                         uint8_t *r, uint8_t *g, uint8_t *b);

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
