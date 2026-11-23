/* hidparse.c - siehe hidparse.h.
 *
 * Eine Berichtsbeschreibung ist eine Folge von Elementen. Jedes beginnt
 * mit einem Praefixbyte: zwei Bit Laenge der Nutzdaten (0, 1, 2 oder
 * 4 Byte), zwei Bit Art, vier Bit Kennung.
 *
 *   Haupt-Elemente (Art 0) tun etwas: "hier liegt ein Eingabefeld".
 *   Globale (Art 1) stellen etwas ein, das danach gilt: Seite,
 *     Wertebereich, Breite, Anzahl, Berichtsnummer.
 *   Lokale (Art 2) gelten nur fuer das naechste Haupt-Element:
 *     welche Verwendung das Feld hat.
 *
 * Gelesen wird also mit einem Zustand, der sich aufbaut, und jedes
 * "Eingabe" nimmt ihn mit, um zu sagen: Ab Bit so-und-so liegen so
 * viele Felder dieser Breite mit diesen Verwendungen. Mehr als das
 * braucht ein Zeiger nicht zu wissen.
 */

#include "hidparse.h"
#include "kstring.h"

#define PAGE_DESKTOP 0x01
#define PAGE_BUTTON  0x09

#define USAGE_X      0x30
#define USAGE_Y      0x31
#define USAGE_WHEEL  0x38

#define MAX_USAGES   32
#define MAX_IDS      8
/* Mehr Felder in einem einzigen Element gibt es bei keinem Geraet; wer
 * mehr behauptet, redet Unsinn - und soll weder die Schleife noch den
 * Bitzaehler zum Ueberlaufen bringen. */
#define MAX_COUNT    1024
#define NO_USAGE     0xFFFFFFFFu

struct state {
    /* global */
    uint32_t page;
    int32_t  lmin, lmax;
    uint32_t lmax_raw;       /* das Maximum ohne Vorzeichen gelesen */
    uint32_t size, count;

    /* lokal - nach jedem Haupt-Element wieder leer */
    uint32_t usage[MAX_USAGES];
    size_t   usage_count;
    uint32_t umin, umax;
    bool     have_range;
};

static void clear_local(struct state *s)
{
    s->usage_count = 0;
    s->have_range = false;
    s->umin = s->umax = 0;
}

