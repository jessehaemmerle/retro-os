/* displayutil.c - siehe displayutil.h. */

#include "displayutil.h"
#include "kstring.h"

/* Die Modi, die praktisch jede Grafikkarte kann und die auf einem
 * Bildschirm auch etwas taugen. Mehr waere eine Liste, durch die
 * niemand mehr blaettern will.
 *
 * Geordnet nach Flaeche und nicht nach Breite: Durch die Liste wird
 * geblaettert, und "eine Stufe weiter" soll heissen, dass mehr auf den
 * Schirm passt - nicht, dass er breiter und dabei niedriger wird. Eine
 * Pruefung im Testlauf besteht darauf. */
static const struct disp_mode standard[] = {
    {  800,  600 },
    { 1024,  768 },
    { 1280,  720 },
    { 1152,  864 },
    { 1280,  800 },
    { 1440,  900 },
    { 1280, 1024 },
    { 1600,  900 },
    { 1680, 1050 },
    { 1920, 1080 },
    { 1920, 1200 },
    { 2560, 1440 },
};

size_t disp_standard_modes(struct disp_mode *out, size_t max)
{
    size_t n = 0;

    for (size_t i = 0; i < ARRAY_LEN(standard) && n < max; i++)
        out[n++] = standard[i];
    return n;
}

static bool digits(const char **p, int32_t *out)
{
    int32_t value = 0;
    int      count = 0;

    while (**p >= '0' && **p <= '9') {
        /* Vier Stellen reichen bis 9999 - was darueber steht, ist ein
         * Tippfehler und kein Bildschirm. */
        if (++count > 4)
            return false;
        value = value * 10 + (*(*p)++ - '0');
    }
    if (!count)
        return false;
    *out = value;
    return true;
}

bool disp_parse_mode(const char *text, int32_t *w, int32_t *h)
{
    if (!text || !w || !h)
        return false;

    while (*text == ' ')
        text++;

    int32_t width, height, depth;

    if (!digits(&text, &width))
        return false;
    if (*text != 'x' && *text != 'X')
        return false;
    text++;
    if (!digits(&text, &height))
        return false;

    /* Eine angehaengte Farbtiefe wird geschluckt, aber nicht benutzt. */
    if (*text == 'x' || *text == 'X') {
        text++;
        if (!digits(&text, &depth))
            return false;
    }

    while (*text == ' ')
        text++;
    if (*text)
        return false;

    if (width < 640 || height < 400 || width > 4096 || height > 4096)
        return false;

    *w = width;
    *h = height;
    return true;
}

void disp_format_mode(char *out, size_t size, int32_t w, int32_t h)
{
    ksnprintf(out, size, "%dx%d", (int)w, (int)h);
}

uint32_t disp_max_scale(int32_t w, int32_t h)
{
    uint32_t scale = 1;

    /* Solange nach dem Teilen noch eine brauchbare Flaeche bleibt,
     * darf es eine Stufe mehr sein. */
    while (scale < 4 && w / (int32_t)(scale + 1) >= 640 &&
           h / (int32_t)(scale + 1) >= 400)
        scale++;
    return scale;
}

uint32_t disp_auto_scale(int32_t w, int32_t h)
{
    uint32_t wish = 1;

    /* Die Grenzen sind die ueblichen Sprungstellen: ab 4K wird
     * dreifach vergroessert, ab etwa 1440 Zeilen doppelt. */
    if (h >= 2000)
        wish = 3;
    else if (h >= 1400)
        wish = 2;

    uint32_t possible = disp_max_scale(w, h);

    return wish < possible ? wish : possible;
}

bool disp_mode_fits(int32_t w, int32_t h, uint64_t vram_bytes)
{
    if (w <= 0 || h <= 0)
        return false;
    return (uint64_t)w * (uint64_t)h * 4u <= vram_bytes;
}

