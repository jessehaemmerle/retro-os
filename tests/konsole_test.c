/* konsole_test.c - prueft die rechnenden Teile der Konsole.
 *
 * Die Schleifen ueber den Dateibaum sind nicht die Stellen, an denen
 * eine Konsole falsch liegt. Falsch liegt sie beim Mustervergleich -
 * ein Stern am Ende, zwei Sterne hintereinander, ein Muster, das laenger
 * ist als der Name -, beim Vorrang im Ausdruck und beim Wochentag im
 * Februar eines Schaltjahres. Genau das steht hier.
 */

#include <stdio.h>
#include <string.h>

#include "shellutil.h"

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
        printf("  FEHLER: %s\n    erwartet: %s\n    bekommen: %s\n",
               was, soll, ist);
        fehler++;
    }
}

static void pruefe_zahl(const char *ausdruck, int64_t soll)
{
    int64_t ist = 0;
    char error[80] = "";

    geprueft++;
    if (!sh_eval(ausdruck, &ist, error, sizeof(error))) {
        printf("  FEHLER: \"%s\" ging nicht: %s\n", ausdruck, error);
        fehler++;
        return;
    }
    if (ist != soll) {
        printf("  FEHLER: \"%s\" - erwartet %ld, bekommen %ld\n", ausdruck,
               (long)soll, (long)ist);
        fehler++;
    }
}

static void pruefe_fehler(const char *ausdruck)
{
    int64_t ist = 0;
    char error[80] = "";

    geprueft++;
    if (sh_eval(ausdruck, &ist, error, sizeof(error))) {
        printf("  FEHLER: \"%s\" haette scheitern muessen, gab %ld\n",
               ausdruck, (long)ist);
        fehler++;
        return;
    }
    if (!error[0]) {
        printf("  FEHLER: \"%s\" scheiterte ohne Begruendung\n", ausdruck);
        fehler++;
    }
}

/* ------------------------------------------------------------------ */

static void test_muster(void)
{
    printf("Namensmuster\n");

    pruefe("Genau derselbe Name", sh_match("bericht.txt", "bericht.txt"));
    pruefe("Gross und klein ist gleich",
           sh_match("BERICHT.TXT", "bericht.txt"));
    pruefe("Ein anderer Name nicht", !sh_match("bericht.txt", "brief.txt"));

    pruefe("Stern am Anfang",  sh_match("*.txt", "bericht.txt"));
    pruefe("Stern am Ende",    sh_match("bericht*", "bericht.txt"));
    pruefe("Stern in der Mitte", sh_match("be*.txt", "bericht.txt"));
    pruefe("Nur ein Stern",    sh_match("*", "irgendwas"));
    pruefe("Stern frisst auch nichts", sh_match("bericht*", "bericht"));
    pruefe("Zwei Sterne",      sh_match("*ric*", "bericht.txt"));
    pruefe("Drei hintereinander", sh_match("***.txt", "bericht.txt"));

    pruefe("Falsche Endung",   !sh_match("*.txt", "bericht.png"));
    pruefe("Stern rettet nicht alles", !sh_match("*.txt", "txt.bericht"));

    pruefe("Fragezeichen",     sh_match("b?richt.txt", "bericht.txt"));
    pruefe("Genau eines",      !sh_match("b?richt.txt", "bbericht.txt"));
    pruefe("Vier Fragezeichen", sh_match("????", "abcd"));
    pruefe("Eines zu viel",     !sh_match("?????", "abcd"));

    /* Der Klassiker: Der Stern muss zuruecksetzen koennen, wenn es
     * hinter ihm doch nicht passt. */
    pruefe("Zuruecksetzen nach dem Stern", sh_match("*b.txt", "ab.txt"));
    pruefe("Und mehrfach",  sh_match("*ab*cd", "xxabxxabxxcd"));
    pruefe("Und richtig nein", !sh_match("*ab*cd", "xxabxxxx"));

    pruefe("Leeres Muster passt auf leeren Namen", sh_match("", ""));
    pruefe("Aber nicht auf einen Namen",  !sh_match("", "a"));
    pruefe("Ein Stern passt auf nichts",  sh_match("*", ""));
    pruefe("Ohne Zeiger nichts",          !sh_match(NULL, "a"));
    pruefe("Und andersherum auch nicht",  !sh_match("*", NULL));
}

