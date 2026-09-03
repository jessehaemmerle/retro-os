/* widgets.c - Knoepfe, Eingabefelder, Bildlaufleisten.
 *
 * Alle Elemente zeichnen sich selbst und kennen keinen eigenen Zustand.
 * Wer sie benutzt, haelt den Zustand (gedrueckt, Auswahl, Bildlauf) selbst
 * und ruft beim Neuzeichnen die passende Funktion auf. Das haelt die
 * Anwendungen einfach und den Bildaufbau berechenbar.
 */

#include "widgets.h"
#include "font.h"
#include "kstring.h"
#include "lang.h"
#include "theme.h"

/* Die Beschriftung wird hier uebersetzt und nicht beim Aufrufer:
 * Knoepfe gibt es in jedem Fenster, und sie beim Zeichnen zu
 * uebersetzen heisst, dass ein Sprachwechsel sofort durchschlaegt -
 * auch in Fenstern, die schon offen sind. */
void widget_button(struct canvas *c, struct rect r, const char *label,
                   bool pressed, bool enabled)
{
    label = tr(label);
    gfx_fill(c, r, COL_FACE);
    gfx_bevel(c, r, !pressed);

    int32_t off = pressed ? 1 : 0;
    int32_t tw = gfx_text_width(label);
    int32_t tx = r.x + (r.w - tw) / 2 + off;
    int32_t ty = r.y + (r.h - FONT_HEIGHT) / 2 + off;

    if (!enabled) {
        gfx_text(c, tx + 1, ty + 1, label, COL_HILIGHT);
        gfx_text(c, tx, ty, label, COL_SHADOW);
    } else {
        gfx_text(c, tx, ty, label, COL_TEXT);
    }
}

void widget_icon_button(struct canvas *c, struct rect r, enum icon_id icon,
                        const char *label, bool pressed, bool enabled)
{
    label = tr(label);
    gfx_fill(c, r, COL_FACE);
    gfx_bevel(c, r, !pressed);

    int32_t off = pressed ? 1 : 0;

    if (label && label[0]) {
        icon_draw(c, r.x + 5 + off, r.y + (r.h - ICON_SIZE) / 2 + off, icon, 1);
        gfx_text(c, r.x + 25 + off, r.y + (r.h - FONT_HEIGHT) / 2 + off,
                 label, enabled ? COL_TEXT : COL_SHADOW);
    } else {
        icon_draw(c, r.x + (r.w - ICON_SIZE) / 2 + off,
                  r.y + (r.h - ICON_SIZE) / 2 + off, icon, 1);
    }
}

void widget_field(struct canvas *c, struct rect r, const char *text,
                  int32_t cursor, bool focused)
{
    gfx_fill(c, r, COL_FIELD);
    gfx_bevel(c, r, false);

    struct rect inner = rect_make(r.x + 3, r.y + 3, r.w - 6, r.h - 6);
    struct canvas clipped = *c;
    gfx_set_clip(&clipped, rect_intersect(c->clip, inner));

    /* Bei langen Eingaben mitscrollen, damit die Schreibmarke sichtbar bleibt. */
    int32_t visible_chars = inner.w / FONT_WIDTH;
    int32_t len = (int32_t)strlen(text);
    int32_t first = 0;

    if (cursor >= 0 && cursor > visible_chars - 1)
        first = cursor - visible_chars + 1;
    if (first > len)
        first = len;

    int32_t ty = r.y + (r.h - FONT_HEIGHT) / 2;
    gfx_text(&clipped, inner.x - first * FONT_WIDTH, ty, text, COL_TEXT);

    if (focused && cursor >= 0) {
        int32_t cx = inner.x + (cursor - first) * FONT_WIDTH;
        gfx_fill(&clipped, rect_make(cx, ty, 1, FONT_HEIGHT), COL_TEXT);
    }
}

void widget_vscroll(struct canvas *c, struct rect r,
                    int32_t offset, int32_t total, int32_t visible)
{
    gfx_fill(c, r, RGB(0xDC, 0xDC, 0xDC));

    struct rect up   = rect_make(r.x, r.y, r.w, SCROLLBAR_WIDTH);
    struct rect down = rect_make(r.x, r.y + r.h - SCROLLBAR_WIDTH, r.w, SCROLLBAR_WIDTH);

    gfx_fill(c, up, COL_FACE);
    gfx_bevel(c, up, true);
    gfx_fill(c, down, COL_FACE);
    gfx_bevel(c, down, true);

    /* Pfeilspitzen */
    int32_t cx = r.x + r.w / 2;
    for (int32_t i = 0; i < 4; i++) {
        gfx_hline(c, cx - i, up.y + 9 - i, i * 2 + 1, COL_TEXT);
        gfx_hline(c, cx - i, down.y + 6 + i, i * 2 + 1, COL_TEXT);
    }

    if (total <= visible)
        return;

    int32_t track_y = r.y + SCROLLBAR_WIDTH;
    int32_t track_h = r.h - 2 * SCROLLBAR_WIDTH;
    if (track_h < 16)
        return;

    int32_t thumb_h = MAX(track_h * visible / total, 16);
    int32_t span    = track_h - thumb_h;
    int32_t max_off = total - visible;
    int32_t thumb_y = track_y + (max_off > 0 ? span * offset / max_off : 0);

    struct rect thumb = rect_make(r.x, thumb_y, r.w, thumb_h);
    gfx_fill(c, thumb, COL_FACE);
    gfx_bevel(c, thumb, true);
}

int32_t widget_vscroll_click(struct rect r, int32_t y,
                             int32_t offset, int32_t total, int32_t visible)
{
    int32_t max_off = MAX(total - visible, 0);

    if (y < r.y + SCROLLBAR_WIDTH)
        return MAX(offset - 1, 0);
    if (y >= r.y + r.h - SCROLLBAR_WIDTH)
        return MIN(offset + 1, max_off);

    int32_t track_y = r.y + SCROLLBAR_WIDTH;
    int32_t track_h = r.h - 2 * SCROLLBAR_WIDTH;

    if (track_h <= 0 || max_off == 0)
        return offset;

    int32_t pos = ((y - track_y) * total) / track_h - visible / 2;
    return CLAMP(pos, 0, max_off);
}

void widget_statusbar(struct canvas *c, struct rect r, const char *left,
                      const char *right)
{
    left = tr(left);
    right = tr(right);
    gfx_fill(c, r, COL_FACE);
    gfx_hline(c, r.x, r.y, r.w, COL_HILIGHT);

    if (left && left[0]) {
        struct rect panel = rect_make(r.x + 2, r.y + 3, r.w - 150, r.h - 5);
        gfx_bevel_thin(c, panel, false);
        struct canvas clipped = *c;
        gfx_set_clip(&clipped, rect_intersect(c->clip, panel));
        gfx_text(&clipped, panel.x + 4, panel.y + 3, left, COL_TEXT);
    }

    if (right && right[0]) {
        struct rect panel = rect_make(r.x + r.w - 146, r.y + 3, 144, r.h - 5);
        gfx_bevel_thin(c, panel, false);
        struct canvas clipped = *c;
        gfx_set_clip(&clipped, rect_intersect(c->clip, panel));
        gfx_text(&clipped, panel.x + 4, panel.y + 3, right, COL_TEXT);
    }
}

void widget_toolbar(struct canvas *c, struct rect r)
{
    gfx_fill(c, r, COL_FACE);
    gfx_hline(c, r.x, r.y + r.h - 1, r.w, COL_SHADOW);
    gfx_hline(c, r.x, r.y + r.h, r.w, COL_HILIGHT);
}
