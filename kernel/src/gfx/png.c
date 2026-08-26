/* png.c - Leser fuer PNG-Bilder (ISO 15948).
 *
 * Unterstuetzt werden alle Farbtypen mit 8 und 16 Bit je Kanal sowie
 * Paletten mit 1, 2, 4 und 8 Bit, dazu Adam7-Verschraenkung und
 * einfache Durchsichtigkeit ueber tRNS.
 */

#include "image.h"
#include "inflate.h"
#include "kstring.h"
#include "mm.h"

#define PNG_GRAY       0
#define PNG_RGB        2
#define PNG_PALETTE    3
#define PNG_GRAY_ALPHA 4
#define PNG_RGBA       6

struct png_header {
    uint32_t width, height;
    uint8_t  depth, color, compression, filter, interlace;
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint8_t channels_of(uint8_t color)
{
    switch (color) {
    case PNG_GRAY:        return 1;
    case PNG_RGB:         return 3;
    case PNG_PALETTE:     return 1;
    case PNG_GRAY_ALPHA:  return 2;
    case PNG_RGBA:        return 4;
    default:              return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Rueckrechnen der Zeilenfilter                                       */
/* ------------------------------------------------------------------ */

static uint8_t paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;

    if (pa <= pb && pa <= pc)
        return (uint8_t)a;
    if (pb <= pc)
        return (uint8_t)b;
    return (uint8_t)c;
}

/* Entfiltert eine Zeile an Ort und Stelle. previous darf NULL sein. */
static bool unfilter(uint8_t filter, uint8_t *row, const uint8_t *previous,
                     size_t bytes, size_t bpp)
{
    switch (filter) {
    case 0:
        break;
    case 1:
        for (size_t i = bpp; i < bytes; i++)
            row[i] = (uint8_t)(row[i] + row[i - bpp]);
        break;
    case 2:
        if (previous)
            for (size_t i = 0; i < bytes; i++)
                row[i] = (uint8_t)(row[i] + previous[i]);
        break;
    case 3:
        for (size_t i = 0; i < bytes; i++) {
            int left = i >= bpp ? row[i - bpp] : 0;
            int up = previous ? previous[i] : 0;

            row[i] = (uint8_t)(row[i] + ((left + up) >> 1));
        }
        break;
    case 4:
        for (size_t i = 0; i < bytes; i++) {
            int left = i >= bpp ? row[i - bpp] : 0;
            int up = previous ? previous[i] : 0;
            int corner = (previous && i >= bpp) ? previous[i - bpp] : 0;

            row[i] = (uint8_t)(row[i] + paeth(left, up, corner));
        }
        break;
    default:
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Pixel aus einer entfilterten Zeile holen                            */
/* ------------------------------------------------------------------ */

struct png_palette {
    uint8_t  entry[256][3];
    uint8_t  alpha[256];
    uint16_t count;
    bool     has_alpha;
    bool     has_key;      /* durchsichtige Farbe bei Grau und RGB */
    uint16_t key[3];
};

/* Liest den Wert des Kanals c des Pixels x aus einer Zeile. */
static uint32_t sample(const uint8_t *row, const struct png_header *hd,
                       uint32_t x, uint32_t c, uint8_t nchannels)
{
    uint32_t index = x * nchannels + c;

    switch (hd->depth) {
    case 16:
        return ((uint32_t)row[index * 2] << 8) | row[index * 2 + 1];
    case 8:
        return row[index];
    default: {
        uint32_t per_byte = 8 / hd->depth;
        uint8_t  byte = row[index / per_byte];
        uint32_t shift = (per_byte - 1 - (index % per_byte)) * hd->depth;

        return (byte >> shift) & ((1u << hd->depth) - 1u);
    }
    }
}

/* Streckt einen Wert der Tiefe depth auf volle 8 Bit. */
static uint8_t to_byte(uint32_t value, uint8_t depth)
{
    switch (depth) {
    case 16: return (uint8_t)(value >> 8);
    case 8:  return (uint8_t)value;
    case 4:  return (uint8_t)(value * 17);
    case 2:  return (uint8_t)(value * 85);
    default: return value ? 255 : 0;
    }
}

static uint32_t pixel_at(const uint8_t *row, const struct png_header *hd,
                         const struct png_palette *pal, uint32_t x)
{
    uint8_t nch = channels_of(hd->color);
    uint8_t r, g, b, a = 255;

    switch (hd->color) {
    case PNG_GRAY: {
        uint32_t raw = sample(row, hd, x, 0, nch);

        r = g = b = to_byte(raw, hd->depth);
        if (pal->has_key && raw == pal->key[0])
            a = 0;
        break;
    }
    case PNG_RGB: {
        uint32_t rr = sample(row, hd, x, 0, nch);
        uint32_t gg = sample(row, hd, x, 1, nch);
        uint32_t bb = sample(row, hd, x, 2, nch);

        r = to_byte(rr, hd->depth);
        g = to_byte(gg, hd->depth);
        b = to_byte(bb, hd->depth);
        if (pal->has_key && rr == pal->key[0] && gg == pal->key[1] &&
            bb == pal->key[2])
            a = 0;
        break;
    }
    case PNG_PALETTE: {
        uint32_t index = sample(row, hd, x, 0, nch);

        if (index >= pal->count)
            index = 0;
        r = pal->entry[index][0];
        g = pal->entry[index][1];
        b = pal->entry[index][2];
        if (pal->has_alpha)
            a = pal->alpha[index];
        break;
    }
    case PNG_GRAY_ALPHA:
        r = g = b = to_byte(sample(row, hd, x, 0, nch), hd->depth);
        a = to_byte(sample(row, hd, x, 1, nch), hd->depth);
        break;
    default:
        r = to_byte(sample(row, hd, x, 0, nch), hd->depth);
        g = to_byte(sample(row, hd, x, 1, nch), hd->depth);
        b = to_byte(sample(row, hd, x, 2, nch), hd->depth);
        a = to_byte(sample(row, hd, x, 3, nch), hd->depth);
        break;
    }

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
           ((uint32_t)g << 8) | b;
}

/* ------------------------------------------------------------------ */
/* Ein Durchgang: entweder das ganze Bild oder eine Adam7-Stufe        */
/* ------------------------------------------------------------------ */

static const uint8_t adam_x0[7] = { 0, 4, 0, 2, 0, 1, 0 };
static const uint8_t adam_y0[7] = { 0, 0, 4, 0, 2, 0, 1 };
static const uint8_t adam_dx[7] = { 8, 8, 4, 4, 2, 2, 1 };
static const uint8_t adam_dy[7] = { 8, 8, 8, 4, 4, 2, 2 };

static bool read_pass(const uint8_t **cursor, size_t *left,
                      const struct png_header *hd,
                      const struct png_palette *pal, struct image *out,
                      uint32_t x0, uint32_t y0, uint32_t dx, uint32_t dy)
{
    uint32_t cols = (out->w > (int32_t)x0)
                    ? ((uint32_t)out->w - x0 + dx - 1) / dx : 0;
    uint32_t rows = (out->h > (int32_t)y0)
                    ? ((uint32_t)out->h - y0 + dy - 1) / dy : 0;

    if (cols == 0 || rows == 0)
        return true;

    uint8_t nch = channels_of(hd->color);
    size_t  bits_per_pixel = (size_t)nch * hd->depth;
    size_t  bytes = ((size_t)cols * bits_per_pixel + 7) / 8;
    size_t  bpp = (bits_per_pixel + 7) / 8;
    uint8_t *row = kmalloc(bytes);
    uint8_t *previous = kmalloc(bytes);

    if (!row || !previous) {
        kfree(row);
        kfree(previous);
        return false;
    }
    memset(previous, 0, bytes);

    bool first = true;

    for (uint32_t y = 0; y < rows; y++) {
        if (*left < bytes + 1) {
            kfree(row);
            kfree(previous);
            return false;
        }

        uint8_t filter = **cursor;

        memcpy(row, *cursor + 1, bytes);
        *cursor += bytes + 1;
        *left -= bytes + 1;

        if (!unfilter(filter, row, first ? NULL : previous, bytes, bpp)) {
            kfree(row);
            kfree(previous);
            return false;
        }
        first = false;

        uint32_t py = y0 + y * dy;

        for (uint32_t x = 0; x < cols; x++) {
            uint32_t px = x0 + x * dx;

            out->px[(size_t)py * out->w + px] = pixel_at(row, hd, pal, x);
        }
        memcpy(previous, row, bytes);
    }

    kfree(row);
    kfree(previous);
    return true;
}

/* ------------------------------------------------------------------ */
/* Hauptteil                                                           */
/* ------------------------------------------------------------------ */

bool image_decode_png(const uint8_t *data, size_t length, struct image *out)
{
    static const uint8_t magic[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };

    if (length < 8 + 25 || memcmp(data, magic, 8) != 0)
        return false;

    struct png_header hd;
    struct png_palette pal;
    uint8_t *idat = NULL;
    size_t idat_length = 0, idat_capacity = 0;
    bool have_header = false;
    size_t pos = 8;

    memset(&hd, 0, sizeof(hd));
    memset(&pal, 0, sizeof(pal));

    while (pos + 8 <= length) {
        uint32_t chunk = be32(data + pos);
        const uint8_t *type = data + pos + 4;

        if (pos + 12 + (size_t)chunk > length)
            break;

        const uint8_t *body = data + pos + 8;

        if (memcmp(type, "IHDR", 4) == 0 && chunk >= 13) {
            hd.width = be32(body);
            hd.height = be32(body + 4);
            hd.depth = body[8];
            hd.color = body[9];
            hd.compression = body[10];
            hd.filter = body[11];
            hd.interlace = body[12];
            have_header = true;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            pal.count = (uint16_t)MIN(chunk / 3, 256u);
            for (uint16_t i = 0; i < pal.count; i++) {
                pal.entry[i][0] = body[i * 3];
                pal.entry[i][1] = body[i * 3 + 1];
                pal.entry[i][2] = body[i * 3 + 2];
                pal.alpha[i] = 255;
            }
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (hd.color == PNG_PALETTE) {
                pal.has_alpha = true;
                for (uint32_t i = 0; i < chunk && i < 256; i++)
                    pal.alpha[i] = body[i];
            } else if (hd.color == PNG_GRAY && chunk >= 2) {
                pal.has_key = true;
                pal.key[0] = (uint16_t)((body[0] << 8) | body[1]);
            } else if (hd.color == PNG_RGB && chunk >= 6) {
                pal.has_key = true;
                for (int i = 0; i < 3; i++)
                    pal.key[i] = (uint16_t)((body[i * 2] << 8) | body[i * 2 + 1]);
            }
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (idat_length + chunk > idat_capacity) {
                size_t want = idat_capacity ? idat_capacity * 2 : 8192;

                while (want < idat_length + chunk)
                    want *= 2;

                uint8_t *bigger = krealloc(idat, want);

                if (!bigger) {
                    kfree(idat);
                    return false;
                }
                idat = bigger;
                idat_capacity = want;
            }
            memcpy(idat + idat_length, body, chunk);
            idat_length += chunk;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + chunk;
    }

    if (!have_header || !idat || hd.compression != 0 || hd.filter != 0 ||
        hd.width == 0 || hd.height == 0 ||
        hd.width > 8192 || hd.height > 8192 ||
        channels_of(hd.color) == 0 || hd.interlace > 1) {
        kfree(idat);
        return false;
    }
    if (hd.depth != 1 && hd.depth != 2 && hd.depth != 4 &&
        hd.depth != 8 && hd.depth != 16) {
        kfree(idat);
        return false;
    }
    if (hd.color != PNG_GRAY && hd.color != PNG_PALETTE && hd.depth < 8) {
        kfree(idat);
        return false;
    }

    size_t raw_length = 0;
    uint8_t *raw = inflate_zlib(idat, idat_length, &raw_length);

    kfree(idat);
    if (!raw)
        return false;

    out->w = (int32_t)hd.width;
    out->h = (int32_t)hd.height;
    out->px = kmalloc((size_t)out->w * out->h * 4);
    if (!out->px) {
        kfree(raw);
        return false;
    }
    memset(out->px, 0, (size_t)out->w * out->h * 4);

    const uint8_t *cursor = raw;
    size_t left = raw_length;
    bool ok = true;

    if (hd.interlace == 0) {
        ok = read_pass(&cursor, &left, &hd, &pal, out, 0, 0, 1, 1);
    } else {
        for (int p = 0; p < 7 && ok; p++)
            ok = read_pass(&cursor, &left, &hd, &pal, out,
                           adam_x0[p], adam_y0[p], adam_dx[p], adam_dy[p]);
    }

    kfree(raw);
    if (!ok) {
        image_free(out);
        return false;
    }
    return true;
}
