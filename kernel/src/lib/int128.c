/* int128.c - Teilen von 128-Bit-Zahlen.
 *
 * Der Uebersetzer erzeugt fuer __int128 Aufrufe an diese Hilfsfunktionen.
 * Auf einem gewoehnlichen System kaemen sie aus libgcc; ein Kern bringt
 * sie selbst mit. Geteilt wird nach dem Schulverfahren mit Schieben und
 * Abziehen - 128 Durchgaenge, dafuer ohne jede Tabelle.
 */

#include "retro.h"

typedef unsigned __int128 uint128_t;
typedef signed __int128   int128_t;

uint128_t __udivmodti4(uint128_t zaehler, uint128_t nenner,
                       uint128_t *rest);

uint128_t __udivmodti4(uint128_t zaehler, uint128_t nenner, uint128_t *rest)
{
    if (nenner == 0) {                  /* Teilen durch null: alles Einsen */
        if (rest)
            *rest = 0;
        return ~(uint128_t)0;
    }
    if (nenner > zaehler) {
        if (rest)
            *rest = zaehler;
        return 0;
    }

    /* Solange der Nenner passt, wird er nach links geschoben. */
    int32_t schritte = 0;

    while (nenner <= zaehler && !(nenner >> 127)) {
        nenner <<= 1;
        schritte++;
    }
    if (nenner > zaehler) {
        nenner >>= 1;
        schritte--;
    }

    uint128_t ergebnis = 0;

    for (int32_t i = schritte; i >= 0; i--) {
        ergebnis <<= 1;
        if (zaehler >= nenner) {
            zaehler -= nenner;
            ergebnis |= 1;
        }
        nenner >>= 1;
    }

    if (rest)
        *rest = zaehler;
    return ergebnis;
}

uint128_t __udivti3(uint128_t a, uint128_t b);
uint128_t __umodti3(uint128_t a, uint128_t b);
int128_t  __divti3(int128_t a, int128_t b);
int128_t  __modti3(int128_t a, int128_t b);

uint128_t __udivti3(uint128_t a, uint128_t b)
{
    return __udivmodti4(a, b, NULL);
}

uint128_t __umodti3(uint128_t a, uint128_t b)
{
    uint128_t rest;

    __udivmodti4(a, b, &rest);
    return rest;
}

int128_t __divti3(int128_t a, int128_t b)
{
    bool negativ = (a < 0) != (b < 0);
    uint128_t betrag_a = a < 0 ? (uint128_t)(-a) : (uint128_t)a;
    uint128_t betrag_b = b < 0 ? (uint128_t)(-b) : (uint128_t)b;
    uint128_t ergebnis = __udivmodti4(betrag_a, betrag_b, NULL);

    return negativ ? -(int128_t)ergebnis : (int128_t)ergebnis;
}

int128_t __modti3(int128_t a, int128_t b)
{
    uint128_t betrag_a = a < 0 ? (uint128_t)(-a) : (uint128_t)a;
    uint128_t betrag_b = b < 0 ? (uint128_t)(-b) : (uint128_t)b;
    uint128_t rest;

    __udivmodti4(betrag_a, betrag_b, &rest);
    return a < 0 ? -(int128_t)rest : (int128_t)rest;
}
