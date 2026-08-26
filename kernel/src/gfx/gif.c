/* gif.c - Leser fuer GIF-Bilder (87a und 89a).
 *
 * Gezeigt wird stets das erste Teilbild. Die Daten sind mit LZW nach
 * dem Verfahren von Welch verpackt, wobei die Codebreite waechst,
 * sobald das Woerterbuch voll ist.
 */

#include "image.h"
#include "kstring.h"
#include "mm.h"

#define GIF_MAX_CODES 4096

struct gif_reader {
    const uint8_t *data;
    size_t         length;
    size_t         pos;

    /* aktueller Unterblock */
    size_t         block_end;
    bool           finished;

    uint32_t       bitbuf;
    int32_t        bitcount;
};

/* Liefert das naechste Byte der zusammengesetzten Unterbloecke. */
static int32_t next_byte(struct gif_reader *r)
{
    while (r->pos >= r->block_end) {
        if (r->finished || r->pos >= r->length)
            return -1;

        uint8_t size = r->data[r->pos++];

        if (size == 0) {
            r->finished = true;
            return -1;
        }
        if (r->pos + size > r->length)
            return -1;
        r->block_end = r->pos + size;
    }
    return r->data[r->pos++];
}

static int32_t next_code(struct gif_reader *r, int32_t width)
{
    while (r->bitcount < width) {
        int32_t byte = next_byte(r);

        if (byte < 0)
            return -1;
        r->bitbuf |= (uint32_t)byte << r->bitcount;
        r->bitcount += 8;
    }

    int32_t code = (int32_t)(r->bitbuf & ((1u << width) - 1u));

    r->bitbuf >>= width;
    r->bitcount -= width;
    return code;
}

