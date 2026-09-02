/* kaefig_test.c - prueft den Kaefig um ein Programm.
 *
 * Zwei Dinge muessen hier stimmen, sonst ist der Kaefig eine Bitte.
 *
 * Erstens die Einbahnstrasse: Ein Profil darf nur enger machen. Koennte
 * ein eingesperrtes Programm "offen" waehlen, waere die ganze
 * Einrichtung wertlos - und genau das ist der Fehler, den man beim
 * Lesen des Codes uebersieht.
 *
 * Zweitens die Pfadgrenze. Sie ist eine Textrechnung, und Textrechnungen
 * ueber Pfade gehen an derselben Stelle schief: bei "..", bei doppelten
 * Schraegstrichen und bei Namen, die mit der Wurzel anfangen, ohne
 * darunter zu liegen. Alle drei stehen hier.
 */

#include <stdio.h>
#include <string.h>

#include "sandbox.h"
#include "syscall.h"

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

static struct sandbox leer(void)
{
    struct sandbox box;

    memset(&box, 0, sizeof(box));
    return box;
}

/* ------------------------------------------------------------------ */

static void test_profile(void)
{
    printf("Profile\n");

    pruefe("Vier Profile", sandbox_profile_count() == 4);
    pruefe_text("Das erste heisst offen", "offen", sandbox_profile_name(0));
    pruefe_text("Ueber das Ende hinaus nichts", "",
                sandbox_profile_name(99));

    /* Ohne Kaefig ist alles erlaubt - auch die Abfrage selbst. */
    struct sandbox box = leer();

    pruefe("Ohne Kaefig Netz",     sandbox_allows(&box, SB_NET));
    pruefe("Ohne Kaefig Schreiben", sandbox_allows(&box, SB_FILE_WRITE));
    pruefe("Und ohne Zeiger auch", sandbox_allows(NULL, SB_NET));

    /* "offen" ist kein Kaefig, sondern sein Fehlen. */
    pruefe("offen wird angenommen", sandbox_apply(&box, "offen", NULL));
    pruefe("Und sperrt nichts ein", !box.active);

    box = leer();
    pruefe("netz wird angenommen", sandbox_apply(&box, "netz", NULL));
    pruefe("Es ist ein Kaefig",    box.active);
    pruefe("Netz erlaubt",         sandbox_allows(&box, SB_NET));
    pruefe("Lesen erlaubt",        sandbox_allows(&box, SB_FILE_READ));
    pruefe("Schreiben nicht",      !sandbox_allows(&box, SB_FILE_WRITE));
    pruefe("Abspalten nicht",      !sandbox_allows(&box, SB_PROC));
    pruefe("Eine Speichergrenze",  box.max_pages > 0);
    pruefe("Ein Verstoss ist ein Fehler", box.penalty == SB_DENY);

    box = leer();
    pruefe("heim wird angenommen", sandbox_apply(&box, "heim",
                                                 "/Festplatte/Benutzer/anna"));
    pruefe("Schreiben erlaubt",    sandbox_allows(&box, SB_FILE_WRITE));
    pruefe("Netz nicht",           !sandbox_allows(&box, SB_NET));
    pruefe_text("Mit der Wurzel des Heims", "/Festplatte/Benutzer/anna",
                box.root);

    box = leer();
    pruefe("streng wird angenommen", sandbox_apply(&box, "streng", NULL));
    pruefe("Rechnen erlaubt",     sandbox_allows(&box, SB_CORE));
    pruefe("Ausgeben erlaubt",    sandbox_allows(&box, SB_STDIO));
    pruefe("Roehren erlaubt",     sandbox_allows(&box, SB_IPC));
    pruefe("Lesen nicht",         !sandbox_allows(&box, SB_FILE_READ));
    pruefe("Fenster nicht",       !sandbox_allows(&box, SB_WIN));
    pruefe("Ein Verstoss beendet", box.penalty == SB_KILL);

    box = leer();
    pruefe("Ein Profil, das es nicht gibt",
           !sandbox_apply(&box, "gemuetlich", NULL));
    pruefe("Und es bleibt offen", !box.active);
    pruefe("Ohne Namen auch nicht", !sandbox_apply(&box, NULL, NULL));
}

