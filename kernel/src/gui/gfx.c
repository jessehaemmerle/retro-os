/* gfx.c - alle Zeichenoperationen von RetroOS.
 *
 * Gezeichnet wird immer in einen Backbuffer im normalen Arbeitsspeicher.
 * Der Framebuffer einer Grafikkarte liegt haeufig in nicht gecachtem
 * Speicher, dort waere jedes einzelne Pixel teuer. gfx_flush() kopiert das
 * fertige Bild anschliessend in einem Rutsch.
 */

#include "gfx.h"
#include "fb.h"
#include "font.h"
#include "kstring.h"
#include "mm.h"

static struct canvas screen;

bool rect_contains(struct rect r, int32_t x, int32_t y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

bool rect_intersects(struct rect a, struct rect b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

struct rect rect_intersect(struct rect a, struct rect b)
{
    int32_t x0 = MAX(a.x, b.x);
    int32_t y0 = MAX(a.y, b.y);
    int32_t x1 = MIN(a.x + a.w, b.x + b.w);
    int32_t y1 = MIN(a.y + a.h, b.y + b.h);

    if (x1 <= x0 || y1 <= y0)
        return rect_make(0, 0, 0, 0);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

bool gfx_init(void)
{
    size_t pixels = (size_t)g_fb.width * g_fb.height;
    uint64_t phys = pmm_alloc_pages(ALIGN_UP(pixels * 4, PAGE_SIZE) / PAGE_SIZE);

    if (!phys)
        return false;

    screen.px     = phys_to_virt(phys);
    screen.w      = (int32_t)g_fb.width;
    screen.h      = (int32_t)g_fb.height;
    screen.stride = (int32_t)g_fb.width;
    gfx_reset_clip(&screen);

    memset32(screen.px, 0, pixels);
    return true;
}

struct canvas *gfx_screen(void)
{
    return &screen;
}

void gfx_flush(void)
{
    fb_present(screen.px, 0, 0, (uint32_t)screen.w, (uint32_t)screen.h);
}

void gfx_flush_rect(struct rect r)
{
    r = rect_intersect(r, rect_make(0, 0, screen.w, screen.h));
    if (r.w <= 0 || r.h <= 0)
        return;

    fb_present(screen.px, (uint32_t)r.x, (uint32_t)r.y,
               (uint32_t)r.w, (uint32_t)r.h);
}

void gfx_set_clip(struct canvas *c, struct rect r)
{
    c->clip = rect_intersect(r, rect_make(0, 0, c->w, c->h));
}

void gfx_reset_clip(struct canvas *c)
{
    c->clip = rect_make(0, 0, c->w, c->h);
}

void gfx_pixel(struct canvas *c, int32_t x, int32_t y, uint32_t color)
{
    if (!rect_contains(c->clip, x, y))
        return;
    c->px[y * c->stride + x] = color;
}

void gfx_fill(struct canvas *c, struct rect r, uint32_t color)
{
    r = rect_intersect(r, c->clip);

    for (int32_t y = 0; y < r.h; y++)
        memset32(&c->px[(r.y + y) * c->stride + r.x], color, (size_t)r.w);
}

void gfx_hline(struct canvas *c, int32_t x, int32_t y, int32_t len, uint32_t color)
{
    gfx_fill(c, rect_make(x, y, len, 1), color);
}

void gfx_vline(struct canvas *c, int32_t x, int32_t y, int32_t len, uint32_t color)
{
    gfx_fill(c, rect_make(x, y, 1, len), color);
}

void gfx_frame(struct canvas *c, struct rect r, uint32_t color)
{
    gfx_hline(c, r.x, r.y, r.w, color);
    gfx_hline(c, r.x, r.y + r.h - 1, r.w, color);
    gfx_vline(c, r.x, r.y, r.h, color);
    gfx_vline(c, r.x + r.w - 1, r.y, r.h, color);
}

void gfx_line(struct canvas *c, int32_t x0, int32_t y0,
              int32_t x1, int32_t y1, uint32_t color)
{
    /* Bresenham, ganzzahlig - der Kernel kennt keine Fliesskommazahlen. */
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int32_t dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int32_t sx = x0 < x1 ? 1 : -1;
    int32_t sy = y0 < y1 ? 1 : -1;
    int32_t err = dx - dy;

    for (;;) {
        gfx_pixel(c, x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;

        int32_t e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Lineare Interpolation zwischen zwei Farben, kanalweise. */
static uint32_t blend(uint32_t a, uint32_t b, int32_t num, int32_t den)
{
    if (den <= 0)
        return a;

    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        int32_t ca = (int32_t)((a >> shift) & 0xFF);
        int32_t cb = (int32_t)((b >> shift) & 0xFF);
        int32_t cv = ca + ((cb - ca) * num) / den;
        out |= (uint32_t)(cv & 0xFF) << shift;
    }
    return out;
}

void gfx_gradient_v(struct canvas *c, struct rect r, uint32_t top, uint32_t bottom)
{
    for (int32_t y = 0; y < r.h; y++)
        gfx_hline(c, r.x, r.y + y, r.w, blend(top, bottom, y, r.h - 1));
}

void gfx_gradient_h(struct canvas *c, struct rect r, uint32_t left, uint32_t right)
{
    for (int32_t x = 0; x < r.w; x++)
        gfx_vline(c, r.x + x, r.y, r.h, blend(left, right, x, r.w - 1));
}

/* Die typische Doppelkante: aussen hart, innen weich. */
void gfx_bevel(struct canvas *c, struct rect r, bool raised)
{
    uint32_t tl_out = raised ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x86, 0x86, 0x86);
    uint32_t tl_in  = raised ? RGB(0xE0, 0xE0, 0xE0) : RGB(0x3A, 0x3A, 0x3A);
    uint32_t br_out = raised ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xFF, 0xFF, 0xFF);
    uint32_t br_in  = raised ? RGB(0x86, 0x86, 0x86) : RGB(0xE0, 0xE0, 0xE0);

    gfx_hline(c, r.x, r.y, r.w, tl_out);
    gfx_vline(c, r.x, r.y, r.h, tl_out);
    gfx_hline(c, r.x + 1, r.y + 1, r.w - 2, tl_in);
    gfx_vline(c, r.x + 1, r.y + 1, r.h - 2, tl_in);

    gfx_hline(c, r.x, r.y + r.h - 1, r.w, br_out);
    gfx_vline(c, r.x + r.w - 1, r.y, r.h, br_out);
    gfx_hline(c, r.x + 1, r.y + r.h - 2, r.w - 2, br_in);
    gfx_vline(c, r.x + r.w - 2, r.y + 1, r.h - 2, br_in);
}

void gfx_bevel_thin(struct canvas *c, struct rect r, bool raised)
{
    uint32_t tl = raised ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x86, 0x86, 0x86);
    uint32_t br = raised ? RGB(0x86, 0x86, 0x86) : RGB(0xFF, 0xFF, 0xFF);

    gfx_hline(c, r.x, r.y, r.w, tl);
    gfx_vline(c, r.x, r.y, r.h, tl);
    gfx_hline(c, r.x, r.y + r.h - 1, r.w, br);
    gfx_vline(c, r.x + r.w - 1, r.y, r.h, br);
}

void gfx_char(struct canvas *c, int32_t x, int32_t y, unsigned char ch,
              uint32_t color, bool bold)
{
    const uint8_t *glyph = font_glyph(ch);

    for (int32_t row = 0; row < FONT_HEIGHT; row++) {
        uint32_t bits = glyph[row];

        /* Fett wird durch ein um ein Pixel versetztes Zweitbild erzeugt. */
        if (bold)
            bits |= bits >> 1;

        if (!bits)
            continue;

        for (int32_t col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col))
                gfx_pixel(c, x + col, y + row, color);
        }
    }
}

