/* asn1.c - so viel DER, wie zum Lesen eines Zertifikats noetig ist.
 *
 * DER schreibt jeden Wert als drei Teile: eine Kennung (was es ist), eine
 * Laenge und den Inhalt. Ist der Wert eine Folge, steht darin wieder
 * dasselbe Muster. Mehr Regeln braucht man nicht, um sich durch ein
 * Zertifikat zu bewegen.
 */

#include "pki.h"

void der_start(struct der *reader, const uint8_t *data, size_t length)
{
    reader->data = data;
    reader->length = length;
    reader->position = 0;
}

bool der_next(struct der *reader, struct der_value *out)
{
    if (reader->position + 2 > reader->length)
        return false;

    const uint8_t *p = reader->data + reader->position;
    size_t left = reader->length - reader->position;
    size_t used = 0;

    out->full = p;
    out->tag = p[used++];

    /* Kennungen ueber 30 werden mehrbytig kodiert - kommt hier nicht vor. */
    if ((out->tag & 0x1F) == 0x1F)
        return false;

    if (used >= left)
        return false;

    uint8_t first = p[used++];
    size_t length;

    if (first < 0x80) {
        length = first;
    } else {
        size_t count = first & 0x7F;

        if (count == 0 || count > 4 || used + count > left)
            return false;

        length = 0;
        for (size_t i = 0; i < count; i++)
            length = (length << 8) | p[used++];
    }

    if (used + length > left)
        return false;

    out->content = p + used;
    out->length = length;
    out->full_length = used + length;

    reader->position += out->full_length;
    return true;
}

bool der_enter(const struct der_value *value, struct der *out)
{
    /* Nur zusammengesetzte Werte haben einen Inhalt aus weiteren Werten. */
    if (!(value->tag & 0x20))
        return false;

    der_start(out, value->content, value->length);
    return true;
}
