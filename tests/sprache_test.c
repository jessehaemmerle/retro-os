/* sprache_test.c - prueft die Uebersetzungstabelle.
 *
 * Eine Sprachtabelle geht nicht dadurch kaputt, dass ein Wort schlecht
 * uebersetzt ist - das sieht man. Sie geht dadurch kaputt, dass jemand
 * einen Eintrag von Hand einfuegt und damit die Sortierung zerstoert,
 * auf der die binaere Suche steht: Danach findet die Haelfte der
 * Eintraege nicht mehr statt, und zwar lautlos.
 *
 * Genauso lautlos ist der zweite Fehler: eine Uebersetzung, in der ein
 * Platzhalter fehlt oder die Reihenfolge zweier vertauscht ist. Aus
 * "%s of %s in use" wird dann irgendwann ein Absturz oder eine Zahl,
 * die als Zeiger gelesen wird. Beides steht hier.
 */

#include <stdio.h>
#include <string.h>

#include "lang.h"

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
    if (!ist || strcmp(soll, ist) != 0) {
        printf("  FEHLER: %s\n    erwartet: %s\n    bekommen: %s\n",
               was, soll, ist ? ist : "(nichts)");
        fehler++;
    }
}

/* --- Die Tabelle selbst -------------------------------------------- */

static void test_tabelle(void)
{
    printf("Aufbau der Tabelle\n");

    pruefe("Es gibt Eintraege", lang_entry_count() > 100);

    size_t unsortiert = 0;
    size_t leer = 0;
    size_t nicht_ascii = 0;

    for (size_t i = 0; i < lang_entry_count(); i++) {
        const char *de = lang_entry_de(i);
        const char *en = lang_entry_en(i);

        if (!de || !en || !de[0] || !en[0]) {
            leer++;
            continue;
        }

        /* Streng aufsteigend: gleich waere schon ein doppelter
         * Schluessel, und der zweite waere nie erreichbar. */
        if (i > 0 && strcmp(lang_entry_de(i - 1), de) >= 0) {
            if (unsortiert == 0)
                printf("    zuerst bei: %s\n", de);
            unsortiert++;
        }

        for (const char *p = de; *p; p++)
            if ((unsigned char)*p > 126)
                nicht_ascii++;
        for (const char *p = en; *p; p++)
            if ((unsigned char)*p > 126)
                nicht_ascii++;
    }

    pruefe("Streng aufsteigend sortiert", unsortiert == 0);
    pruefe("Keine leere Haelfte", leer == 0);
    pruefe("Nur ASCII - der Zeichensatz kann nicht mehr", nicht_ascii == 0);
}

/* --- Platzhalter ---------------------------------------------------- */

/* Sammelt die Umwandlungen einer Formatzeichenkette der Reihe nach
 * ein: aus "%s von %s belegt" wird "ss". "%%" zaehlt nicht mit. */
static void specs(const char *text, char *out, size_t size)
{
    size_t at = 0;

    for (const char *p = text; *p && at + 1 < size; p++) {
        if (*p != '%')
            continue;
        p++;
        if (*p == '%' || !*p)
            continue;

        /* Breite, Genauigkeit und Laenge ueberspringen - auf den
         * Buchstaben am Ende kommt es an. Ein Leerzeichen beendet die
         * Suche: "% und Klammern" ist ein Prozentzeichen in einem Satz
         * und keine Umwandlung, und genau so ein Satz steht in der
         * Erklaerung zu "rechne". */
        while (*p && strchr("-+#0123456789.", *p))
            p++;
        while (*p == 'l' || *p == 'h' || *p == 'z')
            p++;
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))
            out[at++] = *p;
        else
            p--;   /* nichts gefunden - das Zeichen selbst noch ansehen */
    }
    out[at] = '\0';
}

static void test_platzhalter(void)
{
    printf("Platzhalter\n");

    char probe[16];

    specs("%s von %s belegt", probe, sizeof(probe));
    pruefe_text("Zwei Zeichenketten", "ss", probe);
    specs("100 %% davon", probe, sizeof(probe));
    pruefe_text("Ein doppeltes Prozent zaehlt nicht", "", probe);
    specs("%-42s %s", probe, sizeof(probe));
    pruefe_text("Breite gehoert nicht dazu", "ss", probe);
    specs("Gehoert %s:%s - %s (%04o)", probe, sizeof(probe));
    pruefe_text("Auch oktal", "ssso", probe);
    specs("Es gibt + - * / % und Klammern", probe, sizeof(probe));
    pruefe_text("Ein Prozentzeichen im Satz ist keine Umwandlung", "", probe);

    size_t abweichend = 0;

    for (size_t i = 0; i < lang_entry_count(); i++) {
        char links[24], rechts[24];

        specs(lang_entry_de(i), links, sizeof(links));
        specs(lang_entry_en(i), rechts, sizeof(rechts));

        if (strcmp(links, rechts) != 0) {
            printf("    %s\n      de: %s\n      en: %s\n",
                   lang_entry_de(i), links, rechts);
            abweichend++;
        }
    }
    pruefe("Beide Haelften haben dieselben Platzhalter", abweichend == 0);
}

/* --- Nachschlagen --------------------------------------------------- */

