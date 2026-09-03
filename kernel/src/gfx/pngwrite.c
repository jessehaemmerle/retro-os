/* pngwrite.c - PNG schreiben.
 *
 * Zum Lesen gehoert ein vollstaendiger DEFLATE-Entpacker (inflate.c);
 * zum Schreiben braucht es den nicht. DEFLATE kennt einen Blocktyp,
 * der gar nicht packt, sondern die Bytes unveraendert weiterreicht -
 * und den versteht jeder Leser, auch der eigene.
 *
 * Ein Bildschirmfoto wird damit etwa so gross wie das Bild selbst.
 * Dafuer sind es zweihundert Zeilen statt zweitausend, und ein Packer,
 * den niemand prueft, waere die schlechtere Wahl: Ein Fehler darin
 * faellt erst auf, wenn ein fremdes Programm die Datei nicht mehr
 * lesen kann.
 *
 * Eine Pruefung im Testlauf schreibt Bilder und liest sie mit dem
 * eigenen Leser wieder ein - Punkt fuer Punkt.
 */

#include "image.h"
#include "kstring.h"
#include "mm.h"

/* Ein Puffer, der mitwaechst. */
struct sink {
    uint8_t *data;
    size_t   used;
    size_t   size;
    bool     failed;
};

static bool reserve(struct sink *s, size_t extra)
{
    if (s->failed)
        return false;
    if (s->used + extra <= s->size)
        return true;

    size_t want = s->size ? s->size : 4096;

    while (want < s->used + extra)
        want *= 2;

    uint8_t *next = kmalloc(want);

    if (!next) {
        s->failed = true;
        return false;
    }
    if (s->data) {
        memcpy(next, s->data, s->used);
        kfree(s->data);
    }
    s->data = next;
    s->size = want;
    return true;
}

static void put(struct sink *s, const void *bytes, size_t length)
{
    if (!reserve(s, length))
        return;
    memcpy(s->data + s->used, bytes, length);
    s->used += length;
}

static void put8(struct sink *s, uint8_t value)
{
    put(s, &value, 1);
}

static void put32(struct sink *s, uint32_t value)
{
    uint8_t bytes[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16),
                         (uint8_t)(value >> 8), (uint8_t)value };

    put(s, bytes, 4);
}

/* Ein PNG-Abschnitt: Laenge, Kennung, Inhalt, Pruefsumme ueber
 * Kennung und Inhalt. */
static void put_chunk(struct sink *s, const char *type,
                      const uint8_t *data, size_t length)
{
    put32(s, (uint32_t)length);
    put(s, type, 4);
    put(s, data, length);

    /* crc32_update nimmt und liefert die fertige Pruefsumme - das
     * Umdrehen der Bits steckt schon darin. Wer hier von Hand mit
     * 0xFFFFFFFF anfaengt und am Ende noch einmal umdreht, bekommt
     * eine Zahl, die der eigene Leser nicht prueft und ein fremdes
     * Programm sehr wohl. */
    uint32_t crc = crc32_update(0, type, 4);

    put32(s, crc32_update(crc, data, length));
}

/* Adler-32 - die Pruefsumme, die zlib um die gepackten Daten legt. */
static uint32_t adler32(const uint8_t *data, size_t length)
{
    uint32_t a = 1, b = 0;

    for (size_t i = 0; i < length; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

bool png_encode(const uint32_t *pixels, int32_t w, int32_t h, int32_t stride,
                uint8_t **out, size_t *out_size)
{
    if (!pixels || !out || !out_size || w <= 0 || h <= 0)
        return false;
    if ((uint64_t)w * h > 64u * 1024 * 1024)
        return false;

    *out = NULL;
    *out_size = 0;

    /* Die Rohdaten: je Zeile ein Filterbyte (0 = kein Filter) und
     * danach die Punkte als RGB. Der Alphakanal faellt weg - ein
     * Bildschirmfoto ist deckend, und drei Bytes je Punkt sind ein
     * Viertel weniger Datei. */
    size_t row_bytes = 1 + (size_t)w * 3;
    size_t raw_size = row_bytes * (size_t)h;
    uint8_t *raw = kmalloc(raw_size);

    if (!raw)
        return false;

    for (int32_t y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * row_bytes;

        *row++ = 0;
        for (int32_t x = 0; x < w; x++) {
            uint32_t px = pixels[(size_t)y * stride + x];

            *row++ = (uint8_t)(px >> 16);
            *row++ = (uint8_t)(px >> 8);
            *row++ = (uint8_t)px;
        }
    }

    struct sink s = { 0 };

    static const uint8_t signature[8] = { 137, 'P', 'N', 'G', 13, 10, 26, 10 };

    put(&s, signature, sizeof(signature));

    uint8_t header[13];

    header[0] = (uint8_t)((uint32_t)w >> 24);
    header[1] = (uint8_t)((uint32_t)w >> 16);
    header[2] = (uint8_t)((uint32_t)w >> 8);
    header[3] = (uint8_t)w;
    header[4] = (uint8_t)((uint32_t)h >> 24);
    header[5] = (uint8_t)((uint32_t)h >> 16);
    header[6] = (uint8_t)((uint32_t)h >> 8);
    header[7] = (uint8_t)h;
    header[8] = 8;      /* acht Bit je Kanal            */
    header[9] = 2;      /* Farbtyp 2: RGB ohne Alpha    */
    header[10] = 0;     /* Deflate                      */
    header[11] = 0;     /* Standardfilter               */
    header[12] = 0;     /* nicht verschraenkt           */
    put_chunk(&s, "IHDR", header, sizeof(header));

    /* Der zlib-Kopf: Verfahren 8, Fenstergroesse 32 KB, keine
     * Vorgabe. Die beiden Bytes muessen zusammen durch 31 teilbar
     * sein - 0x78 0x01 erfuellt das. */
    struct sink z = { 0 };

    put8(&z, 0x78);
    put8(&z, 0x01);

    /* Ungepackte Bloecke zu hoechstens 65535 Bytes: ein Kopfbyte mit
     * dem Endekennzeichen, dann die Laenge zweimal - einmal gerade,
     * einmal verkehrt herum. */
    size_t at = 0;

    while (at < raw_size) {
        size_t chunk = raw_size - at;

        if (chunk > 65535)
            chunk = 65535;

        bool last = (at + chunk) >= raw_size;

        put8(&z, last ? 1 : 0);
        put8(&z, (uint8_t)chunk);
        put8(&z, (uint8_t)(chunk >> 8));
        put8(&z, (uint8_t)~chunk);
        put8(&z, (uint8_t)(~chunk >> 8));
        put(&z, raw + at, chunk);
        at += chunk;
    }

    put32(&z, adler32(raw, raw_size));
    kfree(raw);

    if (z.failed) {
        kfree(z.data);
        kfree(s.data);
        return false;
    }

    put_chunk(&s, "IDAT", z.data, z.used);
    kfree(z.data);

    put_chunk(&s, "IEND", NULL, 0);

    if (s.failed) {
        kfree(s.data);
        return false;
    }

    *out = s.data;
    *out_size = s.used;
    return true;
}
