/* theme.c - siehe theme.h.
 *
 * Zwei Tafeln, sonst nichts. Die helle ist die von Anfang an: grauer
 * Kunststoff, weisse Kanten, blaue Titelleiste. Die dunkle behaelt
 * genau denselben Aufbau - dieselben Kanten, dieselben Abstufungen -
 * und dreht nur die Helligkeit um. Ein Dunkelmodus, der nebenbei das
 * Aussehen aendert, ist zwei Aenderungen in einer.
 */

#include "theme.h"
#include "kstring.h"

static const struct theme hell = {
    .name = "Hell",
    .dark = false,

    .desktop_top    = RGB(0x1B, 0x63, 0x6E),
    .desktop_bottom = RGB(0x0B, 0x33, 0x3B),

    .face       = RGB(0xC6, 0xC6, 0xC6),
    .face_light = RGB(0xE8, 0xE8, 0xE8),
    .hilight    = RGB(0xFF, 0xFF, 0xFF),
    .shadow     = RGB(0x86, 0x86, 0x86),
    .dark_edge  = RGB(0x3A, 0x3A, 0x3A),

    .title_a1   = RGB(0x0A, 0x24, 0x6A),
    .title_a2   = RGB(0x2A, 0x8C, 0xD0),
    .title_i1   = RGB(0x6E, 0x6E, 0x6E),
    .title_i2   = RGB(0xA6, 0xA6, 0xA6),
    .title_text = RGB(0xFF, 0xFF, 0xFF),

    .text        = RGB(0x10, 0x10, 0x10),
    .text_dim    = RGB(0x60, 0x60, 0x60),
    .select      = RGB(0x0A, 0x24, 0x6A),
    .select_text = RGB(0xFF, 0xFF, 0xFF),

    .field   = RGB(0xFF, 0xFF, 0xFF),
    .taskbar = RGB(0xC6, 0xC6, 0xC6),
    .accent  = RGB(0xD8, 0x6E, 0x1E),
};

/* Die Kanten sind hier nicht weiss und grau, sondern grau und
 * schwarz - eine helle Kante auf dunklem Kunststoff leuchtet sonst
 * wie eine Leuchtstoffroehre. Die Schrift ist nicht reinweiss:
 * Weiss auf Schwarz flimmert, 0xE6 nicht. */
static const struct theme dunkel = {
    .name = "Dunkel",
    .dark = true,

    .desktop_top    = RGB(0x14, 0x28, 0x30),
    .desktop_bottom = RGB(0x07, 0x0F, 0x14),

    .face       = RGB(0x2E, 0x31, 0x36),
    .face_light = RGB(0x3E, 0x42, 0x48),
    .hilight    = RGB(0x5A, 0x5F, 0x66),
    .shadow     = RGB(0x1A, 0x1C, 0x1F),
    .dark_edge  = RGB(0x0B, 0x0C, 0x0E),

    .title_a1   = RGB(0x0C, 0x2A, 0x4E),
    .title_a2   = RGB(0x24, 0x6A, 0xA2),
    .title_i1   = RGB(0x2A, 0x2C, 0x30),
    .title_i2   = RGB(0x46, 0x4A, 0x50),
    .title_text = RGB(0xEC, 0xEC, 0xEC),

    .text        = RGB(0xE4, 0xE6, 0xE8),
    .text_dim    = RGB(0x96, 0x9A, 0xA0),
    .select      = RGB(0x1E, 0x5A, 0x96),
    .select_text = RGB(0xFF, 0xFF, 0xFF),

    .field   = RGB(0x1C, 0x1E, 0x22),
    .taskbar = RGB(0x24, 0x27, 0x2B),
    .accent  = RGB(0xE8, 0x8E, 0x38),
};

const struct theme *g_theme = &hell;

static enum theme_mode mode = THEME_HELL;

bool theme_dark_for_hour(int32_t hour)
{
    /* Ab sieben ist es hell, ab neunzehn dunkel. Eine Stunde, die
     * ausserhalb von 0..23 liegt, ist keine - dann bleibt es hell. */
    if (hour < 0 || hour > 23)
        return false;
    return hour >= 19 || hour < 7;
}

static void apply(bool dark)
{
    g_theme = dark ? &dunkel : &hell;
}

void theme_set_mode(enum theme_mode m)
{
    mode = m;

    if (m == THEME_AUTO)
        return;              /* der naechste theme_tick() entscheidet */
    apply(m == THEME_DUNKEL);
}

enum theme_mode theme_mode(void) { return mode; }
bool theme_is_dark(void) { return g_theme->dark; }

bool theme_tick(int32_t hour)
{
    if (mode != THEME_AUTO)
        return false;

    bool soll = theme_dark_for_hour(hour);

    if (soll == g_theme->dark)
        return false;

    apply(soll);
    return true;
}

const char *theme_mode_name(enum theme_mode m)
{
    switch (m) {
    case THEME_DUNKEL: return "Dunkel";
    case THEME_AUTO:   return "Automatisch";
    default:           return "Hell";
    }
}

const char *theme_mode_key(enum theme_mode m)
{
    switch (m) {
    case THEME_DUNKEL: return "dunkel";
    case THEME_AUTO:   return "automatisch";
    default:           return "hell";
    }
}

bool theme_mode_parse(const char *text, enum theme_mode *out)
{
    if (!text || !out)
        return false;

    if (strcmp(text, "hell") == 0)        { *out = THEME_HELL;   return true; }
    if (strcmp(text, "dunkel") == 0)      { *out = THEME_DUNKEL; return true; }
    if (strcmp(text, "automatisch") == 0) { *out = THEME_AUTO;   return true; }
    return false;
}

uint32_t theme_shade(uint32_t color, int32_t percent)
{
    uint32_t out = 0;

    if (percent < 0)
        percent = 0;

    for (int shift = 0; shift <= 16; shift += 8) {
        int32_t v = (int32_t)((color >> shift) & 0xFF) * percent / 100;
        out |= (uint32_t)MIN(v, 255) << shift;
    }
    return out;
}
