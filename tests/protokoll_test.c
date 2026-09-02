/* protokoll_test.c - prueft den Ring des Systemprotokolls.
 *
 * Der Ring ist die Stelle, an der ein Protokoll gern kaputtgeht: Er
 * muss ueberlaufen duerfen, ohne dass die Zaehlung durcheinandergeraet,
 * und die aelteste noch vorhandene Meldung muss auch nach dem
 * tausendsten Umlauf die richtige sein.
 *
 * Dazu kommt die Zerlegung dessen, was kprintf schreibt: Aus
 * "Netzwerk    : bereit" soll die Herkunft "Netzwerk" und der Text
 * "bereit" werden, und aus einer Zeile mit "kein" eine Warnung.
 */

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "vfs.h"

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

static void pruefe_text(const char *was, const char *soll, const char *ist)
{
    geprueft++;
    if (strcmp(soll, ist) != 0) {
        printf("  FEHLER: %s - erwartet \"%s\", bekommen \"%s\"\n",
               was, soll, ist);
        fehler++;
    }
}

/* ------------------------------------------------------------------ */
/* Ersatz fuer den Kernel                                              */
/* ------------------------------------------------------------------ */

/* Uhr und Kalender kommen aus shim.c. Der Dateibaum wird nur beim Sichern angefasst; hier landet alles in
 * einem Puffer, damit sich der geschriebene Text pruefen laesst. */
static char   datei[65536];
static size_t datei_laenge;
static bool   darf_schreiben = true;

static struct fs_node die_datei;

struct fs_node *fs_lookup(struct fs_node *base, const char *path)
{
    (void)base;
    (void)path;
    die_datei.type = FS_FILE;
    return darf_schreiben ? &die_datei : NULL;
}

struct fs_node *fs_create_path(struct fs_node *base, const char *path,
                               enum fs_type type)
{
    (void)base;
    (void)path;
    die_datei.type = (uint8_t)type;
    return darf_schreiben ? &die_datei : NULL;
}

bool fs_write(struct fs_node *file, const void *data, size_t size)
{
    if (!file || !darf_schreiben || size >= sizeof(datei))
        return false;
    memcpy(datei, data, size);
    datei[size] = '\0';
    datei_laenge = size;
    return true;
}

/* ------------------------------------------------------------------ */

/* Legt count Meldungen ab, damit der Ring ueberlaeuft. */
static void fuellen(size_t count)
{
    for (size_t i = 0; i < count; i++)
        log_info("test", "Meldung %u", (unsigned)i);
}

static void test_ring(void)
{
    printf("Der Ring\n");

    log_clear();

    /* log_clear schreibt selbst eine Meldung - das ist Absicht, sonst
     * stuende im Protokoll nicht, dass es geleert wurde. */
    pruefe("Nach dem Leeren steht die Notiz darin", log_count() == 1);
    pruefe("Nichts verloren", log_lost() == 0);

    log_clear();
    fuellen(10);
    pruefe("Zehn dazu", log_count() == 11);

    struct log_entry e;

    pruefe("Erste lesbar", log_get(1, &e));
    pruefe_text("Und es ist die erste", "Meldung 0", e.text);
    pruefe_text("Mit der richtigen Herkunft", "test", e.source);
    pruefe("Und der Dringlichkeit", e.level == LOG_INFO);

    pruefe("Letzte lesbar", log_get(10, &e));
    pruefe_text("Und es ist die letzte", "Meldung 9", e.text);

    pruefe("Ueber das Ende hinaus nicht", !log_get(11, &e));

    /* Ueberlaufen: Der Ring behaelt die juengsten LOG_ENTRIES. */
    log_clear();
    fuellen(LOG_ENTRIES + 100);

    pruefe("Voll, aber nicht voller", log_count() == LOG_ENTRIES);
    pruefe("Und der Rest ist gezaehlt", log_lost() == 101);

    pruefe("Die aelteste ist lesbar", log_get(0, &e));

    /* Geschrieben wurden eine Notiz vom Leeren und LOG_ENTRIES + 100
     * Meldungen; uebrig sind die letzten LOG_ENTRIES. */
    char erwartet[32];

    snprintf(erwartet, sizeof(erwartet), "Meldung %u",
             (unsigned)(LOG_ENTRIES + 100 - LOG_ENTRIES));
    pruefe_text("Und es ist die richtige", erwartet, e.text);

    pruefe("Die juengste auch", log_get(LOG_ENTRIES - 1, &e));
    snprintf(erwartet, sizeof(erwartet), "Meldung %u",
             (unsigned)(LOG_ENTRIES + 100 - 1));
    pruefe_text("Und auch die stimmt", erwartet, e.text);

    /* Die laufende Nummer zaehlt weiter, auch was aus dem Ring faellt. */
    pruefe("Laufende Nummer zaehlt alles",
           e.seq == (uint32_t)(LOG_ENTRIES + 101));
}

