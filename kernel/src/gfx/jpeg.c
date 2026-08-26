/* jpeg.c - Leser fuer JPEG-Bilder nach JFIF, Grundverfahren.
 *
 * Umgesetzt ist das sequentielle Huffman-Verfahren (SOF0 und SOF1) mit
 * bis zu vier Komponenten und beliebiger Unterabtastung. Die inverse
 * Kosinustransformation arbeitet ganzzahlig in zwei Durchgaengen nach
 * dem Verfahren von Loeffler, Ligtenberg und Moschytz.
 */

#include "image.h"
#include "kstring.h"
#include "mm.h"

#define JPEG_MAX_COMPONENTS 4

struct jpeg_huffman {
    uint8_t  bits[17];      /* Anzahl Codes je Laenge 1..16 */
    uint8_t  values[256];
    int32_t  mincode[17];
    int32_t  maxcode[18];
    int32_t  valptr[17];
    bool     present;
};

struct jpeg_component {
    uint8_t  id;
    uint8_t  hsample, vsample;
    uint8_t  quant;
    uint8_t  dc_table, ac_table;
    int32_t  predictor;     /* letzter Gleichanteil */
    int32_t  blocks_w, blocks_h;
    uint8_t *pixels;        /* blocks_w*8 mal blocks_h*8 */
};

struct jpeg {
    const uint8_t *data;
    size_t         length;
    size_t         pos;

    uint16_t width, height;
    uint8_t  ncomp;
    struct jpeg_component comp[JPEG_MAX_COMPONENTS];

    uint16_t quant[4][64];
    struct jpeg_huffman dc[4], ac[4];

    uint8_t  hmax, vmax;
    int32_t  mcus_x, mcus_y;
    uint16_t restart_interval;

    /* Bitstrom der Bilddaten */
    uint32_t bitbuf;
    int32_t  bitcount;
    bool     error;
};

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ------------------------------------------------------------------ */
/* Bitstrom mit Byteauffuellung                                        */
/* ------------------------------------------------------------------ */

static int32_t next_bit(struct jpeg *j)
{
    if (j->bitcount == 0) {
        if (j->pos >= j->length) {
            j->error = true;
            return 0;
        }

        uint8_t byte = j->data[j->pos++];

        if (byte == 0xFF) {
            /* Ein eingeschobenes Nullbyte gehoert nicht zu den Daten. */
            uint8_t marker = j->pos < j->length ? j->data[j->pos] : 0xD9;

            if (marker == 0x00) {
                j->pos++;
            } else if (marker >= 0xD0 && marker <= 0xD7) {
                /* Neustartmarke - der Aufrufer behandelt sie. */
                j->pos--;
                j->error = true;
                return 0;
            } else {
                j->pos--;
                j->error = true;
                return 0;
            }
        }
        j->bitbuf = byte;
        j->bitcount = 8;
    }
    j->bitcount--;
    return (int32_t)((j->bitbuf >> j->bitcount) & 1u);
}

static int32_t receive(struct jpeg *j, int32_t count)
{
    int32_t value = 0;

    while (count-- > 0)
        value = (value << 1) | next_bit(j);
    return value;
}

/* Wandelt einen Rohwert in den vorzeichenbehafteten Koeffizienten um. */
static int32_t extend(int32_t value, int32_t count)
{
    if (count == 0)
        return 0;
    if (value < (1 << (count - 1)))
        return value - (1 << count) + 1;
    return value;
}

static int32_t decode_huffman(struct jpeg *j, const struct jpeg_huffman *h)
{
    int32_t code = next_bit(j);
    int32_t len = 1;

    while (len <= 16 && code > h->maxcode[len]) {
        code = (code << 1) | next_bit(j);
        len++;
        if (j->error)
            return -1;
    }
    if (len > 16 || j->error)
        return -1;

    int32_t index = h->valptr[len] + code - h->mincode[len];

    if (index < 0 || index > 255)
        return -1;
    return h->values[index];
}

