/* dom.h - Baum eines geladenen Dokuments.
 *
 * Der Baum ist bewusst schlicht: Elemente, Text und Kommentare. Jedes
 * Element traegt seine Attribute als verkettete Liste, den berechneten
 * Stil und die zuletzt berechnete Kastenlage - Letzteres, damit Maus
 * und Skripte einen Knoten wiederfinden.
 */
#ifndef DOM_H
#define DOM_H

#include "retro.h"
#include "gfx.h"

enum node_kind {
    NODE_ELEMENT,
    NODE_TEXT,
    NODE_COMMENT,
    NODE_DOCUMENT,
};

struct attribute {
    char *name;                 /* stets klein geschrieben */
    char *value;
    struct attribute *next;
};

/* Berechnete Darstellungsangaben eines Elements. */
enum css_display {
    DISPLAY_INLINE,
    DISPLAY_BLOCK,
    DISPLAY_LIST_ITEM,
    DISPLAY_INLINE_BLOCK,
    DISPLAY_NONE,
    DISPLAY_TABLE,
    DISPLAY_TABLE_ROW,
    DISPLAY_TABLE_CELL,
};

enum css_align { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT, ALIGN_JUSTIFY };

/* Eine Laengenangabe: entweder in Pixeln, in Prozent oder "auto". */
enum length_unit { LEN_AUTO, LEN_PX, LEN_PERCENT };

struct length {
    enum length_unit unit;
    int32_t          value;     /* Pixel bzw. Prozent */
};

struct style {
    uint32_t color;
    uint32_t background;
    bool     has_background;

    int32_t  font_size;         /* in Pixeln */
    bool     bold;
    bool     italic;
    bool     underline;
    bool     strike;
    bool     monospace;
    bool     preformatted;
    bool     uppercase;
    bool     lowercase;

    enum css_display display;
    enum css_align   align;

    struct length width, height;
    int32_t  margin[4];         /* oben, rechts, unten, links */
    int32_t  padding[4];
    int32_t  border[4];
    uint32_t border_color[4];
    int32_t  border_radius;

    int32_t  line_height;
    int32_t  letter_spacing;

    bool     hidden;            /* visibility: hidden */
    int32_t  opacity;           /* 0 bis 255 */
    int32_t  z_index;
    bool     float_left, float_right;
    bool     clear_left, clear_right;
    bool     absolute, fixed, relative;
    struct length left, top, right, bottom;
    bool     nowrap;
    bool     rounded_forced;
};

struct node {
    enum node_kind kind;

    char *name;                 /* Elementname, klein geschrieben */
    char *text;                 /* bei Text und Kommentar */

    struct attribute *attributes;

    struct node *parent;
    struct node *first, *last;  /* Kinder */
    struct node *previous, *next;

    struct style style;
    bool         styled;

    /* Ergebnis des letzten Umbruchs - Bildschirmlage im Inhaltsbereich. */
    struct rect box;
    bool        has_box;

    /* Vom Skript gesetzte Ereignisbehandlungen. */
    char *on_click;
    char *on_change;
    char *on_input;
    char *on_submit;

    /* Fuer Eingabefelder und Auswahlfelder. */
    char *value;
    bool  checked;

    uint32_t id;                /* fortlaufend, fuer Skripte */
};

struct document {
    struct node *root;          /* NODE_DOCUMENT */
    struct node *html;
    struct node *head;
    struct node *body;
    char         title[160];
    uint32_t     next_id;
};

/* --- Aufbau --- */
struct node *node_create(enum node_kind kind, const char *name);
void         node_append(struct node *parent, struct node *child);
void         node_remove(struct node *child);
void         node_insert_before(struct node *parent, struct node *child,
                                struct node *before);
void         node_free(struct node *node);

/* --- Attribute --- */
const char *node_attribute(const struct node *node, const char *name);
void        node_set_attribute(struct node *node, const char *name,
                               const char *value);
void        node_remove_attribute(struct node *node, const char *name);
bool        node_has_class(const struct node *node, const char *class_name);

/* --- Suche --- */
struct node *dom_by_id(struct node *root, const char *id);
struct node *dom_by_tag(struct node *root, const char *tag, size_t index);
size_t       dom_count_tag(struct node *root, const char *tag);

/* Sammelt bis zu max Treffer eines einfachen Selektors. */
size_t dom_query(struct node *root, const char *selector,
                 struct node **out, size_t max);

/* --- Text --- */
/* Haengt den sichtbaren Text des Teilbaums an den Puffer an. */
void dom_text_content(const struct node *node, char *out, size_t size);

/* Wie oben, nimmt aber auch den Inhalt von script und style mit. */
void dom_raw_text(const struct node *node, char *out, size_t size);

/* --- Dokument --- */
void document_init(struct document *doc);
void document_free(struct document *doc);

#endif /* DOM_H */
