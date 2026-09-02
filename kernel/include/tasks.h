/* tasks.h - die Aufgabenliste.
 *
 * Eine Aufgabe ist eine Zeile Text, ein Haken, eine Wichtigkeit und
 * wahlweise ein Termin. Mehr braucht es nicht: Alles, was darueber
 * hinausgeht - Unteraufgaben, Anhaenge, Zustaendigkeiten -, macht aus
 * einer Liste, die man in zehn Sekunden fuehrt, ein Programm, das man
 * pflegen muss.
 *
 * Der Kern hier kennt weder Fenster noch Dateien; er sortiert, zaehlt
 * und wandelt zwischen Liste und Text. Genau deshalb laesst er sich auf
 * dem Entwicklungsrechner pruefen.
 *
 * Das Textformat ist so gebaut, dass man es im Editor lesen und
 * schreiben kann - eine Zeile je Aufgabe:
 *
 *     [ ] hoch    2026-09-15  Bericht schreiben
 *     [x] mittel  -           Milch kaufen
 *
 * Gespeichert wird es im Heimatverzeichnis: Aufgaben gehoeren dem, der
 * sie hat, und nicht dem Rechner.
 */
#ifndef TASKS_H
#define TASKS_H

#include "retro.h"

#define TASK_MAX      64
#define TASK_TEXT_MAX 95

enum task_prio {
    TP_LOW,
    TP_MID,
    TP_HIGH,
    TP_COUNT
};

struct task {
    bool     used;
    bool     done;
    uint32_t id;
    uint8_t  prio;
    /* 0 im Jahr heisst: kein Termin. */
    uint16_t year;
    uint8_t  month, day;
    char     text[TASK_TEXT_MAX + 1];
};

struct tasklist {
    struct task items[TASK_MAX];
    uint32_t    next_id;
};

void         tasks_clear(struct tasklist *list);
struct task *tasks_add(struct tasklist *list, const char *text);
struct task *tasks_by_id(struct tasklist *list, uint32_t id);
bool         tasks_remove(struct tasklist *list, uint32_t id);
/* Wirft alles Erledigte weg und liefert, wie viel das war. */
size_t       tasks_purge_done(struct tasklist *list);

size_t tasks_count(const struct tasklist *list, bool only_open);

/* Sortiert nach dem, was als Naechstes ansteht: Offenes vor Erledigtem,
 * darin Termine vor Terminlosem und frueher vor spaeter, dann die
 * Wichtigkeit, zuletzt der Text. Liefert die Anzahl der Zeiger in out. */
size_t tasks_sorted(struct tasklist *list, struct task **out, size_t max,
                    bool hide_done);

/* "15.09.2026", "15.9.2026" oder "2026-09-15". Ein Strich oder ein
 * leerer Text loeschen den Termin. */
bool tasks_parse_date(const char *text, uint16_t *year, uint8_t *month,
                      uint8_t *day);
/* "15.09.2026", oder "-" ohne Termin. */
void tasks_format_date(const struct task *t, char *out, size_t size);

/* Ist der Termin ueberschritten? Erledigtes nie. */
bool tasks_overdue(const struct task *t, uint16_t year, uint8_t month,
                   uint8_t day);

const char *task_prio_name(uint8_t prio);
bool        task_prio_parse(const char *text, uint8_t *out);

/* Schreibt die Liste als Text und liefert die Laenge. */
size_t tasks_to_text(const struct tasklist *list, char *out, size_t size);
/* Liest sie wieder ein; alles Vorherige wird verworfen. */
void   tasks_from_text(struct tasklist *list, const char *text);

#endif /* TASKS_H */
