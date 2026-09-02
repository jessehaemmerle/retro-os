/* sheet.h - das Rechenwerk der Tabellenkalkulation.
 *
 * Eine Tabelle ist ein Gitter aus Zellen. Jede haelt, was der Benutzer
 * hineingeschrieben hat; was daraus wird, entscheidet der erste
 * Buchstabe: Ein Gleichheitszeichen macht eine Formel daraus, eine
 * lesbare Zahl eine Zahl, alles andere bleibt Text.
 *
 * Gerechnet wird in Festkomma - der Kern hat keine Gleitkommaeinheit.
 * Eine Eins sind SHEET_SCALE Schritte, also vier Nachkommastellen.
 * Das reicht fuer Geld, Mengen und Mittelwerte und rechnet exakt,
 * solange man bei Zehnteln bleibt.
 *
 * Formeln koennen einander benutzen. Damit ein Kreis (A1 = B1 und
 * B1 = A1) nicht endlos rechnet, merkt sich jede Zelle, ob sie gerade
 * ausgewertet wird; trifft die Auswertung darauf, ist es ein
 * Kreisbezug.
 *
 * Die Sprache der Formeln:
 *
 *   Zahlen        12   -3,5   0.25      (Komma und Punkt gelten beide)
 *   Bezuege       A1   $B$7              (Dollarzeichen werden ueberlesen)
 *   Bereiche      A1:C9                  (nur als Argument einer Funktion)
 *   Rechnen       + - * / ^  und Klammern
 *   Vergleiche    =  <>  <  <=  >  >=    (liefern 1 oder 0)
 *   Funktionen    SUMME MITTELWERT MIN MAX ANZAHL RUNDEN ABS WURZEL WENN
 *                 dazu die englischen Namen SUM AVERAGE COUNT ROUND
 *                 SQRT IF
 */
#ifndef SHEET_H
#define SHEET_H

#include "retro.h"

#define SHEET_COLS      26          /* A bis Z            */
#define SHEET_ROWS      100
#define SHEET_TEXT_MAX  48
#define SHEET_SCALE     10000LL     /* vier Nachkommastellen */

typedef int64_t sheet_num;

enum cell_kind {
    CELL_EMPTY,
    CELL_TEXT,
    CELL_NUMBER,
    CELL_FORMULA,
};

enum cell_error {
    CELL_OK,
    CELL_ERR_SYNTAX,    /* die Formel ist nicht lesbar        */
    CELL_ERR_CYCLE,     /* die Zelle braucht sich selbst      */
    CELL_ERR_REF,       /* ein Bezug zeigt aus der Tabelle    */
    CELL_ERR_DIV0,      /* geteilt durch null                 */
    CELL_ERR_NAME,      /* diese Funktion gibt es nicht       */
    CELL_ERR_VALUE,     /* das laesst sich nicht ausrechnen   */
};

struct cell {
    char      text[SHEET_TEXT_MAX];
    sheet_num value;
    uint8_t   kind;
    uint8_t   error;
    uint8_t   state;    /* nur waehrend des Rechnens benutzt */
    uint8_t   bold;
};

struct sheet {
    struct cell cells[SHEET_ROWS][SHEET_COLS];
};

void sheet_clear(struct sheet *sh);

/* Setzt den Inhalt einer Zelle. Rechnet noch nicht neu - das macht
 * sheet_recalc(), damit mehrere Aenderungen einen Durchgang teilen. */
void sheet_set(struct sheet *sh, int row, int col, const char *text);
void sheet_recalc(struct sheet *sh);

const struct cell *sheet_cell(const struct sheet *sh, int row, int col);
/* Was in der Zelle stehen soll: die Zahl, der Text oder die Fehlermarke. */
void sheet_display(const struct sheet *sh, int row, int col,
                   char *out, size_t size);
/* Steht rechtsbuendig? Zahlen ja, Text nein. */
bool sheet_is_numeric(const struct sheet *sh, int row, int col);
/* Wie viele Zellen sind belegt? */
size_t sheet_used(const struct sheet *sh);

/* --- Zahlen --- */
/* Liest eine Zahl; liefert false, wenn der Text keine ist. */
bool  sheet_parse_number(const char *text, sheet_num *out);
/* Schreibt eine Zahl in deutscher Schreibweise, ohne unnoetige Nullen. */
void  sheet_format_number(sheet_num value, char *out, size_t size);

/* --- Bezuege --- */
/* "B7" -> Zeile 6, Spalte 1. */
bool sheet_parse_ref(const char *text, int *row, int *col);
void sheet_ref_name(int row, int col, char *out, size_t size);

const char *sheet_error_text(enum cell_error error);

/* --- Dateien --- */
/* Als Tabelle mit Strichpunkten - so, wie es die deutsche Fassung
 * verbreiteter Programme schreibt. Formeln bleiben Formeln. */
size_t sheet_to_csv(const struct sheet *sh, char *out, size_t size);
void   sheet_from_csv(struct sheet *sh, const char *text, size_t length);

#endif /* SHEET_H */
