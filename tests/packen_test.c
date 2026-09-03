/* packen_test.c - prueft den DEFLATE-Packer.
 *
 * Ein Packer laesst sich nur daran pruefen, ob das Gepackte wieder
 * herauskommt - und zwar bei einem fremden Entpacker genauso wie beim
 * eigenen. Der eigene steht im selben Haus (inflate.c) und ist gegen
 * echte Dateien geprueft; ein zweiter Durchgang mit zlib laeuft im
 * Testskript daneben.
 *
 * Geprueft werden die Stellen, an denen ein Packer schiefliegt: der
 * leere Eingang, Daten ohne jede Wiederholung, Daten aus lauter
 * Wiederholung, Uebereinstimmungen genau an den Grenzen von drei und
 * 258 Bytes, und Abstaende bis an das Fenster von 32 KB.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deflate.h"
#include "inflate.h"

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

/* Packt, entpackt und vergleicht. Liefert die gepackte Groesse, damit
 * der Aufrufer sehen kann, ob ueberhaupt gepackt wurde. */
static size_t rundlauf(const char *was, const uint8_t *data, size_t length)
{
    size_t packed_len = 0;
    uint8_t *packed = deflate_raw(data, length, &packed_len);

    geprueft++;
    if (!packed) {
        printf("  FEHLER: %s - packen ging nicht\n", was);
        fehler++;
        return 0;
    }

    size_t back_len = 0;
    uint8_t *back = inflate_raw(packed, packed_len, &back_len);

    geprueft++;
    if (!back) {
        printf("  FEHLER: %s - entpacken ging nicht (%zu Bytes)\n",
               was, packed_len);
        fehler++;
        free(packed);
        return packed_len;
    }

    geprueft++;
    if (back_len != length) {
        printf("  FEHLER: %s - %zu Bytes statt %zu\n", was, back_len, length);
        fehler++;
    } else if (length && memcmp(back, data, length) != 0) {
        printf("  FEHLER: %s - Inhalt weicht ab\n", was);
        fehler++;
    }

    free(back);
    free(packed);
    return packed_len;
}

static void test_grenzfaelle(void)
{
    printf("Grenzfaelle\n");

    rundlauf("Nichts", (const uint8_t *)"", 0);
    rundlauf("Ein Byte", (const uint8_t *)"A", 1);
    rundlauf("Zwei Bytes", (const uint8_t *)"AB", 2);

    /* Genau drei gleiche Bytes: die kuerzeste Uebereinstimmung, die
     * DEFLATE kennt. Ein Byte weniger, und es muessen Literale sein. */
    rundlauf("Dreimal dasselbe", (const uint8_t *)"aaa", 3);
    rundlauf("Sechsmal dasselbe", (const uint8_t *)"abcabc", 6);

    /* Alle 256 Bytewerte - dabei kommen die Kennungen mit neun Bit
     * zum Zug, die es nur oberhalb von 143 gibt. */
    uint8_t alle[256];

    for (int i = 0; i < 256; i++)
        alle[i] = (uint8_t)i;
    rundlauf("Alle Bytewerte", alle, sizeof(alle));
}

static void test_wiederholung(void)
{
    printf("Wiederholung\n");

    /* Lauter dasselbe - hier muss der Packer deutlich kleiner werden,
     * sonst sucht er gar nicht. */
    uint8_t *gleich = malloc(100000);

    memset(gleich, 'x', 100000);

    size_t size = rundlauf("Hunderttausend gleiche", gleich, 100000);

    pruefe("Gleiches wird sehr klein", size > 0 && size < 1000);
    free(gleich);

    /* Genau 258 Bytes - die laengste Uebereinstimmung, die DEFLATE
     * kennt. Dahinter muss eine zweite anfangen. */
    uint8_t lang[600];

    for (size_t i = 0; i < sizeof(lang); i++)
        lang[i] = 'q';
    rundlauf("Ueber die Laengengrenze", lang, sizeof(lang));

    /* Ein Text, der sich alle paar Zeilen wiederholt - der Normalfall. */
    static const char zeile[] =
        "RetroOS ist ein kleines Betriebssystem fuer x86-64-Rechner.\n";
    size_t n = sizeof(zeile) - 1;
    uint8_t *text = malloc(n * 200);

    for (int i = 0; i < 200; i++)
        memcpy(text + (size_t)i * n, zeile, n);

    size = rundlauf("Wiederholter Text", text, n * 200);
    pruefe("Text wird deutlich kleiner", size > 0 && size < n * 200 / 10);
    free(text);
}