/* Liest die Nutzdaten eines Elements: ohne Vorzeichen und mit. */
static uint32_t item_unsigned(const uint8_t *p, unsigned size)
{
    uint32_t v = 0;

    for (unsigned i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int32_t item_signed(const uint8_t *p, unsigned size)
{
    uint32_t v = item_unsigned(p, size);

    if (size == 1)
        return (int8_t)v;
    if (size == 2)
        return (int16_t)v;
    return (int32_t)v;
}

/* Die Verwendung des i-ten Feldes in einem Element.
 *
 * Gibt es weniger Verwendungen als Felder, gilt die letzte fuer den
 * Rest - so beschreibt man "drei Achsen" mit einer Liste. Bei einem
 * Bereich (Minimum bis Maximum) wird gezaehlt; darueber hinaus gibt es
 * nichts. */
static uint32_t usage_for(const struct state *s, uint32_t i)
{
    uint32_t u;

    if (s->have_range) {
        u = s->umin + i;
        if (u > s->umax)
            return NO_USAGE;
    } else if (s->usage_count) {
        u = s->usage[i < s->usage_count ? i : s->usage_count - 1];
    } else {
        return NO_USAGE;
    }

    /* Eine Verwendung in vier Byte bringt ihre Seite selbst mit. */
    if (u > 0xFFFF)
        return u;
    return (s->page << 16) | u;
}

/* Die Berichtsnummern. Jede hat ihren eigenen Bitzaehler: Berichte
 * verschiedener Nummern liegen ja nicht hintereinander, sondern sind
 * verschiedene Berichte. */
struct ids {
    uint32_t id[MAX_IDS];
    uint32_t pos[MAX_IDS];
    size_t   count;
};

static uint32_t ids_lookup(const struct ids *t, uint32_t id)
{
    for (size_t i = 0; i < t->count; i++) {
        if (t->id[i] == id)
            return t->pos[i];
    }
    return 8;       /* vorn steht die Nummer selbst, ein Byte */
}

static void ids_store(struct ids *t, uint32_t id, uint32_t pos)
{
    for (size_t i = 0; i < t->count; i++) {
        if (t->id[i] == id) {
            t->pos[i] = pos;
            return;
        }
    }
    if (t->count < MAX_IDS) {
        t->id[t->count] = id;
        t->pos[t->count] = pos;
        t->count++;
    }
}

static void set_field(struct hid_field *f, uint32_t bit, uint32_t size,
                      const struct state *s, bool relative)
{
    if (f->present)
        return;                 /* das erste gilt */

    f->present = true;
    f->bit = (uint16_t)bit;
    f->size = (uint8_t)size;
    f->min = s->lmin;
    f->max = s->lmax;
    f->is_signed = s->lmin < 0;
    f->relative = relative;
}

static void note_end(struct hid_pointer *p, uint32_t bit, uint32_t size)
{
    uint32_t end = bit + size;

    if (end > p->report_bits)
        p->report_bits = (uint16_t)end;
}

bool hid_parse_pointer(const uint8_t *desc, size_t length,
                       struct hid_pointer *out)
{
    struct state s;
    struct ids ids;
    uint32_t cur_id = 0;
    uint32_t pos = 0;
    bool any_id = false;
    bool bound = false;
    uint32_t bound_id = 0;

    if (!out)
        return false;

    memset(out, 0, sizeof(*out));
    for (int i = 0; i < HID_MAX_BUTTONS; i++)
        out->button_bit[i] = -1;

    if (!desc || length == 0)
        return false;

    memset(&s, 0, sizeof(s));
    memset(&ids, 0, sizeof(ids));

    size_t at = 0;

    while (at < length) {
        uint8_t prefix = desc[at];

        /* Ein langes Element: Kennzeichen 0xFE, dann Laenge und Kennung.
         * Kein Geraet benutzt es, aber ueberspringen muss man es
         * koennen. */
        if (prefix == 0xFE) {
            if (at + 2 >= length)
                break;
            at += 3u + desc[at + 1];
            continue;
        }

        unsigned size = prefix & 3;
        unsigned type = (prefix >> 2) & 3;
        unsigned tag = prefix >> 4;

        if (size == 3)
            size = 4;

        /* Ein abgeschnittenes Element ist das Ende. */
        if (at + 1 + size > length)
            break;

        const uint8_t *data = &desc[at + 1];

        at += 1 + size;

        if (type == 1) {                              /* global */
            switch (tag) {
            case 0x0: s.page = item_unsigned(data, size); break;
            case 0x1: s.lmin = item_signed(data, size);   break;
            case 0x2:
                s.lmax = item_signed(data, size);
                s.lmax_raw = item_unsigned(data, size);
                break;
            case 0x7: s.size = item_unsigned(data, size); break;
            case 0x9: s.count = item_unsigned(data, size); break;
            case 0x8: {
                uint32_t id = item_unsigned(data, size);

                if (!any_id) {
                    any_id = true;
                    pos = 8;
                } else {
                    ids_store(&ids, cur_id, pos);
                    pos = ids_lookup(&ids, id);
                }
                cur_id = id;
                break;
            }
            default:
                break;
            }
            continue;
        }

        if (type == 2) {                              /* lokal */
            uint32_t value = item_unsigned(data, size);

            switch (tag) {
            case 0x0:
                /* Mehr als wir fassen: Der Rest wird ignoriert. */
                if (s.usage_count < MAX_USAGES)
                    s.usage[s.usage_count++] = value;
                break;
            case 0x1: s.umin = value; s.have_range = true; break;
            case 0x2: s.umax = value; s.have_range = true; break;
            default:  break;
            }
            continue;
        }

        if (type != 0)
            continue;                                 /* reserviert */

        /* --- Haupt-Elemente ---------------------------------------- */

        if (tag == 0x8) {                             /* Eingabe */
            uint32_t flags = item_unsigned(data, size);
            uint32_t count = s.count > MAX_COUNT ? MAX_COUNT : s.count;
            uint32_t width = s.size > 32 ? 0 : s.size;

            /* Ein Maximum, das als 0xFF in einem Byte steht, ist
             * 255 und nicht -1: Wer bei null anfaengt, kann nicht
             * rueckwaerts zaehlen. Ohne diese Zeile waere der Bereich
             * eines Zeigers leer, und er stuende still. */
            int32_t lmax = s.lmax;

            if (s.lmin >= 0 && lmax < 0)
                lmax = (int32_t)(s.lmax_raw & 0x7FFFFFFFu);

            struct state eff = s;

            eff.lmax = lmax;

            /* Konstant (Bit 0) ist Fuellmaterial, ein Feld ohne
             * Variable (Bit 1) eine Liste - beides nimmt Platz und
             * sagt uns nichts. */
            if (!(flags & 1) && (flags & 2) && width) {
                bool relative = (flags & 4) != 0;

                for (uint32_t i = 0; i < count; i++) {
                    uint32_t usage = usage_for(&s, i);
                    uint32_t bit = pos + i * width;

                    if (usage == NO_USAGE || bit + width > 0xFFFF)
                        continue;

                    uint32_t page = usage >> 16;
                    uint32_t u = usage & 0xFFFF;
                    bool take = false;

                    /* Alle Felder eines Zeigers liegen im selben
                     * Bericht - das erste bindet die Nummer. */
                    if (page == PAGE_DESKTOP &&
                        (u == USAGE_X || u == USAGE_Y || u == USAGE_WHEEL))
                        take = true;
                    if (page == PAGE_BUTTON && u >= 1 && u <= HID_MAX_BUTTONS)
                        take = true;
                    if (!take)
                        continue;

                    if (!bound) {
                        bound = true;
                        bound_id = cur_id;
                        out->has_id = any_id;
                        out->id = (uint8_t)cur_id;
                    } else if (bound_id != cur_id) {
                        continue;
                    }

                    if (page == PAGE_BUTTON) {
                        if (out->button_bit[u - 1] < 0)
                            out->button_bit[u - 1] = (int16_t)bit;
                    } else if (u == USAGE_X) {
                        set_field(&out->x, bit, width, &eff, relative);
                    } else if (u == USAGE_Y) {
                        set_field(&out->y, bit, width, &eff, relative);
                    } else {
                        set_field(&out->wheel, bit, width, &eff, relative);
                    }
                    note_end(out, bit, width);
                }
            }

            pos += count * width;
        }

        /* Ausgabe, Merkmal, Sammlung und Ende einer Sammlung: Die
         * lokalen Einstellungen gelten nur bis hierher. */
        clear_local(&s);
    }

    return out->x.present && out->y.present;
}

int32_t hid_get_bits(const uint8_t *report, size_t bytes, uint16_t bit,
                     uint8_t size, bool is_signed)
{
    if (!report || size == 0 || size > 32)
        return 0;
    if ((uint32_t)bit + size > (uint32_t)bytes * 8)
        return 0;

    uint64_t v = 0;

    for (unsigned i = 0; i < size; i++) {
        unsigned at = bit + i;

        v |= (uint64_t)((report[at >> 3] >> (at & 7)) & 1u) << i;
    }

    if (is_signed && size < 32 && (v & (1ull << (size - 1))))
        v -= 1ull << size;

    return (int32_t)v;
}

int32_t hid_scale(int32_t value, int32_t min, int32_t max, int32_t span)
{
    if (max <= min || span <= 0)
        return 0;

    if (value < min)
        value = min;
    if (value > max)
        value = max;

    return (int32_t)(((int64_t)value - min) * span / ((int64_t)max - min));
}
