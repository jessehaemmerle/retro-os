/* deflate.c - siehe deflate.h. */

#include "deflate.h"
#include "kstring.h"
#include "mm.h"

#define WINDOW      32768
#define MIN_MATCH   3
#define MAX_MATCH   258
#define HASH_BITS   15
#define HASH_SIZE   (1u << HASH_BITS)
#define NO_POS      0xFFFFFFFFu

/* Wie weit die Kette gleicher Anfaenge zurueckverfolgt wird. Weiter
 * heisst kleiner und langsamer; hundert ist der Punkt, an dem beides
 * noch stimmt. */
#define MAX_CHAIN   128

/* --- Bits schreiben ------------------------------------------------- */

/* DEFLATE schreibt seine Bits von unten nach oben in die Bytes - die
 * Huffman-Kennungen dagegen von oben nach unten. Beide Richtungen
 * sauber auseinanderzuhalten ist die halbe Miete. */
struct writer {
    uint8_t *data;
    size_t   used;
    size_t   size;
    uint32_t bits;      /* was noch nicht in einem Byte steht */
    int      count;
    bool     failed;
};

static bool grow(struct writer *w, size_t extra)
{
    if (w->failed)
        return false;
    if (w->used + extra <= w->size)
        return true;

    size_t want = w->size ? w->size : 1024;

    while (want < w->used + extra)
        want *= 2;

    uint8_t *next = kmalloc(want);

    if (!next) {
        w->failed = true;
        return false;
    }
    if (w->data) {
        memcpy(next, w->data, w->used);
        kfree(w->data);
    }
    w->data = next;
    w->size = want;
    return true;
}

static void put_bits(struct writer *w, uint32_t value, int count)
{
    w->bits |= (value & ((1u << count) - 1)) << w->count;
    w->count += count;

    while (w->count >= 8) {
        if (!grow(w, 1))
            return;
        w->data[w->used++] = (uint8_t)w->bits;
        w->bits >>= 8;
        w->count -= 8;
    }
}

static void flush_bits(struct writer *w)
{
    if (w->count > 0 && grow(w, 1)) {
        w->data[w->used++] = (uint8_t)w->bits;
        w->bits = 0;
        w->count = 0;
    }
}

/* Eine Huffman-Kennung steht mit dem hoechsten Bit zuerst. */
static void put_code(struct writer *w, uint32_t code, int bits)
{
    for (int i = bits - 1; i >= 0; i--)
        put_bits(w, (code >> i) & 1, 1);
}

/* --- Die festen Baeume ---------------------------------------------- */

/* Literale und Laengen: 0-143 mit acht Bit ab 0x30, 144-255 mit neun
 * ab 0x190, 256-279 mit sieben ab 0, 280-287 mit acht ab 0xC0. So
 * steht es in RFC 1951. */
static void put_literal(struct writer *w, uint32_t symbol)
{
    if (symbol < 144)
        put_code(w, 0x30 + symbol, 8);
    else if (symbol < 256)
        put_code(w, 0x190 + symbol - 144, 9);
    else if (symbol < 280)
        put_code(w, symbol - 256, 7);
    else
        put_code(w, 0xC0 + symbol - 280, 8);
}

/* Laenge und Abstand werden je in eine Kennung und ein paar
 * Zusatzbits zerlegt. Die Tabellen stehen ebenfalls in der Norm. */
static const uint16_t length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
    51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3,
    3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
    385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
    16385, 24577
};
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static void put_match(struct writer *w, uint32_t length, uint32_t distance)
{
    int code = 28;

    while (code > 0 && length < length_base[code])
        code--;
    put_literal(w, 257u + (uint32_t)code);
    if (length_extra[code])
        put_bits(w, length - length_base[code], length_extra[code]);

    int dcode = 29;

    while (dcode > 0 && distance < dist_base[dcode])
        dcode--;
    /* Abstaende haben ihren eigenen festen Baum: fuenf Bit, mit dem
     * hoechsten zuerst. */
    put_code(w, (uint32_t)dcode, 5);
    if (dist_extra[dcode])
        put_bits(w, distance - dist_base[dcode], dist_extra[dcode]);
}

/* --- Suchen ---------------------------------------------------------- */

/* Ungepackte Bloecke: ein Kopfbyte, dann die Laenge zweimal - einmal
 * gerade, einmal verkehrt herum. Sie sind der Ausweg fuer Daten, an
 * denen nichts zu holen ist: Feste Huffman-Baeume machen die naemlich
 * groesser, weil jedes Byte oberhalb von 143 neun statt acht Bit
 * bekommt. Ein Packer, der Zufall aufblaeht, ist ein schlechter
 * Packer. */
static size_t stored_size(size_t length)
{
    size_t blocks = length / 65535 + 1;

    return length + blocks * 5;
}