static void test_einbahnstrasse(void)
{
    printf("Nur enger, nie weiter\n");

    struct sandbox box = leer();

    sandbox_apply(&box, "streng", NULL);
    pruefe("Streng eingesperrt", box.active && !sandbox_allows(&box, SB_NET));

    /* Der entscheidende Fall: Wer drin sitzt, kommt nicht heraus. */
    pruefe("offen wird abgelehnt", !sandbox_apply(&box, "offen", NULL));
    pruefe("Und es bleibt streng", !sandbox_allows(&box, SB_NET));

    pruefe("netz wird abgelehnt",  !sandbox_apply(&box, "netz", NULL));
    pruefe("Netz bleibt zu",       !sandbox_allows(&box, SB_NET));
    pruefe("Lesen bleibt zu",      !sandbox_allows(&box, SB_FILE_READ));

    /* Dasselbe Profil noch einmal ist kein Aufweichen. */
    pruefe("streng noch einmal",   sandbox_apply(&box, "streng", NULL));

    /* Von netz nach streng geht - das ist enger. */
    box = leer();
    sandbox_apply(&box, "netz", NULL);
    pruefe("Von netz nach streng", sandbox_apply(&box, "streng", NULL));
    pruefe("Danach kein Netz",     !sandbox_allows(&box, SB_NET));
    pruefe("Und kein Lesen",       !sandbox_allows(&box, SB_FILE_READ));
    pruefe("Der Verstoss beendet jetzt", box.penalty == SB_KILL);

    /* Die Speichergrenze wird ebenfalls nur kleiner. */
    box = leer();
    sandbox_apply(&box, "streng", NULL);

    uint32_t eng = box.max_pages;

    sandbox_apply(&box, "streng", NULL);
    pruefe("Die Grenze bleibt eng", box.max_pages == eng);
}

static void test_pfade(void)
{
    printf("Die Pfadgrenze\n");

    struct sandbox box = leer();

    /* Ohne Kaefig gilt jeder Pfad. */
    pruefe("Ohne Kaefig alles", sandbox_path_ok(&box, "/System/geheim"));

    sandbox_apply(&box, "heim", "/Benutzer/anna");

    pruefe("Das Heim selbst",  sandbox_path_ok(&box, "/Benutzer/anna"));
    pruefe("Etwas darin",      sandbox_path_ok(&box, "/Benutzer/anna/brief.txt"));
    pruefe("Tiefer darin",
           sandbox_path_ok(&box, "/Benutzer/anna/Bilder/urlaub.png"));

    pruefe("Daneben nicht",    !sandbox_path_ok(&box, "/Benutzer/jesse"));
    pruefe("Darueber nicht",   !sandbox_path_ok(&box, "/Benutzer"));
    pruefe("Ganz woanders nicht", !sandbox_path_ok(&box, "/System/version.txt"));

    /* Der Klassiker: ein Name, der mit der Wurzel anfaengt, ohne
     * darunter zu liegen. */
    pruefe("annalise gehoert nicht dazu",
           !sandbox_path_ok(&box, "/Benutzer/annalise/brief.txt"));
    pruefe("annax auch nicht",
           !sandbox_path_ok(&box, "/Benutzer/annax"));

    /* Der zweite Klassiker: der Weg zurueck ueber zwei Punkte. */
    pruefe("Zwei Punkte fuehren nicht hinaus",
           !sandbox_path_ok(&box, "/Benutzer/anna/../jesse/brief.txt"));
    pruefe("Auch nicht mehrfach",
           !sandbox_path_ok(&box, "/Benutzer/anna/a/b/../../../jesse"));
    pruefe("Und nicht bis zur Wurzel",
           !sandbox_path_ok(&box, "/Benutzer/anna/../.."));

    /* Zwei Punkte, die wieder zurueckfuehren, sind dagegen in Ordnung. */
    pruefe("Hin und zurueck bleibt drin",
           sandbox_path_ok(&box, "/Benutzer/anna/Bilder/../brief.txt"));

    /* Doppelte Schraegstriche und Punkte aendern nichts. */
    pruefe("Doppelte Schraegstriche",
           sandbox_path_ok(&box, "/Benutzer//anna///brief.txt"));
    pruefe("Ein Punkt dazwischen",
           sandbox_path_ok(&box, "/Benutzer/./anna/./brief.txt"));
    pruefe("Ein Schraegstrich am Ende",
           sandbox_path_ok(&box, "/Benutzer/anna/"));

    /* Wer gar keine Dateien sehen darf, hat auch keinen erlaubten Pfad -
     * sonst waere "streng" an dieser Stelle offen. */
    box = leer();
    sandbox_apply(&box, "streng", NULL);
    pruefe("streng kennt keinen Pfad", !sandbox_path_ok(&box, "/Temp/x"));
    pruefe("Auch nicht die Wurzel",    !sandbox_path_ok(&box, "/"));

    /* Ein Kaefig ohne Wurzel beschraenkt den Baum nicht. */
    box = leer();
    sandbox_apply(&box, "netz", NULL);
    pruefe("netz sieht den ganzen Baum",
           sandbox_path_ok(&box, "/System/version.txt"));
}

