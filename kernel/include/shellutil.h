/* shellutil.h - die rechnenden Teile der Konsole.
 *
 * Ein paar Befehle brauchen mehr als eine Schleife ueber den
 * Dateibaum: "finde" vergleicht Namen mit einem Muster, "rechne" wertet
 * einen Ausdruck aus, "kalender" muss wissen, auf welchen Wochentag ein
 * Monat faellt. Das sind geschlossene Rechnungen ohne Geraete und ohne
 * Dateien - und genau deshalb stehen sie hier und nicht mitten in
 * terminal.c: So lassen sie sich auf dem Entwicklungsrechner pruefen.
 *
 * Dazu kommt die Tabelle aller Befehle. Sie ist die einzige Stelle, an
 * der ein Befehl beschrieben wird; "hilfe" und "man" lesen beide daraus.
 * Zwei Listen, die auseinanderlaufen, sind schlimmer als eine, die
 * knapp ist.
 */
#ifndef SHELLUTIL_H
#define SHELLUTIL_H

#include "retro.h"

/* --- Namensmuster --------------------------------------------------
 *
 * "*" steht fuer beliebig viele Zeichen, "?" fuer genau eines. Gross-
 * und Kleinschreibung sind gleich, denn das ist auch im uebrigen
 * Dateibaum so. */
bool sh_match(const char *pattern, const char *text);

/* --- Ausdruecke -----------------------------------------------------
 *
 * Ganze Zahlen mit + - * / % und Klammern, Punkt vor Strich. Kein
 * Fliesskomma: Der Kernel hat keine Fliesskommaeinheit, und eine
 * Nachbildung waere fuer einen Taschenrechner in der Konsole mehr
 * Aufwand als Nutzen. Bei einem Fehler steht der Grund in error. */
bool sh_eval(const char *text, int64_t *out, char *error, size_t error_size);

/* --- Kalender -------------------------------------------------------
 *
 * Wochentag nach der Formel von Zeller: 0 ist Montag, 6 ist Sonntag -
 * die deutsche Zaehlung, weil der Kalender so gedruckt wird. */
int     sh_weekday(uint16_t year, uint8_t month, uint8_t day);
bool    sh_leap_year(uint16_t year);
uint8_t sh_days_in_month(uint16_t year, uint8_t month);
const char *sh_month_name(uint8_t month);

/* Schreibt einen Monatskalender, Zeilen durch \n getrennt. Der Tag
 * highlight wird in eckige Klammern gesetzt; 0 hebt nichts hervor. */
size_t sh_calendar(uint16_t year, uint8_t month, uint8_t highlight,
                   char *out, size_t size);

/* --- Die Befehlstabelle --------------------------------------------- */

struct sh_command {
    const char *name;
    const char *alias;       /* zweiter Name, oder NULL          */
    const char *group;       /* fuer die Gliederung in "hilfe"   */
    const char *usage;       /* "kopf [-n] <datei>"              */
    const char *what;        /* eine Zeile                       */
    const char *detail;      /* mehr dazu, Zeilen durch \n, oder NULL */
};

size_t                   sh_command_count(void);
const struct sh_command *sh_command_at(size_t index);
/* Findet einen Befehl ueber Haupt- oder Zweitnamen. */
const struct sh_command *sh_command_find(const char *name);

size_t      sh_group_count(void);
const char *sh_group_at(size_t index);

#endif /* SHELLUTIL_H */
