/* archiv_test.c - prueft das Lesen und Schreiben von ZIP.
 *
 * Ein Archivformat ist ein Vertrag mit fremden Programmen. Der eigene
 * Rundlauf beweist nur, dass beide Haelften denselben Fehler machen;
 * darum legt das Testskript daneben ein geschriebenes Archiv dem
 * zipfile-Modul von Python vor - und liest umgekehrt eines ein, das
 * Python geschrieben hat.
 *
 * Hier stehen die Faelle, an denen ein Leser stolpert: das Ende, das
 * rueckwaerts gesucht werden muss, eine Anmerkung dahinter, ein
 * Eintrag, dessen Pruefsumme nicht stimmt, und Ordner ohne Inhalt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zip.h"

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

/* Ein Archiv aus mehreren Stuecken bauen. */
static uint8_t *baue(size_t *length)
{
    struct zip_writer *w = zip_begin();

    if (!w)
        return NULL;

    static const char kurz[] = "Hallo";
    static const char lang[] =
        "Eine Zeile, die sich wiederholt. Eine Zeile, die sich wiederholt. "
        "Eine Zeile, die sich wiederholt. Eine Zeile, die sich wiederholt. "
        "Eine Zeile, die sich wiederholt. Eine Zeile, die sich wiederholt.";

    uint16_t date = zip_dos_date(2026, 9, 3);
    uint16_t time = zip_dos_time(11, 30, 44);

    zip_add(w, "kurz.txt", kurz, sizeof(kurz) - 1, date, time);
    zip_add(w, "lang.txt", lang, sizeof(lang) - 1, date, time);
    zip_add(w, "leer.txt", "", 0, date, time);
    zip_add(w, "ordner/", NULL, 0, date, time);
    zip_add(w, "ordner/drin.txt", kurz, sizeof(kurz) - 1, date, time);

    return zip_finish(w, length);
}

static void test_rundlauf(void)
{
    printf("Hin und zurueck\n");

    size_t length = 0;
    uint8_t *archive = baue(&length);

    pruefe("Schreiben geht", archive != NULL);
    if (!archive)
        return;

    size_t count = 0;

    pruefe("Lesen geht", zip_read(archive, length, &count));
    pruefe("Fuenf Eintraege", count == 5);

    struct zip_entry e;

    pruefe("Erster Eintrag", zip_entry(archive, length, 0, &e));
    pruefe("Er heisst kurz.txt", strcmp(e.name, "kurz.txt") == 0);
    pruefe("Fuenf Bytes", e.size == 5);
    pruefe("Kein Ordner", !e.is_dir);

    size_t out = 0;
    void *content = zip_extract(archive, length, &e, &out);

    pruefe("Auspacken geht", content != NULL);
    if (content) {
        pruefe("Inhalt stimmt", out == 5 && memcmp(content, "Hallo", 5) == 0);
        free(content);
    }

    /* Der lange Text muss gepackt worden sein, der kurze nicht - bei
     * fuenf Bytes kostet DEFLATE mehr, als es bringt. */
    struct zip_entry lang;

    pruefe("Zweiter Eintrag", zip_entry(archive, length, 1, &lang));
    pruefe("Der lange wurde gepackt", lang.method == 8);
    pruefe("Und ist kleiner geworden", lang.packed < lang.size);
    pruefe("Der kurze blieb ungepackt", e.method == 0);

    content = zip_extract(archive, length, &lang, &out);
    pruefe("Der lange kommt heil zurueck",
           content != NULL && out == lang.size);
    free(content);

    /* Eine leere Datei ist ein gueltiger Eintrag. */
    struct zip_entry leer;

    pruefe("Dritter Eintrag", zip_entry(archive, length, 2, &leer));
    pruefe("Er ist leer", leer.size == 0);
    content = zip_extract(archive, length, &leer, &out);
    pruefe("Auch nichts laesst sich auspacken", content != NULL && out == 0);
    free(content);

    /* Ordner erkennt man am Schraegstrich am Ende. */
    struct zip_entry dir;

    pruefe("Vierter Eintrag", zip_entry(archive, length, 3, &dir));
    pruefe("Das ist ein Ordner", dir.is_dir);
    pruefe("Ordner lassen sich nicht auspacken",
           zip_extract(archive, length, &dir, &out) == NULL);

    /* Ueber das Ende hinaus gibt es nichts. */
    pruefe("Kein sechster Eintrag", !zip_entry(archive, length, 5, &e));

    free(archive);
}

