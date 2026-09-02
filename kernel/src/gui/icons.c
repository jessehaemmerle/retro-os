/* icons.c - Symbole auf die Flaeche bringen.
 *
 * Die Bilder selbst stehen in icon_data.c und bringen einen
 * Deckungswert je Punkt mit. Beim Zeichnen wird deshalb nicht einfach
 * gesetzt, sondern gemischt: Nur so bleiben die weichen Kanten weich,
 * gleich worauf das Symbol liegt.
 */

#include "icons.h"

void icon_draw(struct canvas *c, int32_t x, int32_t y, enum icon_id id,
               int32_t scale)
{
    if (id >= ICON_COUNT || scale < 1)
        return;

    /* Bis zum Doppelten gibt es ein eigens gezeichnetes Bild. Darueber
     * hinaus wird das groessere aufgezogen - das kommt selten vor. */
    const uint32_t *bits = scale == 1 ? icon_bits16[id] : icon_bits32[id];
    int32_t source = scale == 1 ? ICON_SIZE : ICON_SIZE_LARGE;
    int32_t target = ICON_SIZE * scale;

    if (!bits)
        return;

    for (int32_t row = 0; row < target; row++) {
        int32_t sy = row * source / target;

        for (int32_t col = 0; col < target; col++) {
            uint32_t pixel = bits[sy * source + col * source / target];

            gfx_blend(c, x + col, y + row, pixel);
        }
    }
}
