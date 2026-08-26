/* font.h - eingebaute 8x16-Bitmapschrift (Latin-1). */
#ifndef FONT_H
#define FONT_H

#include "retro.h"

#define FONT_WIDTH   8
#define FONT_HEIGHT  16
#define FONT_FIRST   0x20
#define FONT_LAST    0xFF
#define FONT_GLYPHS  (FONT_LAST - FONT_FIRST + 1)

extern const uint8_t font8x16[FONT_GLYPHS][FONT_HEIGHT];

static inline const uint8_t *font_glyph(unsigned char c)
{
    if (c < FONT_FIRST)
        return font8x16[0];
    return font8x16[c - FONT_FIRST];
}

#endif /* FONT_H */
