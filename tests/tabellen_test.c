/* tabellen_test.c - prueft das Rechenwerk der Tabellenkalkulation.
 *
 * Der Auswerter laesst sich hier ohne Kernel pruefen: Er kennt nur
 * Zellen und Zahlen, keine Geraete. Geprueft wird das Lesen und
 * Schreiben von Zahlen, jeder Rechenschritt, die Funktionen, die
 * Fehlerfaelle und der Weg durch eine Datei und zurueck.
 */

#include <stdio.h>
#include <string.h>

#include "sheet.h"

static int fehler;
static int geprueft;

static struct sheet blatt;

static void setze(const char *ref, const char *inhalt)
{
    int row, col;

    if (!sheet_parse_ref(ref, &row, &col)) {
        printf("  FEHLER: Bezug \"%s\" nicht lesbar\n", ref);
        fehler++;
        return;
    }
    sheet_set(&blatt, row, col, inhalt);
}

static const char *anzeige(const char *ref)
{
    static char buffer[64];
    int row, col;

    sheet_parse_ref(ref, &row, &col);
    sheet_display(&blatt, row, col, buffer, sizeof(buffer));
    return buffer;
}

static void erwarte(const char *was, const char *ist, const char *soll)
{
    geprueft++;
    if (strcmp(ist, soll) == 0)
        return;
    printf("  FEHLER: %s ergibt \"%s\", erwartet \"%s\"\n", was, ist, soll);
    fehler++;
}

/* Eine Formel in A1, das Ergebnis abgelesen. */
static void rechne(const char *formel, const char *soll)
{
    sheet_clear(&blatt);
    setze("A1", formel);
    sheet_recalc(&blatt);
    erwarte(formel, anzeige("A1"), soll);
}

static void zahlen(void)
{
    printf("Zahlen lesen und schreiben\n");

    struct {
        const char *text;
        const char *soll;
    } proben[] = {
        { "0",        "0"       },
        { "42",       "42"      },
        { "-7",       "-7"      },
        { "3,5",      "3,5"     },
        { "3.5",      "3,5"     },
        { "0,25",     "0,25"    },
        { "1,2345",   "1,2345"  },
        { "-0,5",     "-0,5"    },
        { "1000000",  "1000000" },
        { " 12 ",     "12"      },
    };

    for (size_t i = 0; i < sizeof(proben) / sizeof(proben[0]); i++) {
        sheet_num value = 0;

        geprueft++;
        if (!sheet_parse_number(proben[i].text, &value)) {
            printf("  FEHLER: \"%s\" wurde nicht als Zahl erkannt\n",
                   proben[i].text);
            fehler++;
            continue;
        }

        char out[32];

        sheet_format_number(value, out, sizeof(out));
        if (strcmp(out, proben[i].soll) != 0) {
            printf("  FEHLER: \"%s\" -> \"%s\", erwartet \"%s\"\n",
                   proben[i].text, out, proben[i].soll);
            fehler++;
        }
    }

    /* Die fuenfte Stelle wird gerundet. */
    sheet_num value = 0;
    char out[32];

    sheet_parse_number("1,23456", &value);
    sheet_format_number(value, out, sizeof(out));
    erwarte("1,23456 gerundet", out, "1,2346");

    static const char *keine[] = { "", "abc", "1,2,3", "12x", "-", "1 2" };

    for (size_t i = 0; i < sizeof(keine) / sizeof(keine[0]); i++) {
        geprueft++;
        if (sheet_parse_number(keine[i], NULL)) {
            printf("  FEHLER: \"%s\" gilt faelschlich als Zahl\n", keine[i]);
            fehler++;
        }
    }
}

static void bezuege(void)
{
    printf("Bezuege\n");

    int row, col;

    geprueft++;
    if (!sheet_parse_ref("A1", &row, &col) || row != 0 || col != 0) {
        printf("  FEHLER: A1 falsch\n");
        fehler++;
    }

    geprueft++;
    if (!sheet_parse_ref("$C$12", &row, &col) || row != 11 || col != 2) {
        printf("  FEHLER: $C$12 falsch\n");
        fehler++;
    }

    geprueft++;
    if (!sheet_parse_ref("z100", &row, &col) || row != 99 || col != 25) {
        printf("  FEHLER: z100 falsch\n");
        fehler++;
    }

    static const char *keine[] = { "A0", "A101", "AA1", "1A", "A", "A1B" };

    for (size_t i = 0; i < sizeof(keine) / sizeof(keine[0]); i++) {
        geprueft++;
        if (sheet_parse_ref(keine[i], &row, &col)) {
            printf("  FEHLER: \"%s\" gilt faelschlich als Bezug\n", keine[i]);
            fehler++;
        }
    }

    char name[8];

    sheet_ref_name(0, 0, name, sizeof(name));
    erwarte("Name von (0,0)", name, "A1");
    sheet_ref_name(41, 25, name, sizeof(name));
    erwarte("Name von (41,25)", name, "Z42");
}

