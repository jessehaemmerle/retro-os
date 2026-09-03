/* zip.c - siehe zip.h. */

#include "zip.h"
#include "deflate.h"
#include "inflate.h"
#include "kstring.h"
#include "mm.h"

#define SIG_LOCAL    0x04034B50u
#define SIG_CENTRAL  0x02014B50u
#define SIG_END      0x06054B50u

#define LOCAL_HEADER   30
#define CENTRAL_HEADER 46
#define END_HEADER     22

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Lesen                                                               */
/* ------------------------------------------------------------------ */

/* Sucht das Verzeichnis am Ende. Es steht nicht ganz hinten - dahinter
 * darf noch eine Anmerkung von bis zu 64 KB stehen -, also wird
 * rueckwaerts gesucht. */
static const uint8_t *find_end(const uint8_t *data, size_t length)
{
    if (length < END_HEADER)
        return NULL;

    size_t limit = length > 65535 + END_HEADER ? 65535 + END_HEADER : length;

    for (size_t back = END_HEADER; back <= limit; back++) {
        const uint8_t *p = data + length - back;

        if (get32(p) == SIG_END)
            return p;
    }
    return NULL;
}

/* Laeuft das Verzeichnis durch und bleibt beim gesuchten Eintrag
 * stehen. want == (size_t)-1 heisst "nur zaehlen". */
static bool walk(const uint8_t *data, size_t length, size_t want,
                 size_t *count, struct zip_entry *out)
{
    const uint8_t *end = find_end(data, length);

    if (!end)
        return false;

    uint32_t total = get16(end + 10);
    uint32_t dir_size = get32(end + 12);
    uint32_t dir_at = get32(end + 16);

    if ((uint64_t)dir_at + dir_size > length)
        return false;

    const uint8_t *p = data + dir_at;
    const uint8_t *stop = p + dir_size;
    size_t seen = 0;

    while (seen < total && p + CENTRAL_HEADER <= stop) {
        if (get32(p) != SIG_CENTRAL)
            return false;

        uint16_t name_len = get16(p + 28);
        uint16_t extra_len = get16(p + 30);
        uint16_t comment_len = get16(p + 32);
        size_t whole = CENTRAL_HEADER + name_len + extra_len + comment_len;

        if (p + whole > stop)
            return false;

        if (want == seen && out) {
            memset(out, 0, sizeof(*out));

            size_t copy = name_len < ZIP_NAME_MAX ? name_len : ZIP_NAME_MAX;

            memcpy(out->name, p + CENTRAL_HEADER, copy);
            out->name[copy] = '\0';

            out->method = get16(p + 10);
            out->time   = get16(p + 12);
            out->date   = get16(p + 14);
            out->crc    = get32(p + 16);
            out->packed = get32(p + 20);
            out->size   = get32(p + 24);
            out->offset = get32(p + 42);
            out->is_dir = copy > 0 && out->name[copy - 1] == '/';
            return true;
        }

        p += whole;
        seen++;
    }

    if (count)
        *count = seen;
    return want == (size_t)-1;
}

bool zip_read(const uint8_t *data, size_t length, size_t *count)
{
    if (!data || !count)
        return false;
    return walk(data, length, (size_t)-1, count, NULL);
}

bool zip_entry(const uint8_t *data, size_t length, size_t index,
               struct zip_entry *out)
{
    if (!data || !out)
        return false;
    return walk(data, length, index, NULL, out);
}

void *zip_extract(const uint8_t *data, size_t length,
                  const struct zip_entry *entry, size_t *out_length)
{
    if (!data || !entry || !out_length || entry->is_dir)
        return NULL;
    if (entry->offset + LOCAL_HEADER > length)
        return NULL;

    const uint8_t *local = data + entry->offset;

    if (get32(local) != SIG_LOCAL)
        return NULL;

    /* Der lokale Kopf hat eigene Laengen fuer Namen und Zusatzfeld -
     * sie duerfen von denen im Verzeichnis abweichen, und massgeblich
     * ist hier der lokale. */
    size_t skip = LOCAL_HEADER + get16(local + 26) + get16(local + 28);

    if (entry->offset + skip + entry->packed > length)
        return NULL;

    const uint8_t *payload = local + skip;
    uint8_t *result = NULL;
    size_t result_len = 0;

    if (entry->method == 0) {
        if (entry->packed != entry->size)
            return NULL;
        result = kmalloc(entry->size ? (size_t)entry->size : 1);
        if (!result)
            return NULL;
        memcpy(result, payload, (size_t)entry->size);
        result_len = (size_t)entry->size;
    } else if (entry->method == 8) {
        result = inflate_raw(payload, (size_t)entry->packed, &result_len);
        if (!result)
            return NULL;
    } else {
        return NULL;
    }

    /* Die Pruefsumme entscheidet, ob es gutgegangen ist - ein Archiv,
     * das unterwegs gelitten hat, entpackt sonst still zu Unsinn. */
    if (result_len != entry->size || crc32(result, result_len) != entry->crc) {
        kfree(result);
        return NULL;
    }

    *out_length = result_len;
    return result;
}

/* ------------------------------------------------------------------ */
/* Schreiben                                                           */
/* ------------------------------------------------------------------ */

#define ZIP_MAX_ENTRIES 256

struct zip_writer {
    uint8_t *data;
    size_t   used;
    size_t   size;
    bool     failed;

    struct {
        char     name[ZIP_NAME_MAX + 1];
        uint32_t crc;
        uint32_t packed;
        uint32_t size;
        uint32_t offset;
        uint16_t method;
        uint16_t date, time;
    } entries[ZIP_MAX_ENTRIES];
    size_t count;
};