void disp_fit_window(int32_t *x, int32_t *y, int32_t *w, int32_t *h,
                     int32_t screen_w, int32_t screen_h)
{
    if (*w > screen_w)
        *w = screen_w;
    if (*h > screen_h)
        *h = screen_h;

    if (*x + *w > screen_w)
        *x = screen_w - *w;
    if (*y + *h > screen_h)
        *y = screen_h - *h;

    /* Erst danach nach links oben begrenzen: Ein Fenster, das breiter
     * ist als der Bildschirm, soll an dessen linkem Rand anfangen und
     * nicht an einer negativen Stelle. */
    if (*x < 0)
        *x = 0;
    if (*y < 0)
        *y = 0;
}

/* --- Schadensverfolgung --------------------------------------------- */

/* Vom Kachelgitter zurueck auf Punkte, beschnitten auf den Bildschirm. */
static struct rect tile_rect(int32_t tx, int32_t ty, int32_t tw, int32_t th,
                             int32_t tile_w, int32_t tile_h,
                             int32_t width, int32_t height)
{
    int32_t x0 = tx * tile_w;
    int32_t y0 = ty * tile_h;
    int32_t x1 = MIN((tx + tw) * tile_w, width);
    int32_t y1 = MIN((ty + th) * tile_h, height);

    if (x1 <= x0 || y1 <= y0)
        return rect_make(0, 0, 0, 0);
    return rect_make(x0, y0, x1 - x0, y1 - y0);
}

/* Alles, was noch gesetzt ist, in ein einziges Rechteck - der Ausweg,
 * wenn die Liste voll ist. */
static struct rect remaining_rect(uint8_t *tiles, int32_t cols, int32_t rows,
                                  int32_t tile_w, int32_t tile_h,
                                  int32_t width, int32_t height)
{
    int32_t x0 = cols, y0 = rows, x1 = 0, y1 = 0;

    for (int32_t ty = 0; ty < rows; ty++) {
        for (int32_t tx = 0; tx < cols; tx++) {
            if (!tiles[ty * cols + tx])
                continue;

            tiles[ty * cols + tx] = 0;
            if (tx < x0) x0 = tx;
            if (ty < y0) y0 = ty;
            if (tx + 1 > x1) x1 = tx + 1;
            if (ty + 1 > y1) y1 = ty + 1;
        }
    }

    if (x1 <= x0 || y1 <= y0)
        return rect_make(0, 0, 0, 0);
    return tile_rect(x0, y0, x1 - x0, y1 - y0, tile_w, tile_h, width, height);
}

size_t disp_damage_rects(uint8_t *tiles, int32_t cols, int32_t rows,
                         int32_t tile_w, int32_t tile_h,
                         int32_t width, int32_t height,
                         struct rect *out, size_t max)
{
    size_t n = 0;

    if (!tiles || !out || max == 0 || cols <= 0 || rows <= 0)
        return 0;
    if (tile_w <= 0 || tile_h <= 0)
        return 0;

    for (int32_t ty = 0; ty < rows; ty++) {
        for (int32_t tx = 0; tx < cols; tx++) {
            if (!tiles[ty * cols + tx])
                continue;

            /* Der letzte Platz gehoert dem Rest. */
            if (n + 1 == max) {
                struct rect r = remaining_rect(tiles, cols, rows, tile_w,
                                               tile_h, width, height);

                if (r.w > 0 && r.h > 0)
                    out[n++] = r;
                return n;
            }

            int32_t tw = 1;
            while (tx + tw < cols && tiles[ty * cols + tx + tw])
                tw++;

            int32_t th = 1;
            while (ty + th < rows) {
                bool full = true;

                for (int32_t i = 0; i < tw; i++) {
                    if (!tiles[(ty + th) * cols + tx + i]) {
                        full = false;
                        break;
                    }
                }
                if (!full)
                    break;
                th++;
            }

            for (int32_t r = 0; r < th; r++) {
                for (int32_t i = 0; i < tw; i++)
                    tiles[(ty + r) * cols + tx + i] = 0;
            }

            struct rect r = tile_rect(tx, ty, tw, th, tile_w, tile_h,
                                      width, height);

            if (r.w > 0 && r.h > 0)
                out[n++] = r;
        }
    }
    return n;
}
