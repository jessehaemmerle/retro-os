/* pngschreiben_test.c - schreibt PNG und liest es wieder ein.
 *
 * Ein Schreiber laesst sich nicht an sich pruefen, sondern nur daran,
 * ob jemand das Ergebnis lesen kann. Der Leser steht im selben Haus -
 * derselbe, mit dem der Browser seine Bilder anzeigt -, und er ist
 * gegen libpng geprueft. Was er hier wieder herausbekommt, muss also
 * Punkt fuer Punkt dasselbe sein, was hineinging.
 *
 * Geprueft werden dabei die Stellen, an denen ein PNG-Schreiber
 * schiefliegt: die Pruefsummen, die Bloecke an ihrer Grenze von 65535
 * Bytes und Bilder, die nur eine Zeile oder nur eine Spalte haben.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"
#include "kstring.h"

static int fehler;
static int geprueft;

static void pruefe(const char *was, bool bedingung)
{
    geprueft++;
    if (!bedingung) {
        printf("  FEHLER: %s\n", was);
        fehler++;
    }
}

/* Ein Muster, das jede Verwechslung von Zeilen, Spalten und Kanaelen
 * auffliegen laesst: Rot waechst nach rechts, Gruen nach unten, Blau
 * ist die Summe. */
static uint32_t muster(int32_t x, int32_t y)
{
    uint32_t r = (uint32_t)(x * 7 + 3) & 0xFF;
    uint32_t g = (uint32_t)(y * 11 + 5) & 0xFF;
    uint32_t b = (uint32_t)(x + y) & 0xFF;

    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Schreibt ein Bild, liest es zurueck und vergleicht. stride sagt, wie
 * weit die Zeilen im Quellpuffer auseinanderliegen - beim Backbuffer
 * ist das nicht die Breite. */
static void rundlauf(const char *was, int32_t w, int32_t h, int32_t extra)
{
    int32_t stride = w + extra;
    uint32_t *src = malloc((size_t)stride * h * 4);

    if (!src) {
        printf("  FEHLER: %s - kein Speicher\n", was);
        fehler++;
        return;
    }

    /* Der Bereich hinter der Breite wird mit Unsinn gefuellt: Wer den
     * Zeilenabstand ignoriert, nimmt ihn mit und faellt hier auf. */
    for (int32_t y = 0; y < h; y++)
        for (int32_t x = 0; x < stride; x++)
            src[y * stride + x] = x < w ? muster(x, y) : 0xFFFF00FFu;

    uint8_t *png = NULL;
    size_t   size = 0;

    geprueft++;
    if (!png_encode(src, w, h, stride, &png, &size)) {
        printf("  FEHLER: %s - schreiben ging nicht\n", was);
        fehler++;
        free(src);
        return;
    }

    struct image back;

    geprueft++;
    if (!image_decode_png(png, size, &back)) {
        printf("  FEHLER: %s - lesen ging nicht (%zu Bytes)\n", was, size);
        fehler++;
        free(png);
        free(src);
        return;
    }

    geprueft++;
    if (back.w != w || back.h != h) {
        printf("  FEHLER: %s - %dx%d statt %dx%d\n", was,
               (int)back.w, (int)back.h, (int)w, (int)h);
        fehler++;
    } else {
        long abweichung = 0;

        for (int32_t y = 0; y < h; y++) {
            for (int32_t x = 0; x < w; x++) {
                uint32_t soll = muster(x, y) & 0x00FFFFFFu;
                uint32_t ist = back.px[y * w + x] & 0x00FFFFFFu;

                if (soll != ist)
                    abweichung++;
            }
        }
        geprueft++;
        if (abweichung) {
            printf("  FEHLER: %s - %ld Punkte weichen ab\n", was, abweichung);
            fehler++;
        }
    }

    image_free(&back);
    free(png);
    free(src);
}

/* Der eigene Leser koennte denselben Fehler machen wie der Schreiber.
 * Darum wird der Kopf der Datei auch von Hand nachgesehen. */
static void test_aufbau(void)
{
    printf("Aufbau der Datei\n");

    uint32_t pixel[4] = { 0xFF112233u, 0xFF445566u, 0xFF778899u, 0xFFAABBCCu };
    uint8_t *png = NULL;
    size_t size = 0;

    pruefe("Schreiben geht", png_encode(pixel, 2, 2, 2, &png, &size));
    if (!png)
        return;

    static const uint8_t signature[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };

    pruefe("Die Kennung stimmt", size > 8 && memcmp(png, signature, 8) == 0);
    pruefe("IHDR steht vorn", size > 16 && memcmp(png + 12, "IHDR", 4) == 0);
    pruefe("Breite steht drin", size > 24 && png[16] == 0 && png[17] == 0 &&
                                png[18] == 0 && png[19] == 2);
    pruefe("Acht Bit je Kanal", size > 25 && png[24] == 8);
    pruefe("Farbtyp 2 - RGB", size > 26 && png[25] == 2);
    pruefe("IEND schliesst ab",
           size > 12 && memcmp(png + size - 8, "IEND", 4) == 0);

    /* Die Pruefsumme des ersten Abschnitts wird nachgerechnet. Der
     * eigene Leser prueft sie nicht - ein fremdes Programm sehr wohl,
     * und dann waere die Datei dort kaputt, ohne dass es hier je
     * aufgefallen waere. */
    uint32_t stored = ((uint32_t)png[29] << 24) | ((uint32_t)png[30] << 16) |
                      ((uint32_t)png[31] << 8) | png[32];
    uint32_t computed = crc32(png + 12, 4 + 13);

    pruefe("Die Pruefsumme von IHDR stimmt", stored == computed);

    free(png);
}

int main(void)
{
    printf("=== PNG schreiben ===\n");

    test_aufbau();

    printf("Hin und zurueck\n");
    rundlauf("Ein Punkt", 1, 1, 0);
    rundlauf("Eine Zeile", 64, 1, 0);
    rundlauf("Eine Spalte", 1, 64, 0);
    rundlauf("Klein", 3, 3, 0);
    rundlauf("Mit Zeilenabstand", 40, 20, 24);
    rundlauf("Mittel", 200, 120, 0);

    /* Ueber 65535 Bytes Rohdaten - dort faengt der zweite Block an.
     * 300 Punkte je Zeile sind 901 Bytes; nach 73 Zeilen ist die
     * Grenze ueberschritten. */
    rundlauf("Ueber die Blockgrenze", 300, 100, 0);
    rundlauf("Weit ueber die Blockgrenze", 320, 240, 7);

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
