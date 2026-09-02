/* aufgaben_test.c - prueft den Kern der Aufgabenliste.
 *
 * Er kennt weder Fenster noch Dateien, nur Eintraege und Text - also
 * laesst er sich hier vollstaendig pruefen. Geprueft werden das
 * Anlegen und Entfernen, die Reihenfolge, in der die Liste erscheint,
 * das Lesen und Schreiben von Terminen samt Schaltjahren und der Weg
 * durch die Datei und zurueck.
 */

#include <stdio.h>
#include <string.h>

#include "tasks.h"

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

static struct tasklist liste;

/* Der Text der n-ten Zeile in der sortierten Ansicht. */
static const char *zeile(size_t index, bool ohne_erledigte)
{
    struct task *view[TASK_MAX];
    size_t n = tasks_sorted(&liste, view, TASK_MAX, ohne_erledigte);

    return index < n ? view[index]->text : "(fehlt)";
}

/* ------------------------------------------------------------------ */

static void test_anlegen(void)
{
    printf("Anlegen und entfernen\n");

    tasks_clear(&liste);
    pruefe("Frisch ist sie leer", tasks_count(&liste, false) == 0);

    struct task *a = tasks_add(&liste, "Bericht schreiben");
    struct task *b = tasks_add(&liste, "Milch kaufen");

    pruefe("Erste angelegt",      a != NULL);
    pruefe("Zweite angelegt",     b != NULL);
    pruefe("Verschiedene Nummern", a && b && a->id != b->id);
    pruefe("Beide gezaehlt",      tasks_count(&liste, false) == 2);
    pruefe("Beide offen",         tasks_count(&liste, true) == 2);
    pruefe("Standard ist mittel", a && a->prio == TP_MID);
    pruefe("Ohne Termin",         a && a->year == 0);

    /* Leerraum am Rand faellt weg, reiner Leerraum ergibt nichts. */
    struct task *c = tasks_add(&liste, "   Wohnung putzen   ");

    pruefe("Rand abgeschnitten", c && strcmp(c->text, "Wohnung putzen") == 0);
    pruefe("Nur Leerzeichen zaehlt nicht", tasks_add(&liste, "    ") == NULL);
    pruefe("Leerer Text auch nicht",       tasks_add(&liste, "") == NULL);

    pruefe("Ueber die Nummer wiederfinden",
           a && tasks_by_id(&liste, a->id) == a);
    pruefe("Fremde Nummer findet nichts",
           tasks_by_id(&liste, 99999) == NULL);

    b->done = true;
    pruefe("Erledigtes faellt aus der Zaehlung",
           tasks_count(&liste, true) == 2 && tasks_count(&liste, false) == 3);

    pruefe("Entfernen geht",     tasks_remove(&liste, b->id));
    pruefe("Danach ist es weg",  tasks_by_id(&liste, b->id) == NULL);
    pruefe("Zweimal geht nicht", !tasks_remove(&liste, b->id));
    pruefe("Noch zwei uebrig",   tasks_count(&liste, false) == 2);

    /* Die Liste ist endlich, und das sagt sie auch. */
    tasks_clear(&liste);
    for (int i = 0; i < TASK_MAX; i++) {
        char text[32];

        snprintf(text, sizeof(text), "Aufgabe %d", i);
        if (!tasks_add(&liste, text)) {
            printf("  FEHLER: Platz %d liess sich nicht belegen\n", i);
            fehler++;
            break;
        }
    }
    geprueft++;
    pruefe("Voll ist voll", tasks_add(&liste, "noch eine") == NULL);
    pruefe("Und zwar mit allen Plaetzen",
           tasks_count(&liste, false) == TASK_MAX);
}

