/* inflate.c - Entpacken nach DEFLATE (RFC 1951).
 *
 * Der Entpacker liest Bits von hinten nach vorne innerhalb eines Bytes,
 * so wie es das Verfahren vorschreibt. Huffman-Baeume werden als
 * kanonische Codes ueber zwei Tabellen dargestellt: fuer jede Codelaenge
 * merken wir uns den kleinsten Code und den Versatz in der Symbolliste.
 * Damit genuegt eine Schleife ueber die Laengen, um ein Symbol zu lesen.
 */

#include "inflate.h"
#include "kstring.h"
#include "mm.h"

#define MAX_BITS 15

struct bitstream {
    const uint8_t *data;
    size_t         length;
    size_t         pos;      /* naechstes Byte      */
    uint32_t       bitbuf;   /* gepufferte Bits     */
    uint32_t       bitcount;
    bool           error;
};

struct huffman {
    uint16_t count[MAX_BITS + 1];   /* Anzahl Codes je Laenge */
    uint16_t symbol[288];           /* Symbole nach Laenge sortiert */
};

/* Wachsender Ausgabepuffer. */
struct sink {
    uint8_t *data;
    size_t   length;
    size_t   capacity;
    bool     error;
};

/* ------------------------------------------------------------------ */
/* Bitstrom                                                            */
/* ------------------------------------------------------------------ */

static uint32_t bits(struct bitstream *bs, uint32_t need)
{
    while (bs->bitcount < need) {
        if (bs->pos >= bs->length) {
            bs->error = true;
            return 0;
        }
        bs->bitbuf |= (uint32_t)bs->data[bs->pos++] << bs->bitcount;
        bs->bitcount += 8;
    }

    uint32_t value = bs->bitbuf & ((1u << need) - 1u);

    bs->bitbuf >>= need;
    bs->bitcount -= need;
    return value;
}

static void align_byte(struct bitstream *bs)
{
    bs->bitbuf = 0;
    bs->bitcount = 0;
}

/* ------------------------------------------------------------------ */
/* Ausgabepuffer                                                       */
/* ------------------------------------------------------------------ */

static bool sink_reserve(struct sink *s, size_t extra)
{
    if (s->length + extra <= s->capacity)
        return true;

    size_t want = s->capacity ? s->capacity : 4096;

    while (want < s->length + extra) {
        if (want > (64u << 20)) {   /* 64 MiB sind genug fuer eine Seite */
            s->error = true;
            return false;
        }
        want *= 2;
    }

    uint8_t *bigger = krealloc(s->data, want);

    if (!bigger) {
        s->error = true;
        return false;
    }
    s->data = bigger;
    s->capacity = want;
    return true;
}

static void sink_byte(struct sink *s, uint8_t value)
{
    if (!sink_reserve(s, 1))
        return;
    s->data[s->length++] = value;
}

/* ------------------------------------------------------------------ */
/* Huffman                                                             */
/* ------------------------------------------------------------------ */

static void huffman_build(struct huffman *h, const uint8_t *lengths, uint16_t n)
{
    memset(h->count, 0, sizeof(h->count));
    for (uint16_t i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;

    uint16_t offsets[MAX_BITS + 2];

    offsets[1] = 0;
    for (uint16_t len = 1; len <= MAX_BITS; len++)
        offsets[len + 1] = offsets[len] + h->count[len];

    for (uint16_t i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offsets[lengths[i]]++] = i;
}

static int decode_symbol(struct bitstream *bs, const struct huffman *h)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len <= MAX_BITS; len++) {
        code |= (int)bits(bs, 1);
        if (bs->error)
            return -1;

        int count = h->count[len];

        if (code - first < count)
            return h->symbol[index + (code - first)];

        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    bs->error = true;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Bloecke                                                             */
/* ------------------------------------------------------------------ */

static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t distance_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577
};
static const uint8_t distance_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static bool block_stored(struct bitstream *bs, struct sink *out)
{
    align_byte(bs);
    if (bs->pos + 4 > bs->length)
        return false;

    uint16_t len = (uint16_t)(bs->data[bs->pos] | (bs->data[bs->pos + 1] << 8));
    uint16_t nlen = (uint16_t)(bs->data[bs->pos + 2] | (bs->data[bs->pos + 3] << 8));

    bs->pos += 4;
    if ((uint16_t)~len != nlen)
        return false;
    if (bs->pos + len > bs->length)
        return false;
    if (!sink_reserve(out, len))
        return false;

    memcpy(out->data + out->length, bs->data + bs->pos, len);
    out->length += len;
    bs->pos += len;
    return true;
}

static bool block_huffman(struct bitstream *bs, struct sink *out,
                          const struct huffman *lit, const struct huffman *dist)
{
    for (;;) {
        int symbol = decode_symbol(bs, lit);

        if (symbol < 0)
            return false;
        if (symbol < 256) {
            sink_byte(out, (uint8_t)symbol);
            if (out->error)
                return false;
            continue;
        }
        if (symbol == 256)
            return true;

        symbol -= 257;
        if (symbol >= 29)
            return false;

        uint32_t length = length_base[symbol] +
                          bits(bs, length_extra[symbol]);
        int dsym = decode_symbol(bs, dist);

        if (dsym < 0 || dsym >= 30)
            return false;

        uint32_t distance = distance_base[dsym] +
                            bits(bs, distance_extra[dsym]);

        if (bs->error || distance == 0 || distance > out->length)
            return false;
        if (!sink_reserve(out, length))
            return false;

        /* Byteweise kopieren: Bereiche duerfen sich ueberlappen. */
        size_t from = out->length - distance;

        for (uint32_t i = 0; i < length; i++)
            out->data[out->length + i] = out->data[from + i];
        out->length += length;
    }
}

