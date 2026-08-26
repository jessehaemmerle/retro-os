/* image.c - Formaterkennung, Skalierung und Zeichnen von Bildern. */

#include "image.h"
#include "kstring.h"
#include "mm.h"

/* ------------------------------------------------------------------ */
/* BMP - schlicht, aber im Netz noch anzutreffen                       */
/* ------------------------------------------------------------------ */

bool image_decode_bmp(const uint8_t *data, size_t length, struct image *out)
{
    if (length < 54 || data[0] != 'B' || data[1] != 'M')
        return false;

    uint32_t offset = (uint32_t)(data[10] | (data[11] << 8) |
                                 (data[12] << 16) | ((uint32_t)data[13] << 24));
    int32_t width = (int32_t)(data[18] | (data[19] << 8) |
                              (data[20] << 16) | ((uint32_t)data[21] << 24));
    int32_t height = (int32_t)(data[22] | (data[23] << 8) |
                               (data[24] << 16) | ((uint32_t)data[25] << 24));
    uint16_t bpp = (uint16_t)(data[28] | (data[29] << 8));
    uint32_t compression = (uint32_t)(data[30] | (data[31] << 8) |
                                      (data[32] << 16) |
                                      ((uint32_t)data[33] << 24));
    bool bottom_up = height > 0;

    if (height < 0)
        height = -height;
    if (compression != 0 || width <= 0 || height <= 0 ||
        width > 8192 || height > 8192)
        return false;
    if (bpp != 24 && bpp != 32)
        return false;

    size_t stride = ((size_t)width * (bpp / 8) + 3) & ~(size_t)3;

    if ((size_t)offset + stride * height > length)
        return false;

    out->w = width;
    out->h = height;
    out->px = kmalloc((size_t)width * height * 4);
    if (!out->px)
        return false;

    for (int32_t y = 0; y < height; y++) {
        const uint8_t *row = data + offset +
                             stride * (bottom_up ? (height - 1 - y) : y);

        for (int32_t x = 0; x < width; x++) {
            const uint8_t *p = row + (size_t)x * (bpp / 8);
            uint32_t alpha = bpp == 32 ? p[3] : 255;

            if (bpp == 32 && alpha == 0)
                alpha = 255;    /* viele Erzeuger lassen den Kanal leer */
            out->px[(size_t)y * width + x] =
                (alpha << 24) | ((uint32_t)p[2] << 16) |
                ((uint32_t)p[1] << 8) | p[0];
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Gemeinsames                                                         */
/* ------------------------------------------------------------------ */

const char *image_format(const uint8_t *data, size_t length)
{
    if (length >= 8 && data[0] == 137 && memcmp(data + 1, "PNG", 3) == 0)
        return "PNG";
    if (length >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return "JPEG";
    if (length >= 6 && memcmp(data, "GIF8", 4) == 0)
        return "GIF";
    if (length >= 2 && data[0] == 'B' && data[1] == 'M')
        return "BMP";
    if (length >= 4 && memcmp(data, "<svg", 4) == 0)
        return "SVG";
    return NULL;
}

bool image_decode(const uint8_t *data, size_t length, struct image *out)
{
    const char *format = image_format(data, length);

    memset(out, 0, sizeof(*out));
    if (!format)
        return false;

    if (strcmp(format, "PNG") == 0)
        return image_decode_png(data, length, out);
    if (strcmp(format, "JPEG") == 0)
        return image_decode_jpeg(data, length, out);
    if (strcmp(format, "GIF") == 0)
        return image_decode_gif(data, length, out);
    if (strcmp(format, "BMP") == 0)
        return image_decode_bmp(data, length, out);
    return false;
}

void image_free(struct image *img)
{
    if (!img)
        return;
    kfree(img->px);
    img->px = NULL;
    img->w = 0;
    img->h = 0;
}

bool image_scale(const struct image *src, int32_t w, int32_t h,
                 struct image *out)
{
    memset(out, 0, sizeof(*out));
    if (!src->px || src->w <= 0 || src->h <= 0 || w <= 0 || h <= 0)
        return false;
    if (w > 4096 || h > 4096)
        return false;

    out->px = kmalloc((size_t)w * h * 4);
    if (!out->px)
        return false;
    out->w = w;
    out->h = h;

    /* Beim Verkleinern wird ueber den zugehoerigen Quellbereich
     * gemittelt, sonst wuerden feine Bilder zu Rauschen. */
    bool shrink = w < src->w || h < src->h;

    for (int32_t y = 0; y < h; y++) {
        int32_t sy0 = (int32_t)((int64_t)y * src->h / h);
        int32_t sy1 = (int32_t)((int64_t)(y + 1) * src->h / h);

        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        sy1 = MIN(sy1, src->h);

        for (int32_t x = 0; x < w; x++) {
            int32_t sx0 = (int32_t)((int64_t)x * src->w / w);
            int32_t sx1 = (int32_t)((int64_t)(x + 1) * src->w / w);

            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            sx1 = MIN(sx1, src->w);

            if (!shrink) {
                out->px[(size_t)y * w + x] =
                    src->px[(size_t)sy0 * src->w + sx0];
                continue;
            }

            uint32_t a = 0, r = 0, g = 0, b = 0, n = 0;

            for (int32_t sy = sy0; sy < sy1; sy++) {
                for (int32_t sx = sx0; sx < sx1; sx++) {
                    uint32_t p = src->px[(size_t)sy * src->w + sx];

                    a += (p >> 24) & 0xFF;
                    r += (p >> 16) & 0xFF;
                    g += (p >> 8) & 0xFF;
                    b += p & 0xFF;
                    n++;
                }
            }
            if (n == 0)
                n = 1;
            out->px[(size_t)y * w + x] = ((a / n) << 24) | ((r / n) << 16) |
                                         ((g / n) << 8) | (b / n);
        }
    }
    return true;
}

void image_draw(struct canvas *c, int32_t x, int32_t y, const struct image *img)
{
    if (!c || !img || !img->px)
        return;

    struct rect area = rect_intersect(c->clip,
                                      rect_make(x, y, img->w, img->h));

    for (int32_t py = area.y; py < area.y + area.h; py++) {
        if (py < 0 || py >= c->h)
            continue;
        for (int32_t px = area.x; px < area.x + area.w; px++) {
            if (px < 0 || px >= c->w)
                continue;

            uint32_t source = img->px[(size_t)(py - y) * img->w + (px - x)];
            uint32_t alpha = source >> 24;

            if (alpha == 0)
                continue;

            uint32_t *target = &c->px[(size_t)py * c->stride + px];

            if (alpha == 255) {
                *target = source & 0x00FFFFFFu;
                continue;
            }

            uint32_t back = *target;
            uint32_t inverse = 255 - alpha;
            uint32_t r = ((((source >> 16) & 0xFF) * alpha) +
                          (((back >> 16) & 0xFF) * inverse)) / 255;
            uint32_t g = ((((source >> 8) & 0xFF) * alpha) +
                          (((back >> 8) & 0xFF) * inverse)) / 255;
            uint32_t b = (((source & 0xFF) * alpha) +
                          ((back & 0xFF) * inverse)) / 255;

            *target = (r << 16) | (g << 8) | b;
        }
    }
}
