/* rechner_test.c - prueft das Rechenwerk des Taschenrechners.
 *
 * Ein Taschenrechner ist ein Zustandsautomat, und die Fehler stecken
 * in den Uebergaengen: was passiert, wenn nach dem Gleichheitszeichen
 * eine Ziffer kommt, wenn zweimal hintereinander ein Rechenzeichen
 * gedrueckt wird, wenn das Komma vor der ersten Ziffer steht. Dazu das
 * Festkomma selbst - Runden, Ueberlauf und das Teilen durch null.
 */

#include <stdio.h>
#include <string.h>

#include "calc.h"

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

/* Tippt eine Folge von Tasten und vergleicht die Anzeige. */
static void tippe(const char *tasten, const char *erwartet)
{
    struct calc c;
    char text[48];

    calc_reset(&c);

    for (const char *p = tasten; *p; p++) {
        if (*p >= '0' && *p <= '9')
            calc_digit(&c, *p - '0');
        else if (*p == ',' || *p == '.')
            calc_point(&c);
        else if (*p == '+' || *p == '-' || *p == '*' || *p == '/')
            calc_op(&c, *p);
        else if (*p == '=')
            calc_equals(&c);
        else if (*p == 'S')
            calc_sign(&c);
        else if (*p == 'B')
            calc_backspace(&c);
        else if (*p == '%')
            calc_percent(&c);
        else if (*p == 'W')
            calc_sqrt(&c);
        else if (*p == 'C')
            calc_reset(&c);
        else if (*p == 'E')
            calc_clear_entry(&c);
    }

    calc_display(&c, text, sizeof(text));
    geprueft++;
    if (strcmp(text, erwartet) != 0) {
        printf("  FEHLER: \"%s\" -> \"%s\" statt \"%s\"\n",
               tasten, text, erwartet);
        fehler++;
    }
}

static void test_grundrechnen(void)
{
    printf("Grundrechnen\n");

    tippe("2+3=", "5");
    tippe("9-4=", "5");
    tippe("6*7=", "42");
    tippe("84/2=", "42");
    tippe("0=", "0");

    /* Die Kette: Nach jedem Zeichen steht ein Zwischenergebnis da. */
    tippe("2+3+4=", "9");
    tippe("2+3+", "5");
    tippe("10-1-1-1=", "7");

    /* Ein Taschenrechner kennt keinen Vorrang - anders als "rechne"
     * in der Konsole, und das ist Absicht. */
    tippe("2+3*4=", "20");

    /* Grosse Zahlen bleiben genau, solange sie in die Anzeige passen. */
    tippe("999999999*999=", "998999999001");
}

static void test_komma(void)
{
    printf("Komma\n");

    tippe("1,5+2,5=", "4");
    tippe("0,1+0,2=", "0,3");        /* genau, weil Festkomma        */
    tippe("1,25*4=", "5");
    tippe("10/4=", "2,5");
    tippe("1/8=", "0,125");

    /* Das Komma vor der ersten Ziffer ist eine Null davor. */
    tippe(",5+,5=", "1");

    /* Ein getipptes Komma ohne Ziffern soll man sehen. */
    tippe("7,", "7,");
    tippe("7,0", "7");

    /* Mehr als sechs Nachkommastellen gibt es nicht - die siebte
     * wird verschluckt und veraendert nichts. */
    tippe("0,1234567", "0,123456");

    /* Gerundet wird kaufmaennisch und nicht abgeschnitten. */
    tippe("2/3=", "0,666667");
    tippe("1/3=", "0,333333");
}

static void test_zustand(void)
{
    printf("Uebergaenge");
    printf("\n");

    /* Nach dem Gleichheitszeichen faengt eine Ziffer neu an. */
    tippe("2+3=7", "7");
    tippe("2+3=7+1=", "8");

    /* Zwei Rechenzeichen hintereinander: das letzte gilt. */
    tippe("6*/2=", "3");

    /* Gleichheitszeichen ohne alles. */
    tippe("=", "0");
    tippe("5=", "5");

    /* Vorzeichen wirkt auf die Eingabe wie auf das Ergebnis. */
    tippe("5S", "-5");
    tippe("5S+3=", "-2");
    tippe("2+3=S", "-5");

    /* Ruecktaste: Ziffern und Nachkommastellen. */
    tippe("123B", "12");
    tippe("1,25B", "1,2");
    tippe("1,2BB", "1");        /* erst die Ziffer, dann das Komma */
    tippe("7BB", "0");

    /* Loeschen: alles gegen nur die Eingabe. */
    tippe("2+3C", "0");
    tippe("2+3E4=", "6");
}

static void test_grenzen(void)
{
    printf("Grenzen\n");

    /* Teilen durch null meldet und rechnet nicht weiter. */
    tippe("5/0=", "Nicht durch null");
    tippe("5/0=3", "Nicht durch null");

    /* Was zu gross wird, meldet sich ebenfalls. */
    tippe("999999999999*999999=", "Zu gross");

    /* Und eine zu lange Eingabe wird gar nicht erst angenommen -
     * die dreizehnte Ziffer bleibt weg. */
    tippe("9999999999999", "999999999999");

    /* Wurzel aus einer negativen Zahl ist keine. */
    tippe("9SW", "Keine Wurzel");

    struct calc c;
    char text[48];

    /* Nach einem Fehler nimmt der Rechner nichts mehr an, bis
     * geloescht wird - alles andere waere geraten. */
    calc_reset(&c);
    calc_digit(&c, 5);
    calc_op(&c, '/');
    calc_digit(&c, 0);
    calc_equals(&c);
    calc_digit(&c, 7);
    calc_display(&c, text, sizeof(text));
    pruefe("Nach einem Fehler bleibt es dabei",
           strcmp(text, "Nicht durch null") == 0);

    calc_reset(&c);
    calc_display(&c, text, sizeof(text));
    pruefe("Loeschen raeumt den Fehler weg", strcmp(text, "0") == 0);
}

static void test_prozent_und_wurzel(void)
{
    printf("Prozent und Wurzel\n");

    /* Bei Strich ist ein Prozent ein Hundertstel dessen, was
     * davorsteht. */
    tippe("200+10%=", "220");
    tippe("200-10%=", "180");

    /* Bei Punkt schlicht ein Hundertstel. */
    tippe("200*10%=", "20");

    tippe("9W", "3");
    tippe("2W", "1,414214");
    tippe("0W", "0");
    tippe("0,25W", "0,5");
    tippe("1000000W", "1000");
}

static void test_format(void)
{
    printf("Anzeige\n");

    char text[48];

    calc_format(0, text, sizeof(text));
    pruefe("Null", strcmp(text, "0") == 0);
    calc_format(CALC_SCALE, text, sizeof(text));
    pruefe("Eins ohne Nachkomma", strcmp(text, "1") == 0);
    calc_format(-CALC_SCALE * 3 / 2, text, sizeof(text));
    pruefe("Negativ mit Komma", strcmp(text, "-1,5") == 0);
    calc_format(CALC_SCALE / 2, text, sizeof(text));
    pruefe("Ein halbes", strcmp(text, "0,5") == 0);
    calc_format(1, text, sizeof(text));
    pruefe("Die kleinste Stufe", strcmp(text, "0,000001") == 0);
    calc_format(CALC_MAX * CALC_SCALE, text, sizeof(text));
    pruefe("Das Groesste", strcmp(text, "999999999999") == 0);
}

int main(void)
{
    printf("=== Rechner ===\n");

    test_grundrechnen();
    test_komma();
    test_zustand();
    test_grenzen();
    test_prozent_und_wurzel();
    test_format();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
