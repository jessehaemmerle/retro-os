/* deck.h - die Folien einer Praesentation.
 *
 * Eine Folie hat einen Titel und eine Handvoll Zeilen darunter. Wie
 * beides aussieht, entscheidet die Anordnung: Eine Titelfolie stellt
 * den Titel gross in die Mitte, eine Punktfolie schreibt ihn oben hin
 * und zaehlt darunter auf, ein Zitat rueckt ein und laesst den Titel
 * als Quellenangabe stehen.
 *
 * Gespeichert wird eine schlichte Textdatei. Sie laesst sich mit jedem
 * Editor aendern - auch mit dem dieses Systems - und ist in zwei
 * Minuten erklaert:
 *
 *     # Titel der Folie          beginnt eine neue Folie
 *     ! Titelfolie               Anordnung fuer diese Folie
 *     - Ein Punkt                eine Zeile darunter
 */
#ifndef DECK_H
#define DECK_H

#include "retro.h"

#define DECK_SLIDES_MAX 60
#define SLIDE_LINES_MAX 10
#define SLIDE_TEXT_MAX  96

enum slide_layout {
    LAYOUT_TITLE,       /* grosser Titel, Zeilen als Untertitel */
    LAYOUT_BULLETS,     /* Titel oben, Punkte darunter          */
    LAYOUT_QUOTE,       /* eingerueckt, Titel als Herkunft      */
    LAYOUT_COUNT
};

struct slide {
    char    title[SLIDE_TEXT_MAX];
    char    lines[SLIDE_LINES_MAX][SLIDE_TEXT_MAX];
    int     line_count;
    uint8_t layout;
};

struct deck {
    struct slide slides[DECK_SLIDES_MAX];
    int          count;
};

void deck_clear(struct deck *deck);

/* Legt hinter index eine Folie an und liefert ihre Nummer. */
int  deck_insert(struct deck *deck, int index);
void deck_remove(struct deck *deck, int index);
void deck_move(struct deck *deck, int index, int direction);

/* Fuegt in einer Folie eine Zeile hinter index ein. */
int  slide_insert_line(struct slide *slide, int index);
void slide_remove_line(struct slide *slide, int index);

const char *deck_layout_name(uint8_t layout);

/* --- Dateien --- */
size_t deck_to_text(const struct deck *deck, char *out, size_t size);
void   deck_from_text(struct deck *deck, const char *text, size_t length);

#endif /* DECK_H */