static void test_dringlichkeit(void)
{
    printf("Dringlichkeiten\n");

    log_clear();
    log_debug("a", "eins");
    log_info("b", "zwei");
    log_warn("c", "drei");
    log_error("d", "vier");
    log_error("e", "fuenf");

    pruefe("Eine Suche",    log_count_level(LOG_DEBUG) == 1);
    /* Das Leeren selbst ist ein Hinweis - darum zwei. */
    pruefe("Zwei Hinweise", log_count_level(LOG_INFO) == 2);
    pruefe("Eine Warnung",  log_count_level(LOG_WARN) == 1);
    pruefe("Zwei Fehler",   log_count_level(LOG_ERROR) == 2);

    pruefe_text("Name zu Warnung", "Warnung", log_level_name(LOG_WARN));
    pruefe_text("Kurz zu Warnung", "WRN",     log_level_short(LOG_WARN));
    pruefe_text("Name zu Fehler",  "Fehler",  log_level_name(LOG_ERROR));
    pruefe_text("Kurz zu Hinweis", "inf",     log_level_short(LOG_INFO));

    /* Ein zu langer Text wird abgeschnitten, nicht ueberschrieben. */
    log_clear();

    char lang[400];

    memset(lang, 'x', sizeof(lang) - 1);
    lang[sizeof(lang) - 1] = '\0';
    log_info("test", "%s", lang);

    struct log_entry e;

    pruefe("Die lange Meldung ist da", log_get(1, &e));
    pruefe("Auf die Feldlaenge gekuerzt", strlen(e.text) == LOG_TEXT_MAX);
}

static void test_kprintf(void)
{
    printf("Was kprintf schreibt\n");

    log_clear();

    const char *zeile = "Netzwerk    : 10.0.2.15, Gateway 10.0.2.2\n";

    for (const char *p = zeile; *p; p++)
        log_kernel_char(*p);

    struct log_entry e;

    pruefe("Die Zeile ist angekommen", log_get(1, &e));
    pruefe_text("Herkunft abgetrennt", "Netzwerk", e.source);
    pruefe_text("Text ohne Doppelpunkt", "10.0.2.15, Gateway 10.0.2.2", e.text);
    pruefe("Und gilt als Hinweis", e.level == LOG_INFO);

    /* Eine Zeile ohne Doppelpunkt bleibt ganz und kommt vom Kern. */
    log_clear();
    for (const char *p = "Ein einfacher Satz\n"; *p; p++)
        log_kernel_char(*p);

    pruefe("Auch die ist da", log_get(1, &e));
    pruefe_text("Vollstaendig",  "Ein einfacher Satz", e.text);
    pruefe_text("Vom Kern",      "kern", e.source);

    /* Ein Doppelpunkt weit hinten gehoert zum Text und nicht zur
     * Herkunft - sonst wuerde jeder Satz zerschnitten. */
    log_clear();
    for (const char *p = "Das hier ist ein langer Satz: mit Doppelpunkt\n";
         *p; p++)
        log_kernel_char(*p);

    pruefe("Vorhanden", log_get(1, &e));
    pruefe_text("Herkunft bleibt der Kern", "kern", e.source);
    pruefe_text("Text unversehrt",
                "Das hier ist ein langer Satz: mit Doppelpunkt", e.text);

    /* Woerter, die auf einen Fehlschlag hindeuten, faerben die Zeile. */
    log_clear();
    for (const char *p = "Datentraeger: kein FAT32 gefunden\n"; *p; p++)
        log_kernel_char(*p);

    pruefe("Vorhanden", log_get(1, &e));
    pruefe("Wird als Warnung gewertet", e.level == LOG_WARN);

    /* Wagenruecklauf wird weggeworfen, leere Zeilen ergeben nichts. */
    log_clear();
    for (const char *p = "\n\r\nMit Ruecklauf\r\n"; *p; p++)
        log_kernel_char(*p);

    pruefe("Nur eine Meldung dazu", log_count() == 2);
    pruefe("Und die richtige", log_get(1, &e));
    pruefe_text("Ohne Ruecklauf", "Mit Ruecklauf", e.text);
}

static void test_sichern(void)
{
    printf("Sichern\n");

    log_clear();
    log_info("start", "Alles bereit");
    log_warn("platte", "keine gefunden");

    datei_laenge = 0;
    darf_schreiben = true;
    pruefe("Schreiben geht", log_save("/Festplatte/protokoll.txt"));
    pruefe("Es steht etwas darin", datei_laenge > 0);
    pruefe("Mit Kopfzeile",  strstr(datei, "# Systemprotokoll") != NULL);
    pruefe("Und den Meldungen",
           strstr(datei, "Alles bereit") != NULL &&
           strstr(datei, "keine gefunden") != NULL);
    pruefe("Die Warnung ist erkennbar", strstr(datei, "WRN") != NULL);
    pruefe("Mit Zeitstempel in eckigen Klammern", strchr(datei, '[') != NULL);

    /* Sichern schreibt selbst eine Meldung - so steht im Protokoll,
     * wann es gesichert wurde. */
    pruefe("Und es merkt sich das", log_count_level(LOG_INFO) >= 2);

    darf_schreiben = false;
    pruefe("Ohne Ziel schlaegt es fehl", !log_save("/nirgends.txt"));
    darf_schreiben = true;
}

int main(void)
{
    printf("=== Systemprotokoll ===\n");

    test_ring();
    test_dringlichkeit();
    test_kprintf();
    test_sichern();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