static void test_reihenfolge(void)
{
    printf("Reihenfolge\n");

    tasks_clear(&liste);

    struct task *spaet   = tasks_add(&liste, "spaeter Termin");
    struct task *frueh   = tasks_add(&liste, "frueher Termin");
    tasks_add(&liste, "ohne Termin");
    struct task *fertig  = tasks_add(&liste, "schon erledigt");

    spaet->year = 2026; spaet->month = 12; spaet->day = 1;
    frueh->year = 2026; frueh->month = 1;  frueh->day = 5;
    fertig->done = true;
    fertig->year = 2020; fertig->month = 1; fertig->day = 1;

    /* Offenes vor Erledigtem, Termine vor Terminlosem, frueh vor spaet -
     * auch wenn das Erledigte den aeltesten Termin hat. */
    pruefe_text("Frueher Termin zuerst", "frueher Termin", zeile(0, false));
    pruefe_text("Dann der spaete",       "spaeter Termin", zeile(1, false));
    pruefe_text("Dann das Terminlose",   "ohne Termin",    zeile(2, false));
    pruefe_text("Erledigtes zuletzt",    "schon erledigt", zeile(3, false));

    pruefe_text("Ohne Erledigte faellt es raus", "ohne Termin",
                zeile(2, true));
    pruefe_text("Und dahinter kommt nichts", "(fehlt)", zeile(3, true));

    /* Bei gleichem Termin entscheidet die Wichtigkeit. */
    tasks_clear(&liste);

    struct task *mittel  = tasks_add(&liste, "b mittel");
    struct task *hoch    = tasks_add(&liste, "c hoch");
    struct task *niedrig = tasks_add(&liste, "a niedrig");

    mittel->prio = TP_MID;
    hoch->prio = TP_HIGH;
    niedrig->prio = TP_LOW;

    pruefe_text("Hoch zuerst",   "c hoch",    zeile(0, false));
    pruefe_text("Dann mittel",   "b mittel",  zeile(1, false));
    pruefe_text("Dann niedrig",  "a niedrig", zeile(2, false));

    /* Bei gleicher Wichtigkeit der Text - damit die Reihenfolge nicht
     * davon abhaengt, in welchem Steckplatz etwas gelandet ist. */
    tasks_clear(&liste);
    tasks_add(&liste, "Zwiebeln");
    tasks_add(&liste, "aepfel");
    tasks_add(&liste, "Brot");

    pruefe_text("Alphabetisch, Gross wie Klein", "aepfel", zeile(0, false));
    pruefe_text("Dann Brot",                     "Brot",   zeile(1, false));
    pruefe_text("Dann Zwiebeln",                 "Zwiebeln", zeile(2, false));

    /* Erledigte wegraeumen laesst die offenen stehen. */
    tasks_clear(&liste);
    tasks_add(&liste, "offen 1");
    tasks_add(&liste, "erledigt 1")->done = true;
    tasks_add(&liste, "erledigt 2")->done = true;
    tasks_add(&liste, "offen 2");

    pruefe("Zwei weggeraeumt", tasks_purge_done(&liste) == 2);
    pruefe("Zwei geblieben",   tasks_count(&liste, false) == 2);
    pruefe("Nichts mehr zu raeumen", tasks_purge_done(&liste) == 0);
}

