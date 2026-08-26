/* widgets.h - wiederkehrende Bedienelemente der Oberflaeche. */
#ifndef WIDGETS_H
#define WIDGETS_H

#include "gfx.h"
#include "icons.h"

/* Knopf mit optionalem Symbol. */
void widget_button(struct canvas *c, struct rect r, const char *label,
                   bool pressed, bool enabled);
void widget_icon_button(struct canvas *c, struct rect r, enum icon_id icon,
                        const char *label, bool pressed, bool enabled);

/* Eingabefeld; cursor < 0 blendet die Schreibmarke aus. */
void widget_field(struct canvas *c, struct rect r, const char *text,
                  int32_t cursor, bool focused);

/* Senkrechte Bildlaufleiste. total/visible in Zeilen, offset in Zeilen. */
void widget_vscroll(struct canvas *c, struct rect r,
                    int32_t offset, int32_t total, int32_t visible);
/* Liefert den neuen Offset fuer einen Klick bei y innerhalb der Leiste. */
int32_t widget_vscroll_click(struct rect r, int32_t y,
                             int32_t offset, int32_t total, int32_t visible);

#define SCROLLBAR_WIDTH 16

/* Statuszeile am unteren Rand eines Fensters. */
void widget_statusbar(struct canvas *c, struct rect r, const char *left,
                      const char *right);

/* Werkzeugleiste als Hintergrundflaeche. */
void widget_toolbar(struct canvas *c, struct rect r);

#endif /* WIDGETS_H */
