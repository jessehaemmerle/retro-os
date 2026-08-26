/* html.h - Zerlegung einer HTML-Seite in darstellbare Bausteine. */
#ifndef HTML_H
#define HTML_H

#include "retro.h"

enum html_item_type {
    HTML_TEXT,
    HTML_LINK,
    HTML_BREAK,       /* Zeilenwechsel  */
    HTML_PARAGRAPH,   /* Absatzabstand  */
    HTML_RULE,        /* Trennlinie     */
    HTML_BULLET,      /* Aufzaehlung    */
    HTML_IMAGE,       /* Platzhalter    */
};

struct html_item {
    enum html_item_type type;
    char   *text;
    char   *href;
    bool    bold;
    bool    pre;
    uint8_t heading;   /* 0 = normal, sonst 1-6 */
};

struct html_doc {
    struct html_item *items;
    size_t            count;
    size_t            capacity;
    char              title[128];
};

void html_parse(struct html_doc *doc, const char *source, size_t length);
void html_parse_plain(struct html_doc *doc, const char *source, size_t length);
void html_free(struct html_doc *doc);

#endif /* HTML_H */
