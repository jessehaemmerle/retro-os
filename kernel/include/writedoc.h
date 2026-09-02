/* writedoc.h - das Dokument der Textverarbeitung.
 *
 * Ein Dokument ist eine Folge von Absaetzen. Jeder hat eine
 * Formatvorlage, die sagt, wie er aussieht, und eine Ausrichtung. Die
 * Auszeichnungen sitzen dagegen an den einzelnen Zeichen: fett und
 * unterstrichen koennen mitten im Wort anfangen und aufhoeren, ein
 * Absatzformat nicht.
 *
 * Kursiv fehlt mit Absicht. Der Zeichensatz hat nur einen aufrechten
 * und einen fetten Schnitt; ein schraeg gerechnetes Bild waere
 * unleserlich. Unterstreichen dagegen ist eine Linie und kostet nichts.
 *
 * Gespeichert wird HTML. Das ist kein Zufall: RetroOS hat einen
 * Browser, und ein Text, den man auch anschauen und ausliefern kann,
 * ist mehr wert als ein eigenes Format, das nur ein Programm liest.
 */
#ifndef WRITEDOC_H
#define WRITEDOC_H

#include "retro.h"

#define DOC_PARAS_MAX 200
#define PARA_TEXT_MAX 512

/* Auszeichnungen an einem Zeichen. */
#define MARK_BOLD      0x01
#define MARK_UNDERLINE 0x02

enum para_style {
    STYLE_BODY,
    STYLE_H1,
    STYLE_H2,
    STYLE_LIST,
    STYLE_QUOTE,
    STYLE_COUNT
};

enum para_align {
    WA_LEFT,
    WA_CENTER,
    WA_RIGHT,
};

struct paragraph {
    char    text[PARA_TEXT_MAX];
    uint8_t marks[PARA_TEXT_MAX];
    int     len;
    uint8_t style;
    uint8_t align;
};

struct wdoc {
    struct paragraph paras[DOC_PARAS_MAX];
    int              count;
};

void wdoc_clear(struct wdoc *doc);

/* Fuegt hinter index einen leeren Absatz ein und liefert seine Nummer. */
int  wdoc_insert(struct wdoc *doc, int index, uint8_t style);
void wdoc_remove(struct wdoc *doc, int index);

/* Setzt ein Zeichen an eine Stelle im Absatz. */
void wdoc_insert_char(struct wdoc *doc, int para, int at, char ch,
                      uint8_t marks);
void wdoc_erase_char(struct wdoc *doc, int para, int at);

/* Teilt einen Absatz an der Stelle - was die Eingabetaste tut. */
void wdoc_split(struct wdoc *doc, int para, int at);
/* Haengt den Absatz an seinen Vorgaenger - was die Ruecktaste am
 * Absatzanfang tut. Liefert die Stelle, an der die Naht sitzt. */
int  wdoc_join(struct wdoc *doc, int para);

const char *wdoc_style_name(uint8_t style);

size_t wdoc_words(const struct wdoc *doc);
size_t wdoc_chars(const struct wdoc *doc);

/* --- Dateien --- */
size_t wdoc_to_html(const struct wdoc *doc, const char *title,
                    char *out, size_t size);
/* Liest ein Dokument aus HTML. Was nicht zu den bekannten Bausteinen
 * gehoert, wird zu gewoehnlichen Absaetzen. */
void   wdoc_from_html(struct wdoc *doc, const char *html, size_t length,
                      char *title, size_t title_size);

#endif /* WRITEDOC_H */