static void fixed_trees(struct huffman *lit, struct huffman *dist)
{
    uint8_t lengths[288];
    int i = 0;

    for (; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    huffman_build(lit, lengths, 288);

    for (i = 0; i < 30; i++) lengths[i] = 5;
    huffman_build(dist, lengths, 30);
}

static bool dynamic_trees(struct bitstream *bs, struct huffman *lit,
                          struct huffman *dist)
{
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint32_t hlit = bits(bs, 5) + 257;
    uint32_t hdist = bits(bs, 5) + 1;
    uint32_t hclen = bits(bs, 4) + 4;

    if (bs->error || hlit > 286 || hdist > 30)
        return false;

    uint8_t code_lengths[19];

    memset(code_lengths, 0, sizeof(code_lengths));
    for (uint32_t i = 0; i < hclen; i++)
        code_lengths[order[i]] = (uint8_t)bits(bs, 3);
    if (bs->error)
        return false;

    struct huffman code_tree;

    huffman_build(&code_tree, code_lengths, 19);

    uint8_t lengths[288 + 30];
    uint32_t filled = 0;

    memset(lengths, 0, sizeof(lengths));
    while (filled < hlit + hdist) {
        int symbol = decode_symbol(bs, &code_tree);

        if (symbol < 0)
            return false;

        if (symbol < 16) {
            lengths[filled++] = (uint8_t)symbol;
            continue;
        }

        uint8_t value = 0;
        uint32_t repeat;

        if (symbol == 16) {
            if (filled == 0)
                return false;
            value = lengths[filled - 1];
            repeat = 3 + bits(bs, 2);
        } else if (symbol == 17) {
            repeat = 3 + bits(bs, 3);
        } else {
            repeat = 11 + bits(bs, 7);
        }
        if (bs->error || filled + repeat > hlit + hdist)
            return false;
        while (repeat--)
            lengths[filled++] = value;
    }

    huffman_build(lit, lengths, (uint16_t)hlit);
    huffman_build(dist, lengths + hlit, (uint16_t)hdist);
    return true;
}

/* ------------------------------------------------------------------ */
/* Oeffentliche Schnittstelle                                          */
/* ------------------------------------------------------------------ */

void *inflate_raw(const uint8_t *data, size_t length, size_t *out_length)
{
    struct bitstream bs = { data, length, 0, 0, 0, false };
    struct sink out = { NULL, 0, 0, false };
    struct huffman lit, dist;
    bool done = false;

    while (!done) {
        uint32_t final = bits(&bs, 1);
        uint32_t type = bits(&bs, 2);

        if (bs.error)
            break;

        bool ok;

        switch (type) {
        case 0:
            ok = block_stored(&bs, &out);
            break;
        case 1:
            fixed_trees(&lit, &dist);
            ok = block_huffman(&bs, &out, &lit, &dist);
            break;
        case 2:
            ok = dynamic_trees(&bs, &lit, &dist) &&
                 block_huffman(&bs, &out, &lit, &dist);
            break;
        default:
            ok = false;
            break;
        }
        if (!ok) {
            bs.error = true;
            break;
        }
        done = final != 0;
    }

    if (!done || out.error) {
        kfree(out.data);
        return NULL;
    }

    *out_length = out.length;
    return out.data ? out.data : kmalloc(1);
}

void *inflate_zlib(const uint8_t *data, size_t length, size_t *out_length)
{
    if (length < 6)
        return NULL;

    uint8_t cmf = data[0], flg = data[1];

    if ((cmf & 0x0F) != 8)              /* nur DEFLATE */
        return NULL;
    if (((cmf << 8) | flg) % 31 != 0)   /* Pruefsumme des Kopfes */
        return NULL;
    if (flg & 0x20)                     /* Vorgabewoerterbuch koennen wir nicht */
        return NULL;

    return inflate_raw(data + 2, length - 2, out_length);
}

void *inflate_gzip(const uint8_t *data, size_t length, size_t *out_length)
{
    if (length < 18 || data[0] != 0x1F || data[1] != 0x8B || data[2] != 8)
        return NULL;

    uint8_t flags = data[3];
    size_t pos = 10;

    if (flags & 0x04) {                 /* zusaetzliches Feld */
        if (pos + 2 > length)
            return NULL;
        pos += 2 + (size_t)(data[pos] | (data[pos + 1] << 8));
    }
    if (flags & 0x08)                   /* Dateiname */
        while (pos < length && data[pos++]) { }
    if (flags & 0x10)                   /* Kommentar */
        while (pos < length && data[pos++]) { }
    if (flags & 0x02)                   /* Kopfpruefsumme */
        pos += 2;
    if (pos >= length)
        return NULL;

    return inflate_raw(data + pos, length - pos, out_length);
}

void *inflate_auto(const uint8_t *data, size_t length, size_t *out_length)
{
    if (length >= 2 && data[0] == 0x1F && data[1] == 0x8B)
        return inflate_gzip(data, length, out_length);
    if (length >= 2 && (data[0] & 0x0F) == 8 &&
        ((data[0] << 8) | data[1]) % 31 == 0)
        return inflate_zlib(data, length, out_length);
    return inflate_raw(data, length, out_length);
}