static void test_suche(void)
{
    printf("Nachschlagen\n");

    /* Jeder Eintrag muss sich auch finden lassen - das ist die Probe
     * auf die binaere Suche und auf die Sortierung zugleich. */
    size_t verfehlt = 0;

    for (size_t i = 0; i < lang_entry_count(); i++) {
        const char *gefunden = lang_lookup(lang_entry_de(i));

        if (!gefunden || strcmp(gefunden, lang_entry_en(i)) != 0)
            verfehlt++;
    }
    pruefe("Jeder Eintrag wird gefunden", verfehlt == 0);

    pruefe("Was nicht da ist, bleibt weg",
           lang_lookup("Diesen Satz hat nie jemand geschrieben") == NULL);
    pruefe("Leer ist nichts", lang_lookup("") == NULL);
    pruefe("NULL bleibt NULL", lang_lookup(NULL) == NULL);
}

static void test_umschalten(void)
{
    printf("Umschalten\n");

    lang_select(LANG_DE);
    pruefe_text("Deutsch laesst den Text stehen", "Einstellungen",
                tr("Einstellungen"));
    pruefe("Auch das Unbekannte bleibt",
           strcmp(tr("Ein Satz ohne Eintrag"), "Ein Satz ohne Eintrag") == 0);

    lang_select(LANG_EN);
    pruefe_text("Englisch uebersetzt", "Settings", tr("Einstellungen"));
    pruefe_text("Auch mit Platzhaltern", "Saved to %s", tr("Gespeichert in %s"));
    pruefe_text("Ohne Eintrag bleibt es deutsch", "Ein Satz ohne Eintrag",
                tr("Ein Satz ohne Eintrag"));
    pruefe("NULL bleibt auch hier NULL", tr(NULL) == NULL);

    pruefe("Kuerzel de", lang_select_by_code("de") && lang_current() == LANG_DE);
    pruefe("Kuerzel en", lang_select_by_code("en") && lang_current() == LANG_EN);
    pruefe("Gross geschrieben geht auch", lang_select_by_code("EN"));
    pruefe("Unbekanntes Kuerzel aendert nichts",
           !lang_select_by_code("kl") && lang_current() == LANG_EN);
    pruefe("NULL aendert nichts", !lang_select_by_code(NULL));

    /* Ueber das Ende hinaus wird nichts gewaehlt - sonst stuende ein
     * Zaehler ausserhalb der Tabelle. */
    lang_select(LANG_COUNT);
    pruefe("Ueber das Ende hinaus bleibt es stehen", lang_current() == LANG_EN);

    pruefe_text("Namen der Sprachen", "Deutsch", lang_name(LANG_DE));
    pruefe_text("Auf Englisch heisst sie English", "English",
                lang_name(LANG_EN));
    pruefe_text("Kuerzel zurueck", "de", lang_code(LANG_DE));
    pruefe_text("Und das andere", "en", lang_code(LANG_EN));

    pruefe_text("Deutsch bekommt die deutsche Belegung", "de",
                lang_default_keymap(LANG_DE));
    pruefe_text("Englisch die amerikanische", "us",
                lang_default_keymap(LANG_EN));

    lang_select(LANG_DE);
}

/* --- Was in der Tabelle stehen muss --------------------------------- */

static void test_inhalt(void)
{
    printf("Was drinstehen muss\n");

    lang_select(LANG_EN);

    /* Eine Handvoll Stellen, an denen ein fehlender Eintrag sofort
     * auffiele: das Startmenue, die zwei Knoepfe jeder Rueckfrage, der
     * Name jedes mitgelieferten Programms. */
    pruefe_text("Startmenue", "Start", tr("Start"));
    pruefe_text("Abschalten", "Shut down", tr("Herunterfahren"));
    pruefe_text("Ja", "Yes", tr("Ja"));
    pruefe_text("Nein", "No", tr("Nein"));
    pruefe_text("Abbrechen", "Cancel", tr("Abbrechen"));
    pruefe_text("Dateimanager", "Files", tr("Dateimanager"));
    pruefe_text("Konsole", "Console", tr("Konsole"));
    pruefe_text("Papierkorb", "Trash", tr("Papierkorb"));
    pruefe_text("Sprache", "Language", tr("Sprache"));
    pruefe_text("Hintergrundbild", "Wallpaper", tr("Hintergrundbild"));
    pruefe_text("Belegung", "English UK (QWERTY)", tr("Englisch GB (QWERTY)"));
    pruefe_text("Ein Bereich der Konsole", "Security", tr("Sicherheit"));

    /* Mehrzeiliges bleibt mehrzeilig - sonst steht die Erklaerung zu
     * einem Befehl auf einer Zeile, die niemand lesen kann. */
    const char *mehrzeilig =
        tr("Im Muster steht * fuer beliebig viele Zeichen und ? fuer eines.");
    pruefe("Auch die Erklaerungen sind da",
           strcmp(mehrzeilig,
                  "Im Muster steht * fuer beliebig viele Zeichen und ? fuer eines.")
           != 0);

    const char *zwei = tr("Ohne Angabe stehen die Profile mit ihren Rechten da.");
    pruefe("Und die zu kaefig", strchr(zwei, ' ') != NULL);

    lang_select(LANG_DE);
}

int main(void)
{
    printf("=== Sprache ===\n");

    test_tabelle();
    test_platzhalter();
    test_suche();
    test_umschalten();
    test_inhalt();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