static void test_abstaende(void)
{
    printf("Abstaende\n");

    /* Eine Uebereinstimmung ueber fast das ganze Fenster hinweg: Am
     * Anfang ein Muster, dann 32000 Bytes Fuellung, dann dasselbe
     * Muster wieder. Nur wer weit genug zurueckschaut, findet es. */
    size_t n = 32000 + 128;
    uint8_t *data = malloc(n);
    unsigned seed = 12345;

    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        data[i] = (uint8_t)(seed >> 16);
    }
    memcpy(data + n - 64, data, 64);

    rundlauf("Weiter Abstand", data, n);
    free(data);

    /* Zufall laesst sich nicht packen - das Ergebnis darf trotzdem
     * nicht kaputt sein, nur groesser. */
    n = 20000;
    data = malloc(n);
    seed = 999;
    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        data[i] = (uint8_t)(seed >> 16);
    }

    size_t size = rundlauf("Zufall", data, n);

    /* An Zufall ist nichts zu holen. Feste Huffman-Baeume wuerden ihn
     * sogar aufblaehen - ein guter Packer schreibt dann ungepackt und
     * bleibt knapp darueber. */
    pruefe("Zufall bleibt fast gleich gross",
           size > 0 && size < n + n / 100 + 16);
    free(data);
}

static void test_zlib_huelle(void)
{
    printf("zlib-Huelle\n");

    static const char text[] =
        "Dieselben Daten, diesmal mit Huelle. Dieselben Daten, diesmal "
        "mit Huelle. Dieselben Daten, diesmal mit Huelle.";
    size_t len = sizeof(text) - 1;
    size_t packed_len = 0;
    uint8_t *packed = deflate_zlib((const uint8_t *)text, len, &packed_len);

    pruefe("Packen mit Huelle geht", packed != NULL);
    if (!packed)
        return;

    pruefe("Der Kopf stimmt", packed_len > 6 && packed[0] == 0x78);
    pruefe("Kopf durch 31 teilbar",
           ((packed[0] << 8) | packed[1]) % 31 == 0);

    size_t back_len = 0;
    uint8_t *back = inflate_zlib(packed, packed_len, &back_len);

    pruefe("Entpacken mit Huelle geht", back != NULL);
    if (back) {
        pruefe("Inhalt stimmt",
               back_len == len && memcmp(back, text, len) == 0);
        free(back);
    }

    /* Die Adler-Summe am Ende muss zur Eingabe passen. */
    uint32_t stored = ((uint32_t)packed[packed_len - 4] << 24) |
                      ((uint32_t)packed[packed_len - 3] << 16) |
                      ((uint32_t)packed[packed_len - 2] << 8) |
                      packed[packed_len - 1];

    pruefe("Adler-32 stimmt", stored == adler32(text, len));
    free(packed);
}

/* Schreibt ein gepacktes Stueck auf die Platte, damit das Testskript
 * es einem fremden Entpacker vorlegen kann. */
static void schreibe_probe(void)
{
    static const char text[] =
        "Ein Text, den ein fremder Entpacker lesen koennen muss. "
        "Ein Text, den ein fremder Entpacker lesen koennen muss. "
        "Und noch ein Stueck, damit es etwas zu finden gibt.";
    size_t len = sizeof(text) - 1;
    size_t packed_len = 0;
    uint8_t *packed = deflate_zlib((const uint8_t *)text, len, &packed_len);

    if (!packed)
        return;

    FILE *f = fopen("probe.zlib", "wb");

    if (f) {
        fwrite(packed, 1, packed_len, f);
        fclose(f);
    }

    f = fopen("probe.txt", "wb");
    if (f) {
        fwrite(text, 1, len, f);
        fclose(f);
    }
    free(packed);
}

int main(void)
{
    printf("=== Packen ===\n");

    test_grenzfaelle();
    test_wiederholung();
    test_abstaende();
    test_zlib_huelle();
    schreibe_probe();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