static void build_huffman(struct jpeg_huffman *h)
{
    int32_t code = 0, k = 0;

    for (int32_t len = 1; len <= 16; len++) {
        h->valptr[len] = k;
        h->mincode[len] = code;
        code += h->bits[len];
        k += h->bits[len];
        h->maxcode[len] = h->bits[len] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
    h->present = true;
}

/* ------------------------------------------------------------------ */
/* Inverse Kosinustransformation, ganzzahlig                           */
/* ------------------------------------------------------------------ */

#define IDCT_BITS 11
#define FIX(x) ((int32_t)((x) * (1 << IDCT_BITS) + 0.5))

static const int32_t c1 = FIX(1.387039845);   /* 2*cos(pi/16)     */
static const int32_t c2 = FIX(1.306562965);   /* 2*cos(2*pi/16)   */
static const int32_t c3 = FIX(1.175875602);   /* 2*cos(3*pi/16)   */
static const int32_t c5 = FIX(0.785694958);   /* 2*cos(5*pi/16)   */
static const int32_t c6 = FIX(0.541196100);   /* 2*cos(6*pi/16)   */
static const int32_t c7 = FIX(0.275899379);   /* 2*cos(7*pi/16)   */
/* Der Faktor fuer die Wurzel aus zwei steht hier auf acht Stellen,
 * nicht auf elf wie die uebrigen - so verlangt es die Schrittfolge. */
#define R2 181                                /* 256 / Wurzel(2)  */

static void idct_row(int32_t *b)
{
    /* Sind alle Wechselanteile null, ist das Ergebnis konstant. */
    if (!(b[1] | b[2] | b[3] | b[4] | b[5] | b[6] | b[7])) {
        int32_t value = b[0] << 3;

        for (int i = 0; i < 8; i++)
            b[i] = value;
        return;
    }

    int32_t x0 = (b[0] << 11) + 128;
    int32_t x1 = b[4] << 11;
    int32_t x2 = b[6], x3 = b[2], x4 = b[1], x5 = b[7], x6 = b[5], x7 = b[3];
    int32_t x8;

    x8 = c7 * (x4 + x5);
    x4 = x8 + (c1 - c7) * x4;
    x5 = x8 - (c1 + c7) * x5;
    x8 = c3 * (x6 + x7);
    x6 = x8 - (c3 - c5) * x6;
    x7 = x8 - (c3 + c5) * x7;

    x8 = x0 + x1;
    x0 -= x1;
    x1 = c6 * (x3 + x2);
    x2 = x1 - (c2 + c6) * x2;
    x3 = x1 + (c2 - c6) * x3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;

    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (R2 * (x4 + x5) + 128) >> 8;
    x4 = (R2 * (x4 - x5) + 128) >> 8;

    b[0] = (x7 + x1) >> 8;
    b[1] = (x3 + x2) >> 8;
    b[2] = (x0 + x4) >> 8;
    b[3] = (x8 + x6) >> 8;
    b[4] = (x8 - x6) >> 8;
    b[5] = (x0 - x4) >> 8;
    b[6] = (x3 - x2) >> 8;
    b[7] = (x7 - x1) >> 8;
}

static uint8_t clamp_byte(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static void idct_column(int32_t *b, uint8_t *out, int32_t stride)
{
    int32_t x0, x1, x2, x3, x4, x5, x6, x7, x8;

    if (!(b[8 * 1] | b[8 * 2] | b[8 * 3] | b[8 * 4] |
          b[8 * 5] | b[8 * 6] | b[8 * 7])) {
        uint8_t value = clamp_byte(((b[0] + 32) >> 6) + 128);

        for (int i = 0; i < 8; i++)
            out[i * stride] = value;
        return;
    }

    x0 = (b[8 * 0] << 8) + 8192;
    x1 = b[8 * 4] << 8;
    x2 = b[8 * 6];
    x3 = b[8 * 2];
    x4 = b[8 * 1];
    x5 = b[8 * 7];
    x6 = b[8 * 5];
    x7 = b[8 * 3];

    x8 = c7 * (x4 + x5) + 4;
    x4 = (x8 + (c1 - c7) * x4) >> 3;
    x5 = (x8 - (c1 + c7) * x5) >> 3;
    x8 = c3 * (x6 + x7) + 4;
    x6 = (x8 - (c3 - c5) * x6) >> 3;
    x7 = (x8 - (c3 + c5) * x7) >> 3;

    x8 = x0 + x1;
    x0 -= x1;
    x1 = c6 * (x3 + x2) + 4;
    x2 = (x1 - (c2 + c6) * x2) >> 3;
    x3 = (x1 + (c2 - c6) * x3) >> 3;
    x1 = x4 + x6;
    x4 -= x6;
    x6 = x5 + x7;
    x5 -= x7;

    x7 = x8 + x3;
    x8 -= x3;
    x3 = x0 + x2;
    x0 -= x2;
    x2 = (R2 * (x4 + x5) + 128) >> 8;
    x4 = (R2 * (x4 - x5) + 128) >> 8;

    out[0 * stride] = clamp_byte(((x7 + x1) >> 14) + 128);
    out[1 * stride] = clamp_byte(((x3 + x2) >> 14) + 128);
    out[2 * stride] = clamp_byte(((x0 + x4) >> 14) + 128);
    out[3 * stride] = clamp_byte(((x8 + x6) >> 14) + 128);
    out[4 * stride] = clamp_byte(((x8 - x6) >> 14) + 128);
    out[5 * stride] = clamp_byte(((x0 - x4) >> 14) + 128);
    out[6 * stride] = clamp_byte(((x3 - x2) >> 14) + 128);
    out[7 * stride] = clamp_byte(((x7 - x1) >> 14) + 128);
}

static void idct_block(int32_t *block, uint8_t *out, int32_t stride)
{
    for (int i = 0; i < 8; i++)
        idct_row(block + i * 8);
    for (int i = 0; i < 8; i++)
        idct_column(block + i, out + i, stride);
}

/* ------------------------------------------------------------------ */
/* Segmente lesen                                                      */
/* ------------------------------------------------------------------ */

static bool read_quant(struct jpeg *j, const uint8_t *body, uint16_t length)
{
    uint16_t pos = 0;

    while (pos < length) {
        uint8_t spec = body[pos++];
        uint8_t id = spec & 0x0F;
        bool wide = (spec >> 4) != 0;

        if (id >= 4)
            return false;
        if (pos + (wide ? 128u : 64u) > length)
            return false;

        for (int i = 0; i < 64; i++) {
            if (wide) {
                j->quant[id][zigzag[i]] =
                    (uint16_t)((body[pos] << 8) | body[pos + 1]);
                pos += 2;
            } else {
                j->quant[id][zigzag[i]] = body[pos++];
            }
        }
    }
    return true;
}

static bool read_huffman_table(struct jpeg *j, const uint8_t *body,
                               uint16_t length)
{
    uint16_t pos = 0;

    while (pos + 17 <= length) {
        uint8_t spec = body[pos++];
        uint8_t id = spec & 0x0F;
        bool is_ac = (spec >> 4) != 0;

        if (id >= 4)
            return false;

        struct jpeg_huffman *h = is_ac ? &j->ac[id] : &j->dc[id];
        uint32_t total = 0;

        h->bits[0] = 0;
        for (int i = 1; i <= 16; i++) {
            h->bits[i] = body[pos++];
            total += h->bits[i];
        }
        if (total > 256 || pos + total > length)
            return false;
        for (uint32_t i = 0; i < total; i++)
            h->values[i] = body[pos++];
        build_huffman(h);
    }
    return true;
}

static bool read_frame(struct jpeg *j, const uint8_t *body, uint16_t length)
{
    if (length < 6)
        return false;
    if (body[0] != 8)          /* nur acht Bit Genauigkeit */
        return false;

    j->height = (uint16_t)((body[1] << 8) | body[2]);
    j->width = (uint16_t)((body[3] << 8) | body[4]);
    j->ncomp = body[5];

    if (j->ncomp == 0 || j->ncomp > JPEG_MAX_COMPONENTS)
        return false;
    if (length < 6u + (uint16_t)j->ncomp * 3u)
        return false;
    if (j->width == 0 || j->height == 0 ||
        j->width > 8192 || j->height > 8192)
        return false;

    j->hmax = 1;
    j->vmax = 1;
    for (uint8_t i = 0; i < j->ncomp; i++) {
        const uint8_t *c = body + 6 + i * 3;

        j->comp[i].id = c[0];
        j->comp[i].hsample = c[1] >> 4;
        j->comp[i].vsample = c[1] & 0x0F;
        j->comp[i].quant = c[2];
        if (j->comp[i].hsample == 0 || j->comp[i].hsample > 4 ||
            j->comp[i].vsample == 0 || j->comp[i].vsample > 4 ||
            j->comp[i].quant >= 4)
            return false;
        j->hmax = MAX(j->hmax, j->comp[i].hsample);
        j->vmax = MAX(j->vmax, j->comp[i].vsample);
    }

    j->mcus_x = (j->width + j->hmax * 8 - 1) / (j->hmax * 8);
    j->mcus_y = (j->height + j->vmax * 8 - 1) / (j->vmax * 8);

    for (uint8_t i = 0; i < j->ncomp; i++) {
        struct jpeg_component *comp = &j->comp[i];

        comp->blocks_w = j->mcus_x * comp->hsample;
        comp->blocks_h = j->mcus_y * comp->vsample;
        comp->pixels = kmalloc((size_t)comp->blocks_w * 8 *
                               comp->blocks_h * 8);
        if (!comp->pixels)
            return false;
        memset(comp->pixels, 128, (size_t)comp->blocks_w * 8 *
                                  comp->blocks_h * 8);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Bilddaten entschluesseln                                            */
/* ------------------------------------------------------------------ */

static bool decode_block(struct jpeg *j, struct jpeg_component *comp,
                         int32_t bx, int32_t by)
{
    int32_t block[64];

    memset(block, 0, sizeof(block));

    const struct jpeg_huffman *dc = &j->dc[comp->dc_table];
    const struct jpeg_huffman *ac = &j->ac[comp->ac_table];
    const uint16_t *q = j->quant[comp->quant];

    int32_t t = decode_huffman(j, dc);

    if (t < 0 || t > 16)
        return false;

    comp->predictor += extend(receive(j, t), t);
    block[0] = comp->predictor * q[0];

    for (int32_t k = 1; k < 64; ) {
        int32_t rs = decode_huffman(j, ac);

        if (rs < 0)
            return false;

        int32_t run = rs >> 4;
        int32_t size = rs & 0x0F;

        if (size == 0) {
            if (run != 15)
                break;          /* Rest des Blocks ist null */
            k += 16;
            continue;
        }
        k += run;
        if (k > 63)
            return false;
        block[zigzag[k]] = extend(receive(j, size), size) * q[zigzag[k]];
        k++;
    }
    if (j->error)
        return false;

    int32_t stride = comp->blocks_w * 8;

    idct_block(block, comp->pixels + (size_t)by * 8 * stride + bx * 8, stride);
    return true;
}

static void reset_bits(struct jpeg *j)
{
    j->bitbuf = 0;
    j->bitcount = 0;
    j->error = false;
    for (uint8_t i = 0; i < j->ncomp; i++)
        j->comp[i].predictor = 0;
}

static bool decode_scan(struct jpeg *j)
{
    int32_t since_restart = 0;

    reset_bits(j);

    for (int32_t my = 0; my < j->mcus_y; my++) {
        for (int32_t mx = 0; mx < j->mcus_x; mx++) {
            if (j->restart_interval && since_restart == j->restart_interval) {
                /* Auf die naechste Neustartmarke aufsetzen. */
                while (j->pos + 1 < j->length &&
                       !(j->data[j->pos] == 0xFF &&
                         j->data[j->pos + 1] >= 0xD0 &&
                         j->data[j->pos + 1] <= 0xD7))
                    j->pos++;
                if (j->pos + 1 >= j->length)
                    return false;
                j->pos += 2;
                reset_bits(j);
                since_restart = 0;
            }

            for (uint8_t c = 0; c < j->ncomp; c++) {
                struct jpeg_component *comp = &j->comp[c];

                for (uint8_t v = 0; v < comp->vsample; v++)
                    for (uint8_t h = 0; h < comp->hsample; h++)
                        if (!decode_block(j, comp,
                                          mx * comp->hsample + h,
                                          my * comp->vsample + v))
                            return false;
            }
            since_restart++;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Farbraum                                                            */
/* ------------------------------------------------------------------ */

static uint8_t component_sample(const struct jpeg *j,
                                const struct jpeg_component *comp,
                                int32_t x, int32_t y)
{
    int32_t sx = x * comp->hsample / j->hmax;
    int32_t sy = y * comp->vsample / j->vmax;
    int32_t stride = comp->blocks_w * 8;

    sx = CLAMP(sx, 0, stride - 1);
    sy = CLAMP(sy, 0, comp->blocks_h * 8 - 1);
    return comp->pixels[(size_t)sy * stride + sx];
}

static void ycbcr_to_rgb(int32_t y, int32_t cb, int32_t cr,
                         uint8_t *r, uint8_t *g, uint8_t *b)
{
    cb -= 128;
    cr -= 128;
    *r = clamp_byte(y + ((91881 * cr) >> 16));
    *g = clamp_byte(y - ((22554 * cb + 46802 * cr) >> 16));
    *b = clamp_byte(y + ((116130 * cb) >> 16));
}

/* ------------------------------------------------------------------ */
/* Hauptteil                                                           */
/* ------------------------------------------------------------------ */

static void jpeg_release(struct jpeg *j)
{
    for (uint8_t i = 0; i < JPEG_MAX_COMPONENTS; i++)
        kfree(j->comp[i].pixels);
}

bool image_decode_jpeg(const uint8_t *data, size_t length, struct image *out)
{
    if (length < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return false;

    struct jpeg *j = kmalloc(sizeof(*j));

    if (!j)
        return false;
    memset(j, 0, sizeof(*j));
    j->data = data;
    j->length = length;
    j->pos = 2;

    bool have_frame = false, decoded = false;

    while (j->pos + 4 <= length) {
        if (data[j->pos] != 0xFF) {
            j->pos++;
            continue;
        }

        uint8_t marker = data[j->pos + 1];

        if (marker == 0xFF) {
            j->pos++;
            continue;
        }
        j->pos += 2;

        if (marker == 0xD9)             /* Bildende */
            break;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7))
            continue;                   /* ohne Nutzlast */

        if (j->pos + 2 > length)
            break;

        uint16_t seglen = (uint16_t)((data[j->pos] << 8) | data[j->pos + 1]);

        if (seglen < 2 || j->pos + seglen > length)
            break;

        const uint8_t *body = data + j->pos + 2;
        uint16_t bodylen = (uint16_t)(seglen - 2);

        switch (marker) {
        case 0xC0:
        case 0xC1:
            if (!read_frame(j, body, bodylen))
                goto fail;
            have_frame = true;
            break;
        case 0xC4:
            if (!read_huffman_table(j, body, bodylen))
                goto fail;
            break;
        case 0xDB:
            if (!read_quant(j, body, bodylen))
                goto fail;
            break;
        case 0xDD:
            if (bodylen >= 2)
                j->restart_interval =
                    (uint16_t)((body[0] << 8) | body[1]);
            break;
        case 0xDA: {
            if (!have_frame || bodylen < 1)
                goto fail;

            uint8_t ns = body[0];

            if (ns != j->ncomp || bodylen < 1u + ns * 2u)
                goto fail;
            for (uint8_t i = 0; i < ns; i++) {
                uint8_t id = body[1 + i * 2];
                uint8_t tables = body[2 + i * 2];
                bool found = false;

                for (uint8_t c = 0; c < j->ncomp; c++) {
                    if (j->comp[c].id != id)
                        continue;
                    j->comp[c].dc_table = tables >> 4;
                    j->comp[c].ac_table = tables & 0x0F;
                    if (j->comp[c].dc_table >= 4 || j->comp[c].ac_table >= 4)
                        goto fail;
                    found = true;
                    break;
                }
                if (!found)
                    goto fail;
            }

            j->pos += seglen;
            if (!decode_scan(j))
                goto fail;
            decoded = true;
            goto finish;
        }
        default:
            break;                      /* alles Weitere ueberspringen */
        }
        j->pos += seglen;
    }

finish:
    if (!have_frame || !decoded)
        goto fail;

    out->w = j->width;
    out->h = j->height;
    out->px = kmalloc((size_t)out->w * out->h * 4);
    if (!out->px)
        goto fail;

    for (int32_t y = 0; y < out->h; y++) {
        for (int32_t x = 0; x < out->w; x++) {
            uint8_t r, g, b;

            if (j->ncomp >= 3) {
                ycbcr_to_rgb(component_sample(j, &j->comp[0], x, y),
                             component_sample(j, &j->comp[1], x, y),
                             component_sample(j, &j->comp[2], x, y),
                             &r, &g, &b);
            } else {
                r = g = b = component_sample(j, &j->comp[0], x, y);
            }
            out->px[(size_t)y * out->w + x] =
                0xFF000000u | ((uint32_t)r << 16) |
                ((uint32_t)g << 8) | b;
        }
    }

    jpeg_release(j);
    kfree(j);
    return true;

fail:
    jpeg_release(j);
    kfree(j);
    return false;
}