bool image_decode_gif(const uint8_t *data, size_t length, struct image *out)
{
    if (length < 13 || memcmp(data, "GIF8", 4) != 0)
        return false;

    uint16_t screen_w = (uint16_t)(data[6] | (data[7] << 8));
    uint16_t screen_h = (uint16_t)(data[8] | (data[9] << 8));
    uint8_t  packed = data[10];
    size_t   pos = 13;
    uint32_t global[256];
    uint32_t local[256];
    uint32_t global_count = 0;

    UNUSED(screen_w);
    UNUSED(screen_h);

    if (packed & 0x80) {
        global_count = 2u << (packed & 0x07);
        if (pos + global_count * 3 > length)
            return false;
        for (uint32_t i = 0; i < global_count; i++) {
            global[i] = 0xFF000000u | ((uint32_t)data[pos] << 16) |
                        ((uint32_t)data[pos + 1] << 8) | data[pos + 2];
            pos += 3;
        }
    }

    int32_t transparent = -1;

    /* Bloecke bis zum ersten Teilbild durchlaufen. */
    while (pos < length) {
        uint8_t block = data[pos++];

        if (block == 0x21) {            /* Erweiterung */
            if (pos >= length)
                return false;

            uint8_t label = data[pos++];

            if (label == 0xF9 && pos + 6 <= length) {
                if (data[pos + 1] & 0x01)
                    transparent = data[pos + 4];
            }
            while (pos < length) {       /* Unterbloecke ueberspringen */
                uint8_t size = data[pos++];

                if (size == 0)
                    break;
                pos += size;
            }
            continue;
        }
        if (block == 0x2C)
            break;                       /* Teilbild gefunden */
        return false;                    /* 0x3B oder Unfug */
    }

    if (pos + 9 > length)
        return false;

    uint16_t img_w = (uint16_t)(data[pos + 4] | (data[pos + 5] << 8));
    uint16_t img_h = (uint16_t)(data[pos + 6] | (data[pos + 7] << 8));
    uint8_t  flags = data[pos + 8];
    bool     interlaced = (flags & 0x40) != 0;

    pos += 9;

    const uint32_t *palette = global;
    uint32_t palette_count = global_count;

    if (flags & 0x80) {
        palette_count = 2u << (flags & 0x07);
        if (pos + palette_count * 3 > length)
            return false;
        for (uint32_t i = 0; i < palette_count; i++) {
            local[i] = 0xFF000000u | ((uint32_t)data[pos] << 16) |
                       ((uint32_t)data[pos + 1] << 8) | data[pos + 2];
            pos += 3;
        }
        palette = local;
    }

    if (palette_count == 0 || img_w == 0 || img_h == 0 ||
        img_w > 8192 || img_h > 8192 || pos >= length)
        return false;

    uint8_t min_code = data[pos++];

    if (min_code < 2 || min_code > 11)
        return false;

    /* Woerterbuch: jeder Eintrag ist Vorgaenger plus ein Zeichen. */
    uint16_t *prefix = kmalloc(GIF_MAX_CODES * sizeof(uint16_t));
    uint8_t  *suffix = kmalloc(GIF_MAX_CODES);
    uint8_t  *stack = kmalloc(GIF_MAX_CODES);
    uint8_t  *indices = kmalloc((size_t)img_w * img_h);

    if (!prefix || !suffix || !stack || !indices) {
        kfree(prefix); kfree(suffix); kfree(stack); kfree(indices);
        return false;
    }
    memset(indices, 0, (size_t)img_w * img_h);

    int32_t clear = 1 << min_code;
    int32_t stop = clear + 1;
    int32_t next_free = stop + 1;
    int32_t width = min_code + 1;
    int32_t previous = -1;

    for (int32_t i = 0; i < clear; i++) {
        prefix[i] = 0xFFFF;
        suffix[i] = (uint8_t)i;
    }

    struct gif_reader r = { data, length, pos, pos, false, 0, 0 };
    size_t written = 0;
    size_t total = (size_t)img_w * img_h;

    while (written < total) {
        int32_t code = next_code(&r, width);

        if (code < 0)
            break;
        if (code == clear) {
            next_free = stop + 1;
            width = min_code + 1;
            previous = -1;
            continue;
        }
        if (code == stop)
            break;
        if (code > next_free || code == stop)
            break;

        /* Die Zeichenfolge des Codes rueckwaerts auf den Stapel legen.
         * Zeigt der Code auf sich selbst, beginnt er mit dem ersten
         * Zeichen der vorigen Folge. */
        int32_t top = 0;
        int32_t current = code;

        if (code == next_free) {
            if (previous < 0)
                break;
            current = previous;
        }
        while (current >= clear) {
            if (top >= GIF_MAX_CODES)
                break;
            stack[top++] = suffix[current];
            current = prefix[current];
        }
        if (top >= GIF_MAX_CODES)
            break;
        stack[top++] = suffix[current];

        uint8_t first = suffix[current];

        if (code == next_free && top < GIF_MAX_CODES) {
            /* Der Sonderfall haengt das erste Zeichen hinten an; auf
             * dem rueckwaerts gefuellten Stapel liegt es ganz unten. */
            for (int32_t i = top; i > 0; i--)
                stack[i] = stack[i - 1];
            stack[0] = first;
            top++;
        }

        while (top > 0 && written < total)
            indices[written++] = stack[--top];

        if (previous >= 0 && next_free < GIF_MAX_CODES) {
            prefix[next_free] = (uint16_t)previous;
            suffix[next_free] = first;
            next_free++;
            if (next_free == (1 << width) && width < 12)
                width++;
        }
        previous = code;
    }

    out->w = img_w;
    out->h = img_h;
    out->px = kmalloc(total * 4);
    if (!out->px) {
        kfree(prefix); kfree(suffix); kfree(stack); kfree(indices);
        return false;
    }

    /* Verschraenkte Bilder kommen in vier Durchgaengen. */
    static const uint8_t pass_start[4] = { 0, 4, 2, 1 };
    static const uint8_t pass_step[4] = { 8, 8, 4, 2 };
    size_t source = 0;

    for (int32_t p = 0; p < (interlaced ? 4 : 1); p++) {
        int32_t start = interlaced ? pass_start[p] : 0;
        int32_t step = interlaced ? pass_step[p] : 1;

        for (int32_t y = start; y < img_h; y += step) {
            for (int32_t x = 0; x < img_w; x++) {
                uint8_t index = source < total ? indices[source] : 0;

                source++;
                uint32_t color = index < palette_count ? palette[index]
                                                      : 0xFF000000u;

                if (transparent >= 0 && index == transparent)
                    color &= 0x00FFFFFFu;
                out->px[(size_t)y * img_w + x] = color;
            }
        }
    }

    kfree(prefix);
    kfree(suffix);
    kfree(stack);
    kfree(indices);
    return true;
}