static void test_termine(void)
{
    printf("Termine\n");

    uint16_t y = 0;
    uint8_t  m = 0, d = 0;

    pruefe("Deutsches Datum",
           tasks_parse_date("15.09.2026", &y, &m, &d) &&
           y == 2026 && m == 9 && d == 15);
    pruefe("Ohne fuehrende Null",
           tasks_parse_date("5.9.2026", &y, &m, &d) &&
           y == 2026 && m == 9 && d == 5);
    pruefe("Von hinten geschrieben",
           tasks_parse_date("2026-09-15", &y, &m, &d) &&
           y == 2026 && m == 9 && d == 15);
    pruefe("Mit Schraegstrichen",
           tasks_parse_date("15/09/2026", &y, &m, &d) && d == 15);

    pruefe("Ein Strich loescht",
           tasks_parse_date("-", &y, &m, &d) && y == 0);
    pruefe("Leer auch",
           tasks_parse_date("", &y, &m, &d) && y == 0);
    pruefe("Und Leerzeichen ebenso",
           tasks_parse_date("   ", &y, &m, &d) && y == 0);

    pruefe("Der 30. Februar nicht", !tasks_parse_date("30.02.2026", &y, &m, &d));
    pruefe("Der 29. Februar 2026 auch nicht",
           !tasks_parse_date("29.02.2026", &y, &m, &d));
    pruefe("2024 aber schon",
           tasks_parse_date("29.02.2024", &y, &m, &d) && d == 29);
    pruefe("1900 war kein Schaltjahr",
           !tasks_parse_date("29.02.1900", &y, &m, &d));
    pruefe("2000 aber doch",
           tasks_parse_date("29.02.2000", &y, &m, &d) && d == 29);

    pruefe("Der 13. Monat nicht",  !tasks_parse_date("01.13.2026", &y, &m, &d));
    pruefe("Der nullte Tag nicht", !tasks_parse_date("00.01.2026", &y, &m, &d));
    pruefe("Buchstaben nicht",     !tasks_parse_date("heute", &y, &m, &d));
    pruefe("Halbes Datum nicht",   !tasks_parse_date("15.09", &y, &m, &d));
    pruefe("Nachsatz nicht",       !tasks_parse_date("15.09.2026 abends",
                                                     &y, &m, &d));
    pruefe("Gemischte Trenner nicht",
           !tasks_parse_date("15.09-2026", &y, &m, &d));

    /* Ausgabe */
    tasks_clear(&liste);

    struct task *t = tasks_add(&liste, "Zahnarzt");
    char text[16];

    tasks_format_date(t, text, sizeof(text));
    pruefe_text("Ohne Termin ein Strich", "-", text);

    t->year = 2026; t->month = 9; t->day = 5;
    tasks_format_date(t, text, sizeof(text));
    pruefe_text("Mit fuehrenden Nullen", "05.09.2026", text);

    /* Ueberfaellig */
    pruefe("Gestern ist ueberfaellig", tasks_overdue(t, 2026, 9, 6));
    pruefe("Heute noch nicht",         !tasks_overdue(t, 2026, 9, 5));
    pruefe("Morgen erst recht nicht",  !tasks_overdue(t, 2026, 9, 4));
    pruefe("Naechstes Jahr schon",     tasks_overdue(t, 2027, 1, 1));

    t->done = true;
    pruefe("Erledigtes ist nie ueberfaellig", !tasks_overdue(t, 2027, 1, 1));

    t->done = false;
    t->year = 0;
    pruefe("Ohne Termin auch nicht", !tasks_overdue(t, 2027, 1, 1));
}

static void test_wichtigkeit(void)
{
    printf("Wichtigkeit\n");

    uint8_t p = TP_MID;

    pruefe("hoch",    task_prio_parse("hoch", &p) && p == TP_HIGH);
    pruefe("mittel",  task_prio_parse("mittel", &p) && p == TP_MID);
    pruefe("niedrig", task_prio_parse("niedrig", &p) && p == TP_LOW);
    pruefe("Gross geschrieben geht auch",
           task_prio_parse("HOCH", &p) && p == TP_HIGH);
    pruefe("Unsinn nicht",  !task_prio_parse("sehr", &p));
    pruefe("Leer nicht",    !task_prio_parse("", &p));

    pruefe_text("Name zu hoch",    "hoch",    task_prio_name(TP_HIGH));
    pruefe_text("Name zu mittel",  "mittel",  task_prio_name(TP_MID));
    pruefe_text("Name zu niedrig", "niedrig", task_prio_name(TP_LOW));
}