static void rechnen(void)
{
    printf("Rechnen\n");

    rechne("=1+2",            "3");
    rechne("=10-4",           "6");
    rechne("=6*7",            "42");
    rechne("=10/4",           "2,5");
    rechne("=1/3",            "0,3333");
    rechne("=2+3*4",          "14");
    rechne("=(2+3)*4",        "20");
    rechne("=-5+2",           "-3");
    rechne("=2^10",           "1024");
    rechne("=2^-2",           "0,25");
    rechne("=100*1,5",        "150");
    rechne("=0,1+0,2",        "0,3");
    rechne("= 8 / 2 - 1 ",    "3");
    rechne("=2^3^2",          "512");        /* rechts vor links */
    rechne("=1/0",            "#DIV/0");
    rechne("=3>2",            "1");
    rechne("=3<2",            "0");
    rechne("=2=2",            "1");
    rechne("=2<>2",           "0");
    rechne("=2<=2",           "1");
    rechne("=1>=2",           "0");
}

static void funktionen(void)
{
    printf("Funktionen\n");

    sheet_clear(&blatt);
    setze("A1", "10");
    setze("A2", "20");
    setze("A3", "30");
    setze("A4", "");
    setze("A5", "Text");
    setze("B1", "=SUMME(A1:A5)");
    setze("B2", "=MITTELWERT(A1:A3)");
    setze("B3", "=MIN(A1:A3)");
    setze("B4", "=MAX(A1:A3)");
    setze("B5", "=ANZAHL(A1:A5)");
    setze("C1", "=SUM(A1:A3)");
    setze("C2", "=ABS(-3,5)");
    setze("C3", "=WURZEL(16)");
    setze("C4", "=RUNDEN(3,14159; 2)");
    setze("C5", "=WENN(A1>5; 100; 200)");
    setze("D1", "=SUMME(A1:A3) / ANZAHL(A1:A3)");
    setze("D2", "=SUMME(1;2;3)");
    setze("D3", "=WURZEL(2)");
    setze("D4", "=RUNDEN(2,5)");
    setze("D5", "=MAX(A1:A3; 100)");
    sheet_recalc(&blatt);

    erwarte("SUMME(A1:A5)",       anzeige("B1"), "60");
    erwarte("MITTELWERT(A1:A3)",  anzeige("B2"), "20");
    erwarte("MIN(A1:A3)",         anzeige("B3"), "10");
    erwarte("MAX(A1:A3)",         anzeige("B4"), "30");
    erwarte("ANZAHL(A1:A5)",      anzeige("B5"), "3");
    erwarte("SUM(A1:A3)",         anzeige("C1"), "60");
    erwarte("ABS(-3,5)",          anzeige("C2"), "3,5");
    erwarte("WURZEL(16)",         anzeige("C3"), "4");
    erwarte("RUNDEN(3,14159; 2)", anzeige("C4"), "3,14");
    erwarte("WENN(A1>5;100;200)", anzeige("C5"), "100");
    erwarte("Summe durch Anzahl", anzeige("D1"), "20");
    erwarte("SUMME(1;2;3)",       anzeige("D2"), "6");
    erwarte("WURZEL(2)",          anzeige("D3"), "1,4142");
    erwarte("RUNDEN(2,5)",        anzeige("D4"), "3");
    erwarte("MAX mit Zahl",       anzeige("D5"), "100");
}

static void ketten(void)
{
    printf("Formeln, die aufeinander zeigen\n");

    sheet_clear(&blatt);
    setze("A1", "2");
    setze("A2", "=A1*3");
    setze("A3", "=A2+A1");
    setze("A4", "=A3*A3");
    sheet_recalc(&blatt);

    erwarte("A2 = A1*3",   anzeige("A2"), "6");
    erwarte("A3 = A2+A1",  anzeige("A3"), "8");
    erwarte("A4 = A3*A3",  anzeige("A4"), "64");

    /* Reihenfolge umgekehrt: die spaetere Zelle zeigt nach vorn. */
    sheet_clear(&blatt);
    setze("A1", "=A2+1");
    setze("A2", "=A3+1");
    setze("A3", "5");
    sheet_recalc(&blatt);
    erwarte("Vorwaertsbezug", anzeige("A1"), "7");

    /* Aendern und neu rechnen. */
    setze("A3", "10");
    sheet_recalc(&blatt);
    erwarte("nach Aenderung", anzeige("A1"), "12");
}