static bool reserve(struct zip_writer *w, size_t extra)
{
    if (w->failed)
        return false;
    if (w->used + extra <= w->size)
        return true;

    size_t want = w->size ? w->size : 4096;

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

static void put(struct zip_writer *w, const void *bytes, size_t length)
{
    if (!reserve(w, length))
        return;
    memcpy(w->data + w->used, bytes, length);
    w->used += length;
}

static void put16(struct zip_writer *w, uint16_t value)
{
    uint8_t b[2] = { (uint8_t)value, (uint8_t)(value >> 8) };

    put(w, b, 2);
}

static void put32(struct zip_writer *w, uint32_t value)
{
    uint8_t b[4] = { (uint8_t)value, (uint8_t)(value >> 8),
                     (uint8_t)(value >> 16), (uint8_t)(value >> 24) };

    put(w, b, 4);
}

struct zip_writer *zip_begin(void)
{
    return kzalloc(sizeof(struct zip_writer));
}

void zip_abort(struct zip_writer *w)
{
    if (!w)
        return;
    kfree(w->data);
    kfree(w);
}

uint16_t zip_dos_date(uint16_t year, uint8_t month, uint8_t day)
{
    /* MS-DOS zaehlt die Jahre ab 1980 und hat fuer sie sieben Bit -
     * bis 2107 reicht das. */
    if (year < 1980)
        year = 1980;
    return (uint16_t)(((year - 1980) << 9) | ((month & 0x0F) << 5) |
                      (day & 0x1F));
}

uint16_t zip_dos_time(uint8_t hour, uint8_t minute, uint8_t second)
{
    /* Sekunden in Zweierschritten - das Format ist von 1981. */
    return (uint16_t)(((hour & 0x1F) << 11) | ((minute & 0x3F) << 5) |
                      ((second / 2) & 0x1F));
}

bool zip_add(struct zip_writer *w, const char *name,
             const void *data, size_t length, uint16_t date, uint16_t time)
{
    if (!w || w->failed || !name || !name[0])
        return false;
    if (w->count >= ZIP_MAX_ENTRIES)
        return false;

    size_t name_len = strlen(name);

    if (name_len > ZIP_NAME_MAX)
        return false;
    if (length > 0xFFFFFFFFu)
        return false;

    bool is_dir = name[name_len - 1] == '/';
    uint8_t *packed = NULL;
    size_t packed_len = 0;
    uint16_t method = 0;

    if (!is_dir && length > 0) {
        packed = deflate_raw(data, length, &packed_len);

        /* Gepackt wird nur, wenn es sich lohnt. Bei kurzen Dateien
         * kostet DEFLATE mehr, als es bringt - dann kommt der Inhalt
         * unveraendert hinein, und jeder Entpacker ist zufrieden. */
        if (packed && packed_len < length) {
            method = 8;
        } else {
            kfree(packed);
            packed = NULL;
            packed_len = length;
        }
    }

    size_t at = w->count++;

    strlcpy(w->entries[at].name, name, sizeof(w->entries[at].name));
    w->entries[at].crc = length ? crc32(data, length) : 0;
    w->entries[at].size = (uint32_t)length;
    w->entries[at].packed = (uint32_t)packed_len;
    w->entries[at].offset = (uint32_t)w->used;
    w->entries[at].method = method;
    w->entries[at].date = date;
    w->entries[at].time = time;

    put32(w, SIG_LOCAL);
    put16(w, 20);            /* dafuer braucht man Fassung 2.0 */
    put16(w, 0);             /* keine Merkmale                 */
    put16(w, method);
    put16(w, time);
    put16(w, date);
    put32(w, w->entries[at].crc);
    put32(w, (uint32_t)packed_len);
    put32(w, (uint32_t)length);
    put16(w, (uint16_t)name_len);
    put16(w, 0);             /* kein Zusatzfeld                */
    put(w, name, name_len);
    put(w, method == 8 ? (const void *)packed : data, packed_len);

    kfree(packed);
    return !w->failed;
}

void *zip_finish(struct zip_writer *w, size_t *out_length)
{
    if (!w)
        return NULL;

    uint32_t dir_at = (uint32_t)w->used;

    for (size_t i = 0; i < w->count; i++) {
        size_t name_len = strlen(w->entries[i].name);

        put32(w, SIG_CENTRAL);
        put16(w, 20);        /* geschrieben von Fassung 2.0    */
        put16(w, 20);        /* noetig ist Fassung 2.0         */
        put16(w, 0);
        put16(w, w->entries[i].method);
        put16(w, w->entries[i].time);
        put16(w, w->entries[i].date);
        put32(w, w->entries[i].crc);
        put32(w, w->entries[i].packed);
        put32(w, w->entries[i].size);
        put16(w, (uint16_t)name_len);
        put16(w, 0);         /* Zusatzfeld                     */
        put16(w, 0);         /* Anmerkung                      */
        put16(w, 0);         /* Datentraeger                   */
        put16(w, 0);         /* innere Merkmale                */
        put32(w, 0);         /* aeussere Merkmale              */
        put32(w, w->entries[i].offset);
        put(w, w->entries[i].name, name_len);
    }

    uint32_t dir_size = (uint32_t)w->used - dir_at;

    put32(w, SIG_END);
    put16(w, 0);             /* dieser Datentraeger            */
    put16(w, 0);             /* der mit dem Verzeichnis        */
    put16(w, (uint16_t)w->count);
    put16(w, (uint16_t)w->count);
    put32(w, dir_size);
    put32(w, dir_at);
    put16(w, 0);             /* keine Anmerkung                */

    if (w->failed) {
        zip_abort(w);
        return NULL;
    }

    uint8_t *result = w->data;

    *out_length = w->used;
    w->data = NULL;
    kfree(w);
    return result;
}
