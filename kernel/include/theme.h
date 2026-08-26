/* theme.h - die Farbwelt von RetroOS.
 *
 * Bewusst am Look der Neunziger orientiert: grauer Kunststoff, harte
 * 3D-Kanten, kraeftige Titelleisten.
 */
#ifndef THEME_H
#define THEME_H

#include "gfx.h"

#define COL_DESKTOP_TOP     RGB(0x1B, 0x63, 0x6E)
#define COL_DESKTOP_BOTTOM  RGB(0x0B, 0x33, 0x3B)

#define COL_FACE            RGB(0xC6, 0xC6, 0xC6)
#define COL_FACE_LIGHT      RGB(0xE8, 0xE8, 0xE8)
#define COL_HILIGHT         RGB(0xFF, 0xFF, 0xFF)
#define COL_SHADOW          RGB(0x86, 0x86, 0x86)
#define COL_DARK            RGB(0x3A, 0x3A, 0x3A)
#define COL_BLACK           RGB(0x00, 0x00, 0x00)
#define COL_WHITE           RGB(0xFF, 0xFF, 0xFF)

#define COL_TITLE_A1        RGB(0x0A, 0x24, 0x6A)
#define COL_TITLE_A2        RGB(0x2A, 0x8C, 0xD0)
#define COL_TITLE_I1        RGB(0x6E, 0x6E, 0x6E)
#define COL_TITLE_I2        RGB(0xA6, 0xA6, 0xA6)
#define COL_TITLE_TEXT      RGB(0xFF, 0xFF, 0xFF)

#define COL_TEXT            RGB(0x10, 0x10, 0x10)
#define COL_TEXT_DIM        RGB(0x60, 0x60, 0x60)
#define COL_SELECT          RGB(0x0A, 0x24, 0x6A)
#define COL_SELECT_TEXT     RGB(0xFF, 0xFF, 0xFF)

#define COL_FIELD           RGB(0xFF, 0xFF, 0xFF)
#define COL_TASKBAR         RGB(0xC6, 0xC6, 0xC6)
#define COL_ACCENT          RGB(0xD8, 0x6E, 0x1E)

#define TITLEBAR_HEIGHT     22
#define TASKBAR_HEIGHT      30
#define BORDER_WIDTH        3

#endif /* THEME_H */