static void fehlerfaelle(void)
{
    printf("Fehler\n");

    sheet_clear(&blatt);
    setze("A1", "=A2");
    setze("A2", "=A1");
    sheet_recalc(&blatt);
    erwarte("Kreis A1<->A2", anzeige("A1"), "#KREIS");

    sheet_clear(&blatt);
    setze("A1", "=A1");
    sheet_recalc(&blatt);
    erwarte("Zelle auf sich selbst", anzeige("A1"), "#KREIS");

    sheet_clear(&blatt);
    setze("A1", "=B1");
    setze("B1", "=C1");
    setze("C1", "=A1");
    sheet_recalc(&blatt);
    erwarte("Kreis ueber drei Ecken", anzeige("A1"), "#KREIS");

    rechne("=UNBEKANNT(1)", "#NAME");
    rechne("=1+",           "#FORMEL");
    rechne("=(1+2",         "#FORMEL");
    rechne("=1 2",          "#FORMEL");
    rechne("=A1:B2",        "#WERT");
    rechne("=WURZEL(-1)",   "#WERT");
    rechne("=ABS(1;2)",     "#FORMEL");
}

static void text_und_zahl(void)
{
    printf("Text, Zahl und Formel unterscheiden\n");

    sheet_clear(&blatt);
    setze("A1", "Hallo");
    setze("A2", "42");
    setze("A3", "=A2*2");
    setze("A4", "");
    sheet_recalc(&blatt);

    geprueft++;
    if (sheet_cell(&blatt, 0, 0)->kind != CELL_TEXT) {
        printf("  FEHLER: \"Hallo\" ist kein Text\n");
        fehler++;
    }
    geprueft++;
    if (sheet_cell(&blatt, 1, 0)->kind != CELL_NUMBER) {
        printf("  FEHLER: \"42\" ist keine Zahl\n");
        fehler++;
    }
    geprueft++;
    if (sheet_cell(&blatt, 2, 0)->kind != CELL_FORMULA) {
        printf("  FEHLER: \"=A2*2\" ist keine Formel\n");
        fehler++;
    }
    geprueft++;
    if (sheet_cell(&blatt, 3, 0)->kind != CELL_EMPTY) {
        printf("  FEHLER: leere Zelle ist nicht leer\n");
        fehler++;
    }

    erwarte("Text bleibt Text",  anzeige("A1"), "Hallo");
    erwarte("Formel auf Text",   anzeige("A3"), "84");

    /* Text zaehlt in einer Rechnung als null. */
    setze("B1", "=A1+5");
    sheet_recalc(&blatt);
    erwarte("Text als null", anzeige("B1"), "5");

    geprueft++;
    if (sheet_used(&blatt) != 4) {
        printf("  FEHLER: %u belegte Zellen, erwartet 4\n",
               (unsigned)sheet_used(&blatt));
        fehler++;
    }
}

static void datei(void)
{
    printf("Speichern und Laden\n");

    sheet_clear(&blatt);
    setze("A1", "Monat");
    setze("B1", "Umsatz");
    setze("A2", "Januar");
    setze("B2", "1200,5");
    setze("A3", "Februar");
    setze("B3", "980");
    setze("A4", "Summe");
    setze("B4", "=SUMME(B2:B3)");
    setze("C1", "mit ; drin");
    setze("C2", "mit \" drin");
    sheet_recalc(&blatt);

    char csv[4096];
    size_t n = sheet_to_csv(&blatt, csv, sizeof(csv));

    geprueft++;
    if (n == 0 || n >= sizeof(csv)) {
        printf("  FEHLER: CSV hat %u Zeichen\n", (unsigned)n);
        fehler++;
    }

    /* Die Formel muss als Formel im Text stehen. */
    geprueft++;
    if (!strstr(csv, "=SUMME(B2:B3)")) {
        printf("  FEHLER: die Formel fehlt in der Datei\n");
        fehler++;
    }

    struct sheet zurueck;

    sheet_from_csv(&zurueck, csv, n);

    char buffer[64];

    sheet_display(&zurueck, 0, 0, buffer, sizeof(buffer));
    erwarte("A1 nach dem Laden", buffer, "Monat");
    sheet_display(&zurueck, 1, 1, buffer, sizeof(buffer));
    erwarte("B2 nach dem Laden", buffer, "1200,5");
    sheet_display(&zurueck, 3, 1, buffer, sizeof(buffer));
    erwarte("Formel rechnet wieder", buffer, "2180,5");
    sheet_display(&zurueck, 0, 2, buffer, sizeof(buffer));
    erwarte("Strichpunkt im Feld", buffer, "mit ; drin");
    sheet_display(&zurueck, 1, 2, buffer, sizeof(buffer));
    erwarte("Anfuehrungszeichen", buffer, "mit \" drin");
}

int main(void)
{
    printf("=== Tabellenkalkulation ===\n");

    zahlen();
    bezuege();
    rechnen();
    funktionen();
    ketten();
    fehlerfaelle();
    text_und_zahl();
    datei();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