static void *store_raw(const uint8_t *data, size_t length, size_t *out_length)
{
    struct writer w = { 0 };
    size_t at = 0;

    do {
        size_t chunk = length - at;

        if (chunk > 65535)
            chunk = 65535;

        bool last = (at + chunk) >= length;

        put_bits(&w, last ? 1 : 0, 1);
        put_bits(&w, 0, 2);
        flush_bits(&w);          /* der Rest wird auf ein Byte gebracht */

        if (!grow(&w, chunk + 4)) {
            kfree(w.data);
            return NULL;
        }
        w.data[w.used++] = (uint8_t)chunk;
        w.data[w.used++] = (uint8_t)(chunk >> 8);
        w.data[w.used++] = (uint8_t)~chunk;
        w.data[w.used++] = (uint8_t)(~chunk >> 8);
        memcpy(w.data + w.used, data + at, chunk);
        w.used += chunk;
        at += chunk;
    } while (at < length);

    if (w.failed) {
        kfree(w.data);
        return NULL;
    }
    *out_length = w.used;
    return w.data;
}

static uint32_t hash3(const uint8_t *p)
{
    uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];

    return (v * 2654435761u) >> (32 - HASH_BITS);
}

void *deflate_raw(const uint8_t *data, size_t length, size_t *out_length)
{
    if (!data || !out_length)
        return NULL;

    uint32_t *head = kmalloc(HASH_SIZE * sizeof(uint32_t));
    uint32_t *prev = length ? kmalloc(length * sizeof(uint32_t)) : NULL;

    if (!head || (length && !prev)) {
        kfree(head);
        kfree(prev);
        return NULL;
    }

    for (uint32_t i = 0; i < HASH_SIZE; i++)
        head[i] = NO_POS;

    struct writer w = { 0 };

    /* Ein einziger Block, letzter Block, feste Baeume. */
    put_bits(&w, 1, 1);
    put_bits(&w, 1, 2);

    size_t at = 0;

    while (at < length) {
        size_t best_len = 0;
        size_t best_dist = 0;

        if (at + MIN_MATCH <= length) {
            uint32_t slot = hash3(data + at);
            uint32_t candidate = head[slot];
            int chain = MAX_CHAIN;

            while (candidate != NO_POS && chain-- > 0) {
                size_t distance = at - candidate;

                if (distance == 0 || distance > WINDOW)
                    break;

                /* Erst das Byte hinter der bisher besten Laenge
                 * vergleichen: Wer dort schon abweicht, kann nicht
                 * laenger werden, und der Rest bleibt ungelesen. */
                if (best_len > 0 &&
                    data[candidate + best_len] != data[at + best_len]) {
                    candidate = prev[candidate];
                    continue;
                }

                size_t len = 0;
                size_t room = length - at;

                if (room > MAX_MATCH)
                    room = MAX_MATCH;

                while (len < room && data[candidate + len] == data[at + len])
                    len++;

                if (len > best_len) {
                    best_len = len;
                    best_dist = distance;
                    if (len >= MAX_MATCH)
                        break;
                }
                candidate = prev[candidate];
            }
        }

        if (best_len >= MIN_MATCH) {
            put_match(&w, (uint32_t)best_len, (uint32_t)best_dist);

            /* Auch die uebersprungenen Stellen kommen in die Kette -
             * sonst findet der naechste Durchgang sie nie. */
            for (size_t i = 0; i < best_len; i++) {
                if (at + i + MIN_MATCH <= length) {
                    uint32_t slot = hash3(data + at + i);

                    prev[at + i] = head[slot];
                    head[slot] = (uint32_t)(at + i);
                }
            }
            at += best_len;
        } else {
            put_literal(&w, data[at]);
            if (at + MIN_MATCH <= length) {
                uint32_t slot = hash3(data + at);

                prev[at] = head[slot];
                head[slot] = (uint32_t)at;
            }
            at++;
        }
    }

    put_literal(&w, 256);     /* Ende des Blocks */
    flush_bits(&w);

    kfree(head);
    kfree(prev);

    if (w.failed) {
        kfree(w.data);
        return NULL;
    }

    /* Ein leerer Eingang ergibt trotzdem einen gueltigen Block. */
    if (!w.data && !grow(&w, 1)) {
        return NULL;
    }

    /* Ist nichts dabei herausgekommen, wird ungepackt geschrieben. */
    if (w.used > stored_size(length)) {
        size_t plain_length = 0;
        void *plain = store_raw(data, length, &plain_length);

        if (plain) {
            kfree(w.data);
            *out_length = plain_length;
            return plain;
        }
    }

    *out_length = w.used;
    return w.data;
}

uint32_t adler32(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t a = 1, b = 0;

    for (size_t i = 0; i < length; i++) {
        a = (a + bytes[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void *deflate_zlib(const uint8_t *data, size_t length, size_t *out_length)
{
    size_t packed_length = 0;
    uint8_t *packed = deflate_raw(data, length, &packed_length);

    if (!packed)
        return NULL;

    uint8_t *out = kmalloc(packed_length + 6);

    if (!out) {
        kfree(packed);
        return NULL;
    }

    /* Verfahren 8, Fenster 32 KB; die beiden Bytes zusammen muessen
     * durch 31 teilbar sein. */
    out[0] = 0x78;
    out[1] = 0x9C;
    memcpy(out + 2, packed, packed_length);
    kfree(packed);

    uint32_t sum = adler32(data, length);

    out[packed_length + 2] = (uint8_t)(sum >> 24);
    out[packed_length + 3] = (uint8_t)(sum >> 16);
    out[packed_length + 4] = (uint8_t)(sum >> 8);
    out[packed_length + 5] = (uint8_t)sum;

    *out_length = packed_length + 6;
    return out;
}
