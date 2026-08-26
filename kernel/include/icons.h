/* icons.h - die Symbolsammlung der Oberflaeche.
 *
 * Jedes Symbol ist 16x16 Pixel gross und als Textbild abgelegt: ein Zeichen
 * je Pixel. Das laesst sich im Quelltext lesen und aendern, ohne dass ein
 * Zeichenprogramm noetig waere.
 */
#ifndef ICONS_H
#define ICONS_H

#include "gfx.h"

#define ICON_SIZE 16

enum icon_id {
    ICON_FOLDER,
    ICON_FOLDER_OPEN,
    ICON_FILE,
    ICON_FILE_TEXT,
    ICON_DISK,
    ICON_COMPUTER,
    ICON_TERMINAL,
    ICON_EDITOR,
    ICON_INFO,
    ICON_TRASH,
    ICON_UP,
    ICON_BACK,
    ICON_HOME,
    ICON_NEW_FOLDER,
    ICON_NEW_FILE,
    ICON_SETTINGS,
    ICON_COUNT
};

void icon_draw(struct canvas *c, int32_t x, int32_t y, enum icon_id id, int32_t scale);

#endif /* ICONS_H */
