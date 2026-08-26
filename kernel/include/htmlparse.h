/* htmlparse.h - HTML in einen Dokumentbaum verwandeln. */
#ifndef HTMLPARSE_H
#define HTMLPARSE_H

#include "dom.h"

/* Baut den Baum auf. Fehlende Schlusszeichen und verschachtelte
 * Absaetze werden wie in einem Browser stillschweigend geradegerueckt. */
void html_build(struct document *doc, const char *source, size_t length);

/* Nimmt reinen Text und packt ihn in ein <pre>. */
void html_build_plain(struct document *doc, const char *source, size_t length);

/* Ersetzt die Kinder eines Knotens durch das Ergebnis von fragment. */
void html_set_inner(struct node *parent, const char *fragment);

/* Loest Zeichenverweise wie &amp; auf; gibt die neue Laenge zurueck. */
size_t html_unescape(char *text);

#endif /* HTMLPARSE_H */
