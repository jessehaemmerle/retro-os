/* search.h - die Suche ueber alles (Alt+Leertaste).
 *
 * Ein Feld, in das man tippt, und darunter, was dazu passt: Programme,
 * Dateien und Einstellungen in einer Liste. Statt zu wissen, wo etwas
 * liegt, schreibt man, wie es heisst.
 *
 * Die Bewertung steht hier getrennt vom Fenster, weil sich genau dort
 * entscheidet, ob die Liste taugt: Ein Treffer am Wortanfang ist mehr
 * wert als einer in der Mitte, ein kurzer Name mehr als ein langer,
 * und der genaue Name schlaegt alles. Das laesst sich auf dem
 * Entwicklungsrechner pruefen, das Fenster nicht.
 */
#ifndef SEARCH_H
#define SEARCH_H

#include "retro.h"

/* Bewertet, wie gut query auf text passt. -1 heisst: gar nicht.
 * Sonst gilt: je hoeher, desto besser. Gross- und Kleinschreibung
 * spielen keine Rolle. */
int32_t search_score(const char *text, const char *query);

/* Sortiert eine Trefferliste absteigend nach Punkten. Bei
 * Gleichstand bleibt die urspruengliche Reihenfolge erhalten - so
 * stehen Programme vor Dateien, wenn beide gleich gut passen. */
struct search_hit;
void search_sort(struct search_hit *hits, size_t count);

enum search_kind {
    SEARCH_APP,
    SEARCH_FILE,
    SEARCH_SETTING,
};

#define SEARCH_MAX_HITS 64

struct search_hit {
    char             label[64];
    char             detail[80];
    enum search_kind kind;
    int              icon;        /* enum icon_id */
    int32_t          score;
    size_t           index;       /* Nummer im Programm- bzw. Zeilensatz */
    void            *node;        /* struct fs_node * bei Dateien */
};

/* Sammelt die Treffer zu einer Eingabe. Liefert, wie viele in out
 * stehen. Eine leere Eingabe liefert nichts. */
size_t search_collect(const char *query, struct search_hit *out, size_t max);

/* Fuehrt einen Treffer aus: Programm starten, Datei oeffnen,
 * Einstellungen aufmachen. */
void search_run(const struct search_hit *hit);

#endif /* SEARCH_H */