static void test_datei(void)
{
    printf("Aufgaben schreiben und lesen\n");

    tasks_clear(&liste);

    struct task *a = tasks_add(&liste, "Bericht schreiben");
    struct task *b = tasks_add(&liste, "Milch kaufen");
    struct task *c = tasks_add(&liste, "Steuer machen");

    a->prio = TP_HIGH;
    a->year = 2026; a->month = 9; a->day = 15;
    b->done = true;
    c->prio = TP_LOW;

    char text[8192];
    size_t len = tasks_to_text(&liste, text, sizeof(text));

    pruefe("Es kam Text heraus", len > 0 && strlen(text) == len);
    pruefe("Der Haken steht drin",   strstr(text, "[x]") != NULL);
    pruefe("Der offene auch",        strstr(text, "[ ]") != NULL);
    pruefe("Die Wichtigkeit auch",   strstr(text, "hoch") != NULL);
    pruefe("Und der Termin",         strstr(text, "15.09.2026") != NULL);

    tasks_from_text(&liste, text);

    pruefe("Alle drei wieder da", tasks_count(&liste, false) == 3);
    pruefe("Eine ist erledigt",   tasks_count(&liste, true) == 2);

    struct task *view[TASK_MAX];
    size_t n = tasks_sorted(&liste, view, TASK_MAX, false);

    pruefe("Sortiert wie vorher", n == 3);
    pruefe_text("Der Termin zuerst", "Bericht schreiben", view[0]->text);
    pruefe("Mit Wichtigkeit hoch",   view[0]->prio == TP_HIGH);
    pruefe("Und dem Termin",         view[0]->year == 2026 &&
                                     view[0]->month == 9 && view[0]->day == 15);
    pruefe_text("Erledigtes zuletzt", "Milch kaufen", view[2]->text);
    pruefe("Und ist erledigt",        view[2]->done);

    /* Zweimal hin und her muss dasselbe ergeben. */
    char again[8192];

    tasks_to_text(&liste, again, sizeof(again));
    pruefe_text("Zweiter Durchgang gleich", text, again);
}

static void test_von_hand(void)
{
    printf("Von Hand geschriebene Datei\n");

    /* Wer die Datei im Editor anlegt, soll nicht das ganze Format
     * kennen muessen: eine Zeile je Aufgabe genuegt. */
    tasks_from_text(&liste,
                    "# eine Liste\n"
                    "\n"
                    "Fenster putzen\n"
                    "[ ] Rasen maehen\n"
                    "[x] hoch Rechnung zahlen\n"
                    "[ ] niedrig 01.01.2027 Keller aufraeumen\n");

    pruefe("Vier Aufgaben gelesen", tasks_count(&liste, false) == 4);
    pruefe("Kommentar uebersprungen",
           tasks_by_id(&liste, 0) == NULL);

    struct task *view[TASK_MAX];
    size_t n = tasks_sorted(&liste, view, TASK_MAX, false);

    pruefe("Vier in der Ansicht", n == 4);
    pruefe_text("Der Termin steht vorn", "Keller aufraeumen", view[0]->text);
    pruefe("Mit Wichtigkeit niedrig",    view[0]->prio == TP_LOW);
    pruefe("Und dem Datum",              view[0]->year == 2027);

    /* Eine nackte Zeile wird zur offenen Aufgabe mittlerer Wichtigkeit. */
    bool gefunden = false;

    for (size_t i = 0; i < n; i++) {
        if (strcmp(view[i]->text, "Fenster putzen") != 0)
            continue;
        gefunden = !view[i]->done && view[i]->prio == TP_MID &&
                   view[i]->year == 0;
    }
    pruefe("Nackte Zeile wird offene Aufgabe", gefunden);

    /* Ein Wort, das wie eine Wichtigkeit aussieht, aber keine ist,
     * bleibt Teil des Textes. */
    tasks_from_text(&liste, "[ ] Hochbeet bauen\n");
    n = tasks_sorted(&liste, view, TASK_MAX, false);
    pruefe("Ein Eintrag", n == 1);
    pruefe_text("Text unversehrt", "Hochbeet bauen", view[0]->text);

    /* Leerer und fehlender Text aendern nichts, ausser dass geleert wird. */
    tasks_from_text(&liste, "");
    pruefe("Leerer Text ergibt leere Liste", tasks_count(&liste, false) == 0);

    tasks_from_text(&liste, NULL);
    pruefe("Und NULL auch", tasks_count(&liste, false) == 0);
}

int main(void)
{
    printf("=== Aufgaben ===\n");

    test_anlegen();
    test_reihenfolge();
    test_termine();
    test_wichtigkeit();
    test_datei();
    test_von_hand();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