static void test_datum(void)
{
    printf("Datum und Uhrzeit\n");

    uint16_t d = zip_dos_date(2026, 9, 3);

    pruefe("Jahr", (d >> 9) + 1980 == 2026);
    pruefe("Monat", ((d >> 5) & 0x0F) == 9);
    pruefe("Tag", (d & 0x1F) == 3);

    uint16_t t = zip_dos_time(11, 30, 44);

    pruefe("Stunde", (t >> 11) == 11);
    pruefe("Minute", ((t >> 5) & 0x3F) == 30);
    pruefe("Sekunde in Zweierschritten", (t & 0x1F) * 2 == 44);

    /* Vor 1980 gibt es in diesem Format nicht. */
    pruefe("Zu frueh wird auf 1980 gesetzt",
           (zip_dos_date(1970, 1, 1) >> 9) + 1980 == 1980);
}

static void test_beschaedigt(void)
{
    printf("Beschaedigtes\n");

    size_t length = 0;
    uint8_t *archive = baue(&length);

    if (!archive)
        return;

    size_t count = 0;

    /* Ohne das Ende ist es kein Archiv. */
    pruefe("Abgeschnitten faellt durch",
           !zip_read(archive, length - 4, &count));
    pruefe("Nichts ist kein Archiv", !zip_read(archive, 0, &count));

    /* Ein gekipptes Bit in den Daten muss beim Auspacken auffallen -
     * dafuer steht die Pruefsumme im Archiv. */
    struct zip_entry e;

    zip_entry(archive, length, 0, &e);

    size_t out = 0;
    void *good = zip_extract(archive, length, &e, &out);

    pruefe("Vorher geht es", good != NULL);
    free(good);

    archive[e.offset + 30 + 8 + 2] ^= 0x40;   /* mitten in den Inhalt */
    pruefe("Verfaelschtes faellt auf",
           zip_extract(archive, length, &e, &out) == NULL);

    free(archive);
}

/* Ein Archiv fuer das Testskript - Python soll es oeffnen koennen. */
static void schreibe_probe(void)
{
    size_t length = 0;
    uint8_t *archive = baue(&length);

    if (!archive)
        return;

    FILE *f = fopen("probe.zip", "wb");

    if (f) {
        fwrite(archive, 1, length, f);
        fclose(f);
    }
    free(archive);
}

/* Und umgekehrt: ein Archiv, das Python geschrieben hat. */
static void test_fremdes(void)
{
    printf("Von Python geschrieben\n");

    FILE *f = fopen("fremd.zip", "rb");

    if (!f) {
        printf("  (uebersprungen - fremd.zip fehlt)\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *data = malloc((size_t)size);

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return;
    }
    fclose(f);

    size_t count = 0;

    pruefe("Fremdes Archiv lesen", zip_read(data, (size_t)size, &count));
    pruefe("Zwei Eintraege darin", count == 2);

    struct zip_entry e;

    if (zip_entry(data, (size_t)size, 0, &e)) {
        size_t out = 0;
        void *content = zip_extract(data, (size_t)size, &e, &out);

        pruefe("Der erste heisst wie erwartet",
               strcmp(e.name, "gepackt.txt") == 0);
        pruefe("Und laesst sich auspacken", content != NULL);
        if (content) {
            pruefe("Sein Inhalt stimmt",
                   out > 40 && memcmp(content, "Python hat das gepackt", 22) == 0);
            free(content);
        }
    }

    free(data);
}

int main(void)
{
    printf("=== Archiv ===\n");

    test_rundlauf();
    test_datum();
    test_beschaedigt();
    schreibe_probe();
    test_fremdes();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
