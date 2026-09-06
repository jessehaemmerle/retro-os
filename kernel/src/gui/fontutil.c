/* fontutil.c - der Filter hinter der Kantenglaettung.
 *
 * Die Vorlage ist dreifach abgetastet: 24 Bit je Zeile statt acht.
 * Daraus wird die Deckung eines Punktes - und bei Subpixel-Glaettung
 * die seiner drei Leuchtpunkte einzeln.
 *
 * Gefiltert wird mit den Gewichten 1-2-3-2-1 ueber fuenf Abtastungen.
 * Ohne diesen Filter faerbte jede senkrechte Kante den Buchstaben
 * bunt: Ein einzelner gesetzter Subpixel waere ein roter oder blauer
 * Strich. Der Filter verteilt ihn auf die Nachbarn, und uebrig
 * bleibt ein Farbsaum, den das Auge nicht mehr als Farbe liest.
 *
 * Getrennt von font.c, weil hier gerechnet wird und dort nur
 * ausgewaehlt - und weil sich das Rechnen auf dem Entwicklungsrechner
 * pruefen laesst.
 */

#include "font.h"
#include "kstring.h"

/* Ein Bit der Zeile, links beginnend. Ausserhalb ist nichts. */
static uint32_t sample(uint32_t row, int32_t sub)
{
    if (sub < 0 || sub >= FONT_WIDTH * 3)
        return 0;
    return (row >> (FONT_WIDTH * 3 - 1 - sub)) & 1u;
}

/* Von der Summe der Gewichte auf die Deckung.
 *
 * Nicht geradlinig, und das ist der Punkt: Ein senkrechter Strich von
 * einem Punkt Breite bringt es nie ueber sieben von neun Gewichten.
 * Geradlinig gerechnet waere er damit nur zu 198 von 255 gedeckt -
 * geglaettete Schrift saehe blasser aus als harte, und niemand haelte
 * das fuer eine Verbesserung. Die Kurve zieht die mittleren Werte an
 * und laesst die Kanten weich. */
static const uint8_t weight_to_alpha[10] = {
    0, 40, 85, 125, 160, 190, 214, 234, 247, 255,
};

uint8_t font_coverage(uint32_t row, int32_t sub)
{
    uint32_t sum = sample(row, sub - 2)
                 + sample(row, sub - 1) * 2
                 + sample(row, sub)     * 3
                 + sample(row, sub + 1) * 2
                 + sample(row, sub + 2);

    return weight_to_alpha[sum <= 9 ? sum : 9];
}

void font_pixel_coverage(uint32_t row, int32_t x, enum font_smoothing mode,
                         uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t a = font_coverage(row, x * 3);
    uint8_t c = font_coverage(row, x * 3 + 1);
    uint8_t d = font_coverage(row, x * 3 + 2);

    switch (mode) {
    case FONT_RGB:
        *r = a; *g = c; *b = d;
        break;
    case FONT_BGR:
        /* Derselbe Punkt, nur liegen die Leuchtpunkte andersherum. */
        *r = d; *g = c; *b = a;
        break;
    default: {
        /* Grau: der Mittelwert der drei. Das ist gewoehnliche
         * Kantenglaettung - sie taugt auf jedem Bildschirm. */
        uint8_t mid = (uint8_t)(((uint32_t)a + c + d) / 3);

        *r = *g = *b = mid;
        break;
    }
    }
}

const char *font_smoothing_name(enum font_smoothing mode)
{
    switch (mode) {
    case FONT_GRAY: return "Graustufen";
    case FONT_RGB:  return "Subpixel (RGB)";
    case FONT_BGR:  return "Subpixel (BGR)";
    default:        return "Aus";
    }
}

const char *font_smoothing_key(enum font_smoothing mode)
{
    switch (mode) {
    case FONT_GRAY: return "grau";
    case FONT_RGB:  return "rgb";
    case FONT_BGR:  return "bgr";
    default:        return "aus";
    }
}

bool font_smoothing_parse(const char *text, enum font_smoothing *out)
{
    if (!text || !out)
        return false;

    if (strcmp(text, "aus") == 0)  { *out = FONT_SHARP; return true; }
    if (strcmp(text, "grau") == 0) { *out = FONT_GRAY;  return true; }
    if (strcmp(text, "rgb") == 0)  { *out = FONT_RGB;   return true; }
    if (strcmp(text, "bgr") == 0)  { *out = FONT_BGR;   return true; }
    return false;
}
