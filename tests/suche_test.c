/* suche_test.c - prueft das Bewerten und Ordnen der Suchtreffer.
 *
 * Das Suchfenster laesst sich hier nicht pruefen, die Reihenfolge
 * seiner Liste schon - und die ist das Ganze: Wer "uhr" tippt, will
 * die Uhr, nicht uhr.txt und erst recht nicht den Unterordner, in dem
 * "uhr" zufaellig vorkommt.
 */

#include <stdio.h>
#include <string.h>

#include "search.h"

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

static void pruefe_zahl(const char *was, long soll, long ist)
{
    geprueft++;
    if (soll != ist) {
        printf("  FEHLER: %s: erwartet %ld, bekommen %ld\n", was, soll, ist);
        fehler++;
    }
}

/* Findet a besser als b? */
static void besser(const char *was, const char *text_a, const char *text_b,
                   const char *query)
{
    int32_t a = search_score(text_a, query);
    int32_t b = search_score(text_b, query);

    geprueft++;
    if (a <= b) {
        printf("  FEHLER: %s: \"%s\" (%d) sollte vor \"%s\" (%d) stehen\n",
               was, text_a, (int)a, text_b, (int)b);
        fehler++;
    }
}

static void test_treffer(void)
{
    printf("Treffen und danebenliegen\n");

    pruefe("genau", search_score("Uhr", "Uhr") >= 0);
    pruefe("Anfang", search_score("Einstellungen", "Ein") >= 0);
    pruefe("Mitte", search_score("Systemmonitor", "monitor") >= 0);
    pruefe("Gross und klein egal", search_score("Uhr", "UHR") >= 0);
    pruefe("Gross und klein egal, andersherum",
           search_score("BILDER", "bild") >= 0);

    pruefe("nicht enthalten", search_score("Uhr", "Kalender") < 0);
    pruefe("laenger als der Text", search_score("Uhr", "Uhrzeit") < 0);
    pruefe("leere Eingabe", search_score("Uhr", "") < 0);
    pruefe("leerer Text", search_score("", "Uhr") < 0);
    pruefe("nichts und nichts", search_score(NULL, NULL) < 0);
    pruefe("kein Text", search_score(NULL, "Uhr") < 0);
    pruefe("keine Eingabe", search_score("Uhr", NULL) < 0);

    /* Ein Treffer ueber die ganze Laenge ist kein Sonderfall. */
    pruefe("ein Zeichen", search_score("A", "a") >= 0);
}

static void test_reihenfolge(void)
{
    printf("Was vor was steht\n");

    /* Der genaue Name schlaegt den laengeren. */
    besser("genau vor Anhang", "Uhr", "Uhrzeit anzeigen", "Uhr");

    /* Der Anfang schlaegt die Mitte. */
    besser("Anfang vor Mitte", "Monitor", "Systemmonitor", "monitor");

    /* Der Wortanfang schlaegt die Wortmitte. */
    besser("Wortanfang vor Wortmitte", "System Monitor", "Systemmonitor",
           "monitor");
    besser("nach Bindestrich", "Netz-Filter", "Netzfilter", "Filter");
    besser("nach Punkt", "bild.png", "bildpng", "png");
    besser("nach Schraegstrich", "/Bilder", "xBilder", "Bilder");

    /* Kurz schlaegt lang, wenn sonst alles gleich ist. */
    besser("kurz vor lang", "Bild", "Bilderverzeichnis lang", "Bild");

    /* Und die erste Fundstelle zaehlt: In "Zeichenkette" steckt "ein"
     * in der Mitte, in "Einstellungen" am Anfang. */
    besser("erste Fundstelle", "Einstellungen", "Zeichenkette", "ein");
}

/* Baut eine Liste mit vorgegebenen Punkten. */
static void fuelle(struct search_hit *hits, const int32_t *punkte, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        memset(&hits[i], 0, sizeof(hits[i]));
        hits[i].score = punkte[i];
        hits[i].index = i;
    }
}

static void test_sortieren(void)
{
    printf("Sortieren\n");

    struct search_hit hits[6];
    const int32_t punkte[] = { 10, 500, 300, 500, 1, 300 };

    fuelle(hits, punkte, 6);
    search_sort(hits, 6);

    pruefe_zahl("bester zuerst", 500, hits[0].score);
    pruefe_zahl("zweitbester", 500, hits[1].score);
    pruefe_zahl("dann 300", 300, hits[2].score);
    pruefe_zahl("dann 300", 300, hits[3].score);
    pruefe_zahl("dann 10", 10, hits[4].score);
    pruefe_zahl("zuletzt 1", 1, hits[5].score);

    /* Bei Gleichstand bleibt die Reihenfolge, in der gesammelt wurde -
     * so stehen Programme vor Dateien, wenn beide gleich gut passen. */
    pruefe_zahl("gleichstand: frueher zuerst", 1, (long)hits[0].index);
    pruefe_zahl("gleichstand: spaeter danach", 3, (long)hits[1].index);
    pruefe_zahl("gleichstand: 300 zuerst", 2, (long)hits[2].index);
    pruefe_zahl("gleichstand: 300 danach", 5, (long)hits[3].index);

    /* Eine leere und eine einelementige Liste duerfen nicht stolpern. */
    search_sort(hits, 0);
    search_sort(hits, 1);
    pruefe("kurze Listen", true);
}

int main(void)
{
    printf("=== Suche ===\n");

    test_treffer();
    test_reihenfolge();
    test_sortieren();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
