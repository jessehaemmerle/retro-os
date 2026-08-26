/* layout.h - Umbruch eines gestalteten Dokuments in Zeichenstuecke.
 *
 * Der Umbruch liefert eine flache Liste in Zeichenreihenfolge: erst der
 * Hintergrund eines Kastens, dann sein Inhalt. So genuegt zum Malen ein
 * Durchlauf von vorne nach hinten.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include "dom.h"
#include "image.h"

enum fragment_kind {
    FRAG_BOX,        /* Hintergrund und Rahmen eines Kastens */
    FRAG_TEXT,
    FRAG_IMAGE,
    FRAG_BULLET,
    FRAG_RULE,
    FRAG_FIELD,      /* Eingabefeld    */
    FRAG_BUTTON,
    FRAG_CHECKBOX,
};

struct fragment {
    enum fragment_kind kind;
    struct rect  rect;           /* Lage im Dokument, nicht im Fenster */
    struct node *node;           /* Ursprung, fuer Verweise und Skripte */

    char        *text;           /* eigener Speicher bei FRAG_TEXT */
    struct image *image;         /* geliehen aus dem Bildspeicher */

    uint32_t color;
    uint32_t background;
    bool     has_background;
    uint32_t border_color[4];
    int32_t  border[4];

    int32_t  font_size;
    bool     bold, italic, underline, strike;
    int32_t  tracking;
    bool     checked;
};

struct layout {
    struct fragment *items;
    size_t           count;
    size_t           capacity;
    int32_t          width;      /* verfuegbare Breite   */
    int32_t          height;     /* Gesamthoehe          */
};

/* Beschafft ein Bild zu einer Adresse; NULL heisst "nicht vorhanden".
 * Der Browser reicht hier seinen Bildspeicher herein. */
typedef struct image *(*image_lookup)(void *context, const char *url);

void layout_run(struct layout *out, struct node *body, int32_t width,
                image_lookup lookup, void *context);
void layout_free(struct layout *out);

/* Findet den obersten Knoten unter einem Punkt im Dokument. */
struct node *layout_hit(const struct layout *out, int32_t x, int32_t y);

#endif /* LAYOUT_H */
