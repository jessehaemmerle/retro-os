/* gfx.h - Zeichenprimitive auf einer Pixelflaeche. */
#ifndef GFX_H
#define GFX_H

#include "retro.h"

#define RGB(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

struct rect {
    int32_t x, y, w, h;
};

struct canvas {
    uint32_t   *px;
    int32_t     w, h;
    int32_t     stride;     /* Pixel je Zeile */
    struct rect clip;       /* Zeichnungen ausserhalb werden verworfen */
};

/* --- Rechtecke --- */
static inline struct rect rect_make(int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct rect r = { x, y, w, h };
    return r;
}

bool rect_contains(struct rect r, int32_t x, int32_t y);
bool rect_intersects(struct rect a, struct rect b);
struct rect rect_intersect(struct rect a, struct rect b);

/* --- Bildschirm --- */
bool gfx_init(void);
struct canvas *gfx_screen(void);       /* Backbuffer                        */
void gfx_flush(void);                  /* Backbuffer -> Framebuffer         */
void gfx_flush_rect(struct rect r);

/* --- Clipping --- */
void gfx_set_clip(struct canvas *c, struct rect r);
void gfx_reset_clip(struct canvas *c);

/* --- Primitive --- */
void gfx_pixel(struct canvas *c, int32_t x, int32_t y, uint32_t color);
/* Mischt einen Punkt ein. Die oberen acht Bit sagen, wie deckend er
 * ist: 0 laesst den Untergrund stehen, 255 ersetzt ihn. Dazwischen
 * wird gemischt - das brauchen die weichen Kanten der Symbole. */
void gfx_blend(struct canvas *c, int32_t x, int32_t y, uint32_t argb);
void gfx_fill(struct canvas *c, struct rect r, uint32_t color);
/* Wie gfx_fill, aber die oberen acht Bit sagen, wie deckend es wird. */
void gfx_fill_blend(struct canvas *c, struct rect r, uint32_t argb);
void gfx_frame(struct canvas *c, struct rect r, uint32_t color);
void gfx_hline(struct canvas *c, int32_t x, int32_t y, int32_t len, uint32_t color);
void gfx_vline(struct canvas *c, int32_t x, int32_t y, int32_t len, uint32_t color);
void gfx_line(struct canvas *c, int32_t x0, int32_t y0,
              int32_t x1, int32_t y1, uint32_t color);
void gfx_gradient_v(struct canvas *c, struct rect r, uint32_t top, uint32_t bottom);
void gfx_gradient_h(struct canvas *c, struct rect r, uint32_t left, uint32_t right);

/* Klassischer 3D-Rahmen: raised = herausstehend, sonst eingelassen. */
void gfx_bevel(struct canvas *c, struct rect r, bool raised);
void gfx_bevel_thin(struct canvas *c, struct rect r, bool raised);

/* --- Text --- */
void gfx_char(struct canvas *c, int32_t x, int32_t y, unsigned char ch,
              uint32_t color, bool bold);
void gfx_text(struct canvas *c, int32_t x, int32_t y, const char *s, uint32_t color);
void gfx_text_bold(struct canvas *c, int32_t x, int32_t y, const char *s, uint32_t color);
/* Text, der bei max_w Pixeln mit "..." abgeschnitten wird. */
void gfx_text_clipped(struct canvas *c, int32_t x, int32_t y, const char *s,
                      uint32_t color, int32_t max_w);
int32_t gfx_text_width(const char *s);

/* Vergroesserte Schrift fuer Ueberschriften - jedes Pixel wird zum Quadrat. */
void gfx_text_scaled(struct canvas *c, int32_t x, int32_t y, const char *s,
                     uint32_t color, int32_t scale, bool bold);
int32_t gfx_text_width_scaled(const char *s, int32_t scale);

/* Frei skalierte Schrift: cell_w mal cell_h Pixel je Zeichen. Die
 * Bitmapschrift wird dabei abgetastet, kursiv entsteht durch Scheren. */
void gfx_glyph_sized(struct canvas *c, int32_t x, int32_t y, unsigned char ch,
                     uint32_t color, int32_t cell_w, int32_t cell_h,
                     bool bold, bool italic);
/* Zeichenbreite ist size/2, Zeilenhoehe size; tracking ist der
 * zusaetzliche Abstand zwischen den Zeichen. */
void gfx_text_sized(struct canvas *c, int32_t x, int32_t y, const char *s,
                    uint32_t color, int32_t size, bool bold, bool italic,
                    int32_t tracking);
int32_t gfx_text_width_sized(const char *s, int32_t size, int32_t tracking);

/* --- Flaechen kopieren --- */
void gfx_blit(struct canvas *dst, int32_t x, int32_t y, const struct canvas *src);

#endif /* GFX_H */
