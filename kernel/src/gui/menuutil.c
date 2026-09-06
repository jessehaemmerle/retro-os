/* menuutil.c - das Blaettern durch ein Menue mit der Tastatur.
 *
 * Getrennt vom Fenstersystem, weil hier die Faelle liegen, die man
 * leicht uebersieht: Ein Menue, das oben und unten Trennlinien hat.
 * Eines, in dem die Haelfte blass ist. Eines, in dem gar nichts
 * waehlbar ist - dort darf die Suche nach der naechsten Zeile nicht
 * ewig im Kreis laufen.
 */

#include "gui.h"

int menu_next_index(const struct menu_item *items, size_t count, int current,
                    int delta)
{
    if (!items || count == 0 || delta == 0)
        return current;

    int at = current;

    /* Hoechstens einmal herum: Ist nichts waehlbar, bleibt es, wie es
     * war. */
    for (size_t tries = 0; tries < count; tries++) {
        at += delta > 0 ? 1 : -1;

        if (at < 0)
            at = (int)count - 1;
        else if (at >= (int)count)
            at = 0;

        /* Eine Trennlinie hat keine Beschriftung, eine blasse Zeile
         * keine Wirkung - beide waeren ein Tastendruck ohne Ergebnis. */
        if (items[at].label && items[at].enabled)
            return at;
    }
    return current;
}
