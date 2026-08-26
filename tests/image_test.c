/* image_test.c - prueft die Bildleser gegen Vergleichsdaten.
 *
 * Der Erzeuger legt zu jedem Testbild eine Rohdatei mit den erwarteten
 * Pixeln ab. Hier wird beides verglichen; kleine Abweichungen sind bei
 * JPEG erlaubt, weil die Kosinustransformation ganzzahlig rechnet.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

static int checks, failures;

static void report(const char *name, bool ok, const char *detail)
{
    checks++;
    if (ok) {
        printf("  ok    %s\n", name);
    } else {
        failures++;
        printf("  FEHLER %s - %s\n", name, detail ? detail : "");
    }
}

static uint8_t *slurp(const char *path, size_t *length)
{
    FILE *f = fopen(path, "rb");

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);

    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc((size_t)size);

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *length = (size_t)size;
    return data;
}

/* Vergleicht mit dem Rohbild; erlaubt ist eine Abweichung je Kanal. */
static void compare(const char *name, const char *image_path,
                    const char *raw_path, int tolerance)
{
    size_t length = 0, raw_length = 0;
    uint8_t *data = slurp(image_path, &length);
    uint8_t *raw = slurp(raw_path, &raw_length);
    char detail[256];

    if (!data || !raw) {
        report(name, false, "Datei fehlt");
        free(data);
        free(raw);
        return;
    }

    struct image img;

    if (!image_decode(data, length, &img)) {
        report(name, false, "Lesen fehlgeschlagen");
        free(data);
        free(raw);
        return;
    }

    /* Die Rohdatei beginnt mit Breite und Hoehe als 32-Bit-Zahlen. */
    uint32_t w = ((uint32_t *)raw)[0];
    uint32_t h = ((uint32_t *)raw)[1];

    if ((uint32_t)img.w != w || (uint32_t)img.h != h) {
        snprintf(detail, sizeof(detail), "%dx%d statt %ux%u",
                 img.w, img.h, w, h);
        report(name, false, detail);
        image_free(&img);
        free(data);
        free(raw);
        return;
    }

    const uint8_t *expect = raw + 8;
    size_t pixels = (size_t)w * h;
    size_t bad = 0;
    int worst = 0;

    for (size_t i = 0; i < pixels; i++) {
        uint32_t got = img.px[i];
        const uint8_t *want = expect + i * 4;   /* RGBA */
        int diff[4] = {
            (int)((got >> 16) & 0xFF) - want[0],
            (int)((got >> 8) & 0xFF) - want[1],
            (int)(got & 0xFF) - want[2],
            (int)((got >> 24) & 0xFF) - want[3],
        };

        for (int c = 0; c < 4; c++) {
            int d = diff[c] < 0 ? -diff[c] : diff[c];

            if (d > worst)
                worst = d;
            if (d > tolerance) {
                bad++;
                break;
            }
        }
    }

    if (bad == 0) {
        snprintf(detail, sizeof(detail), "%ux%u, groesste Abweichung %d",
                 w, h, worst);
        printf("  ok    %s (%s)\n", name, detail);
        checks++;
    } else {
        snprintf(detail, sizeof(detail),
                 "%zu von %zu Pixeln daneben, groesste Abweichung %d",
                 bad, pixels, worst);
        report(name, false, detail);
    }

    image_free(&img);
    free(data);
    free(raw);
}

int main(void)
{
    printf("\nBildleser\n");
    compare("PNG, echte Farben",        "bilder/rgb.png",     "bilder/rgb.raw",     0);
    compare("PNG mit Alphakanal",       "bilder/rgba.png",    "bilder/rgba.raw",    0);
    compare("PNG mit Palette",          "bilder/palette.png", "bilder/palette.raw", 0);
    compare("PNG in Graustufen",        "bilder/grau.png",    "bilder/grau.raw",    0);
    compare("PNG mit 4 Bit Palette",    "bilder/pal4.png",    "bilder/pal4.raw",    0);
    compare("PNG mit 16 Bit je Kanal",  "bilder/rgb16.png",   "bilder/rgb16.raw",   0);
    compare("PNG, Adam7-verschraenkt",  "bilder/adam7.png",   "bilder/adam7.raw",   0);
    compare("GIF mit Palette",          "bilder/bild.gif",    "bilder/gif.raw",     0);
    compare("GIF, verschraenkt",        "bilder/lace.gif",    "bilder/lace.raw",    0);
    compare("BMP mit 24 Bit",           "bilder/bild.bmp",    "bilder/bmp.raw",     0);
    compare("JPEG, volle Abtastung",    "bilder/voll.jpg",    "bilder/voll.raw",    12);
    compare("JPEG, 4:2:0",              "bilder/halb.jpg",    "bilder/halb.raw",    24);
    compare("JPEG in Graustufen",       "bilder/grau.jpg",    "bilder/graujpg.raw", 12);

    printf("\n%d Pruefungen, %d Fehler\n\n", checks, failures);
    return failures ? 1 : 0;
}