void gfx_text(struct canvas *c, int32_t x, int32_t y, const char *s, uint32_t color)
{
    for (; *s; s++, x += FONT_WIDTH)
        gfx_char(c, x, y, (unsigned char)*s, color, false);
}

void gfx_text_bold(struct canvas *c, int32_t x, int32_t y, const char *s, uint32_t color)
{
    for (; *s; s++, x += FONT_WIDTH)
        gfx_char(c, x, y, (unsigned char)*s, color, true);
}

int32_t gfx_text_width(const char *s)
{
    return (int32_t)strlen(s) * FONT_WIDTH;
}

void gfx_text_clipped(struct canvas *c, int32_t x, int32_t y, const char *s,
                      uint32_t color, int32_t max_w)
{
    int32_t width = gfx_text_width(s);

    if (width <= max_w) {
        gfx_text(c, x, y, s, color);
        return;
    }

    int32_t room = (max_w - 3 * FONT_WIDTH) / FONT_WIDTH;
    if (room < 1) {
        gfx_set_clip(c, rect_intersect(c->clip, rect_make(x, y, max_w, FONT_HEIGHT)));
        gfx_text(c, x, y, s, color);
        gfx_reset_clip(c);
        return;
    }

    for (int32_t i = 0; i < room; i++)
        gfx_char(c, x + i * FONT_WIDTH, y, (unsigned char)s[i], color, false);
    gfx_text(c, x + room * FONT_WIDTH, y, "...", color);
}

void gfx_blit(struct canvas *dst, int32_t x, int32_t y, const struct canvas *src)
{
    struct rect area = rect_intersect(rect_make(x, y, src->w, src->h), dst->clip);

    for (int32_t row = 0; row < area.h; row++) {
        int32_t sy = area.y - y + row;
        int32_t sx = area.x - x;

        memcpy(&dst->px[(area.y + row) * dst->stride + area.x],
               &src->px[sy * src->stride + sx],
               (size_t)area.w * 4);
    }
}