static void test_rechnen(void)
{
    printf("Ausdruecke\n");

    pruefe_zahl("0", 0);
    pruefe_zahl("42", 42);
    pruefe_zahl("1+2", 3);
    pruefe_zahl("10-3", 7);
    pruefe_zahl("6*7", 42);
    pruefe_zahl("84/2", 42);
    pruefe_zahl("85%43", 42);

    /* Punkt vor Strich - der Grund, warum es einen Absteiger braucht
     * und keine Schleife von links nach rechts. */
    pruefe_zahl("2+3*4", 14);
    pruefe_zahl("3*4+2", 14);
    pruefe_zahl("2+3*4-6/2", 11);
    pruefe_zahl("(2+3)*4", 20);
    pruefe_zahl("2*(3+4)*2", 28);
    pruefe_zahl("((((5))))", 5);

    /* Links vor rechts, wo der Vorrang gleich ist. */
    pruefe_zahl("10-3-2", 5);
    pruefe_zahl("100/10/2", 5);

    pruefe_zahl("-5", -5);
    pruefe_zahl("-5+8", 3);
    pruefe_zahl("--5", 5);
    pruefe_zahl("3*-2", -6);
    pruefe_zahl("(0-7)*2", -14);

    pruefe_zahl("  7  +  3  ", 10);
    pruefe_zahl("1024*1024", 1048576);
    pruefe_zahl("2147483647+1", 2147483648LL);

    pruefe_fehler("");
    pruefe_fehler("+");
    pruefe_fehler("3+");
    pruefe_fehler("3 4");
    pruefe_fehler("(3+4");
    pruefe_fehler("3+)4(");
    pruefe_fehler("hallo");
    pruefe_fehler("3+hallo");

    /* Durch null geht es nicht - und es soll auffallen, nicht still
     * eine Null ergeben. */
    pruefe_fehler("3/0");
    pruefe_fehler("3%0");
    pruefe_fehler("3/(2-2)");

    /* Eine Zahl, die nicht mehr hineinpasst, faellt auf, bevor sie
     * ueberlaeuft. */
    pruefe_fehler("99999999999999999999999");
}

static void test_kalender(void)
{
    printf("Kalender\n");

    pruefe("2024 war ein Schaltjahr",  sh_leap_year(2024));
    pruefe("2025 nicht",               !sh_leap_year(2025));
    pruefe("1900 nicht",               !sh_leap_year(1900));
    pruefe("2000 schon",               sh_leap_year(2000));

    pruefe("Januar hat 31",  sh_days_in_month(2025, 1) == 31);
    pruefe("Februar 28",     sh_days_in_month(2025, 2) == 28);
    pruefe("Im Schaltjahr 29", sh_days_in_month(2024, 2) == 29);
    pruefe("April hat 30",   sh_days_in_month(2025, 4) == 30);
    pruefe("Dezember 31",    sh_days_in_month(2025, 12) == 31);
    pruefe("Den 13. Monat nicht", sh_days_in_month(2025, 13) == 0);

    pruefe_text("Name zum Monat", "Maerz", sh_month_name(3));
    pruefe_text("Und keiner zu 0", "", sh_month_name(0));

    /* Bekannte Daten. 0 ist Montag. */
    pruefe("Der 1.1.2024 war ein Montag",   sh_weekday(2024, 1, 1) == 0);
    pruefe("Der 29.2.2024 ein Donnerstag",  sh_weekday(2024, 2, 29) == 3);
    pruefe("Der 1.9.2026 ein Dienstag",     sh_weekday(2026, 9, 1) == 1);
    pruefe("Der 25.12.2025 ein Donnerstag", sh_weekday(2025, 12, 25) == 3);
    pruefe("Der 1.1.2000 ein Samstag",      sh_weekday(2000, 1, 1) == 5);
    pruefe("Der 4.7.1976 ein Sonntag",      sh_weekday(1976, 7, 4) == 6);

    /* Der ganze Monat: Kopf, Wochentage, Einrueckung, alle Tage. */
    char text[512];
    size_t n = sh_calendar(2026, 2, 0, text, sizeof(text));

    pruefe("Es kam etwas heraus", n > 0 && strlen(text) == n);
    pruefe("Mit dem Monatsnamen", strstr(text, "Februar 2026") != NULL);
    pruefe("Und den Wochentagen", strstr(text, " Mo  Di  Mi") != NULL);
    pruefe("Der 28. steht drin",  strstr(text, " 28 ") != NULL);
    pruefe("Der 29. nicht",       strstr(text, " 29 ") == NULL);

    /* Der 1.2.2026 ist ein Sonntag - also sechs leere Spalten davor,
     * jede vier Zeichen breit. */
    const char *erste = strstr(text, "So\n");

    pruefe("Die Wochenzeile ist da", erste != NULL);
    /* Sechs leere Spalten zu je vier Zeichen, dann die 1 in ihrer
     * eigenen vier Zeichen breiten Spalte. */
    pruefe("Sechs Spalten Einrueckung",
           erste && strncmp(erste + 3,
                            "                          1 ", 28) == 0);

    /* Der hervorgehobene Tag steht in Klammern und bleibt vier Zeichen
     * breit - sonst verrutschte die ganze Woche. */
    sh_calendar(2026, 2, 15, text, sizeof(text));
    pruefe("Der Tag ist hervorgehoben", strstr(text, "[15]") != NULL);
    pruefe("Und die anderen nicht",     strstr(text, "[16]") == NULL);

    pruefe("Ein Monat, den es nicht gibt",
           sh_calendar(2026, 13, 0, text, sizeof(text)) == 0);
}

