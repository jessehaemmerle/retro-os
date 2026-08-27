/* crc32.c - die Pruefsumme, die GPT und PNG benutzen.
 *
 * Es ist immer dieselbe: Polynom 0xEDB88320, rueckwaerts gerechnet, mit
 * lauter Einsen begonnen und am Ende umgedreht. Die Tabelle wird beim
 * ersten Aufruf angelegt - so steht sie nicht als Kilobyte im
 * Kernelabbild herum.
 */

#include "kstring.h"

static uint32_t table[256];
static bool     table_ready;

static void build_table(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t value = i;

        for (int bit = 0; bit < 8; bit++)
            value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
        table[i] = value;
    }
    table_ready = true;
}

uint32_t crc32_update(uint32_t crc, const void *data, size_t length)
{
    const uint8_t *bytes = data;

    if (!table_ready)
        build_table();

    crc = ~crc;
    for (size_t i = 0; i < length; i++)
        crc = table[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t crc32(const void *data, size_t length)
{
    return crc32_update(0, data, length);
}
