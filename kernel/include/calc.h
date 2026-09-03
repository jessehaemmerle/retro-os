/* calc.h - das Rechenwerk des Taschenrechners.
 *
 * Getrennt von seinem Fenster, weil sich hier alles pruefen laesst,
 * was schiefgehen kann: das Tippen von Nachkommastellen, das
 * Ueberlaufen, das Teilen durch null und die Reihenfolge, in der ein
 * Taschenrechner rechnet.
 *
 * Und die ist eine andere als die in der Konsole. "2 + 3 * 4 =" gibt
 * hier zwanzig und nicht vierzehn: Ein Taschenrechner rechnet, sobald
 * die naechste Taste kommt, und kennt keinen Vorrang. Das ist kein
 * Fehler, sondern was jeder von so einem Geraet erwartet - wer Punkt
 * vor Strich will, tippt "rechne" in die Konsole.
 *
 * Gerechnet wird in Festkomma mit sechs Nachkommastellen. Keine
 * Gleitkommazahlen: Der Kern schaltet die Recheneinheit dafuer gar
 * nicht erst ein, und fuer einen Taschenrechner ist Festkomma ohnehin
 * das ehrlichere Verfahren - 0,1 + 0,2 gibt hier genau 0,3.
 */
#ifndef CALC_H
#define CALC_H

#include "retro.h"

/* Sechs Nachkommastellen. */
#define CALC_SCALE   1000000LL
/* Darueber wird nicht mehr gerechnet, sondern gemeldet. Zwoelf
 * Vorkommastellen sind mehr, als in die Anzeige passen. */
#define CALC_MAX     999999999999LL

/* Warum es nicht weitergeht. Ein Rechner, der nur "Fehler" sagt,
 * laesst den Benutzer raten - und die drei Faelle sind gut zu
 * unterscheiden. */
enum calc_fault {
    CALC_OK,
    CALC_DIV0,     /* durch null geteilt          */
    CALC_RANGE,    /* zu gross fuer die Anzeige   */
    CALC_DOMAIN,   /* Wurzel aus etwas Negativem  */
};

struct calc {
    int64_t acc;        /* was schon dasteht                        */
    int64_t entry;      /* was gerade getippt wird                  */
    char    op;         /* 0, '+', '-', '*', '/'                    */
    bool    typing;     /* Ziffern gehen in entry statt in acc      */
    int     decimals;   /* wie viele Nachkommastellen schon getippt */
    bool    point;      /* das Komma ist getippt, Ziffern fehlen    */
    bool    error;
    uint8_t fault;      /* enum calc_fault */
};

void calc_reset(struct calc *c);
/* Nur die Eingabe loeschen, das Angefangene behalten. */
void calc_clear_entry(struct calc *c);

void calc_digit(struct calc *c, int digit);
void calc_point(struct calc *c);
void calc_backspace(struct calc *c);
void calc_sign(struct calc *c);

/* Ein Rechenzeichen. Steht schon eines an, wird erst das ausgerechnet -
 * so entsteht die Kette 2 + 3 + 4. */
void calc_op(struct calc *c, char op);
void calc_equals(struct calc *c);
/* Prozent: nimmt den angezeigten Wert als Hundertstel dessen, was
 * davorsteht. "200 + 10 %" ist damit 220. */
void calc_percent(struct calc *c);
void calc_sqrt(struct calc *c);

/* Was in der Anzeige steht. */
int64_t calc_value(const struct calc *c);
/* Schreibt einen Festkommawert als Text - ohne ueberfluessige Nullen
 * hinter dem Komma. */
void calc_format(int64_t value, char *out, size_t size);
/* Die Anzeige, samt Fehlermeldung. */
void calc_display(const struct calc *c, char *out, size_t size);

#endif /* CALC_H */