static void test_befehle(void)
{
    printf("Die Befehlstabelle\n");

    pruefe("Es gibt Befehle",  sh_command_count() > 40);
    pruefe("Und sechs Bereiche", sh_group_count() == 6);

    const struct sh_command *ls = sh_command_find("ls");

    pruefe("ls ist da",            ls != NULL);
    pruefe_text("Mit Zweitnamen",  "dir", ls->alias);
    pruefe("Und einem Bereich",    ls->group[0] != '\0');
    pruefe("Und einer Zeile dazu", ls->what[0] != '\0');

    pruefe("Ueber den Zweitnamen findet man ihn auch",
           sh_command_find("dir") == ls);
    pruefe("Gross geschrieben auch", sh_command_find("LS") == ls);
    pruefe("Unsinn nicht",           sh_command_find("zauberei") == NULL);
    pruefe("Leer auch nicht",        sh_command_find("") == NULL);
    pruefe("Und ohne Zeiger",        sh_command_find(NULL) == NULL);

    /* Jeder Eintrag muss vollstaendig sein, und jeder Bereich muss es
     * geben - sonst faellt ein Befehl aus der Hilfe heraus, ohne dass
     * es jemand merkt. */
    for (size_t i = 0; i < sh_command_count(); i++) {
        const struct sh_command *c = sh_command_at(i);
        bool group_ok = false;

        if (!c->name || !c->name[0] || !c->usage || !c->usage[0] ||
            !c->what || !c->what[0]) {
            printf("  FEHLER: Eintrag %u ist unvollstaendig\n", (unsigned)i);
            fehler++;
            break;
        }

        for (size_t g = 0; g < sh_group_count(); g++)
            if (strcmp(sh_group_at(g), c->group) == 0)
                group_ok = true;

        if (!group_ok) {
            printf("  FEHLER: \"%s\" steht im Bereich \"%s\", den es nicht "
                   "gibt\n", c->name, c->group);
            fehler++;
            break;
        }

        /* Die Gebrauchszeile muss mit dem Namen anfangen - sonst sucht
         * man in der Hilfe nach etwas, das anders heisst. */
        if (strncmp(c->usage, c->name, strlen(c->name)) != 0) {
            printf("  FEHLER: \"%s\" hat die Gebrauchszeile \"%s\"\n",
                   c->name, c->usage);
            fehler++;
            break;
        }
    }
    geprueft += 3;

    /* Kein Name darf zweimal vorkommen - auch nicht als Zweitname. */
    for (size_t i = 0; i < sh_command_count(); i++) {
        const struct sh_command *a = sh_command_at(i);

        for (size_t k = i + 1; k < sh_command_count(); k++) {
            const struct sh_command *b = sh_command_at(k);

            if (strcmp(a->name, b->name) == 0 ||
                (a->alias && b->alias && strcmp(a->alias, b->alias) == 0) ||
                (b->alias && strcmp(a->name, b->alias) == 0) ||
                (a->alias && strcmp(a->alias, b->name) == 0)) {
                printf("  FEHLER: \"%s\" und \"%s\" teilen sich einen Namen\n",
                       a->name, b->name);
                fehler++;
            }
        }
    }
    geprueft++;

    pruefe("Ueber das Ende hinaus nichts", sh_command_at(9999) == NULL);
    pruefe_text("Und kein Bereich", "", sh_group_at(9999));
}

int main(void)
{
    printf("=== Konsole ===\n");

    test_muster();
    test_rechnen();
    test_kalender();
    test_befehle();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