static void test_gruppen(void)
{
    printf("Systemaufrufe und Gruppen\n");

    pruefe("Beenden ist Grundlage", sandbox_group_of(SYS_EXIT) == SB_CORE);
    pruefe("Schlafen auch",         sandbox_group_of(SYS_SLEEP) == SB_CORE);
    pruefe("Oeffnen ist Lesen",     sandbox_group_of(SYS_OPEN) == SB_FILE_READ);
    pruefe("Auflisten auch",
           sandbox_group_of(SYS_READDIR) == SB_FILE_READ);
    pruefe("Verbinden ist Netz",    sandbox_group_of(SYS_CONNECT) == SB_NET);
    pruefe("Zuhoeren auch",         sandbox_group_of(SYS_LISTEN) == SB_NET);
    pruefe("Fenster ist Fenster",   sandbox_group_of(SYS_WIN_OPEN) == SB_WIN);
    pruefe("Abspalten ist Abspalten",
           sandbox_group_of(SYS_FORK) == SB_PROC);
    pruefe("Roehre ist Roehre",     sandbox_group_of(SYS_PIPE) == SB_IPC);

    /* Lesen und Schreiben haengen an der Nummer, nicht am Aufruf -
     * dieselbe Nummer bedient Konsole, Datei und Roehre. Deshalb
     * entscheidet der Aufruf selbst und die Gruppe ist hier null. */
    pruefe("Lesen hat keine Gruppe",   sandbox_group_of(SYS_READ) == 0);
    pruefe("Schreiben auch nicht",     sandbox_group_of(SYS_WRITE) == 0);
    pruefe("Und null ist immer erlaubt", sandbox_allows(NULL, 0));

    pruefe_text("Name zum Netz", "Netz", sandbox_group_name(SB_NET));
    pruefe_text("Name zum Lesen", "Dateien lesen",
                sandbox_group_name(SB_FILE_READ));

    /* Die Beschreibung soll man vorlesen koennen. */
    struct sandbox box = leer();
    char text[128];

    sandbox_text(&box, text, sizeof(text));
    pruefe_text("Ohne Kaefig", "kein Kaefig", text);

    sandbox_apply(&box, "streng", NULL);
    sandbox_text(&box, text, sizeof(text));
    pruefe_text("streng im Klartext",
                "Grundlagen, Ein-/Ausgabe, Roehren", text);

    box = leer();
    box.active = true;
    box.allow = 0;
    sandbox_text(&box, text, sizeof(text));
    pruefe_text("Ein Kaefig ohne alles", "nichts", text);
}

int main(void)
{
    printf("=== Kaefig ===\n");

    test_profile();
    test_einbahnstrasse();
    test_pfade();
    test_gruppen();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
