/* css.h - Formatvorlagen lesen und auf den Dokumentbaum anwenden. */
#ifndef CSS_H
#define CSS_H

#include "dom.h"

struct stylesheet;

/* Erzeugt ein leeres Regelwerk mit den eingebauten Vorgaben. */
struct stylesheet *css_create(void);
void               css_free(struct stylesheet *sheet);

/* Nimmt weitere Regeln auf; darf mehrfach aufgerufen werden. */
void css_add(struct stylesheet *sheet, const char *text, size_t length);

/* Berechnet fuer jeden Knoten den Stil und legt ihn im Baum ab. */
void css_apply(struct stylesheet *sheet, struct node *root, int32_t base_size);

/* Liest eine einzelne Eigenschaftsliste, wie sie im style-Attribut steht. */
void css_apply_inline(struct style *style, const char *text,
                      const struct style *parent);

/* Farbnamen und Schreibweisen wie #abc oder rgb(1,2,3). */
bool css_parse_color(const char *text, uint32_t *out);

#endif /* CSS_H */
