/* font.c - die gewaehlte Schriftart.
 *
 * Die Punkte selbst liegen in font_data.c, erzeugt aus den Vorlagen
 * unter third_party/fonts. Hier steht nur, welche davon gerade gilt.
 */

#include "font.h"
#include "kstring.h"

/* font_active selbst wird in font_data.c gesetzt, damit schon der
 * allererste Text Punkte findet - noch vor jedem Aufruf hier. */
static size_t current;

size_t font_count(void) { return FONT_FACES; }

size_t font_current(void) { return current; }

const char *font_name(size_t index)
{
    return index < FONT_FACES ? font_faces[index].name : "";
}

const char *font_license(size_t index)
{
    return index < FONT_FACES ? font_faces[index].license : "";
}

void font_select(size_t index)
{
    if (index >= FONT_FACES)
        return;
    current = index;
    font_active = font_faces[index].glyphs;
}

bool font_select_by_name(const char *name)
{
    if (!name || !name[0])
        return false;

    for (size_t i = 0; i < FONT_FACES; i++) {
        if (strcasecmp(font_faces[i].name, name) == 0) {
            font_select(i);
            return true;
        }
    }
    return false;
}
