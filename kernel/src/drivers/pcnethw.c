/* pcnethw.c - siehe pcnethw.h. */

#include "pcnethw.h"
#include "kstring.h"

uint16_t pcnet_buffer_length(uint32_t bytes)
{
    if (bytes == 0 || bytes > 4095)
        return 0;
    return (uint16_t)(0xF000u | ((0x1000u - bytes) & 0x0FFFu));
}

int pcnet_ring_log2(uint32_t count)
{
    for (int i = 0; i <= 9; i++) {
        if (count == (1u << i))
            return i;
    }
    return -1;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

void pcnet_build_init(uint8_t out[PCNET_INIT_BYTES], const uint8_t mac[6],
                      int rx_log2, int tx_log2,
                      uint32_t rx_ring_phys, uint32_t tx_ring_phys)
{
    memset(out, 0, PCNET_INIT_BYTES);

    put16(out + 0, 0);                              /* Modus              */
    out[2] = (uint8_t)((rx_log2 & 0x0F) << 4);      /* Laenge Empfangsring */
    out[3] = (uint8_t)((tx_log2 & 0x0F) << 4);      /* Laenge Sendering    */
    memcpy(out + 4, mac, 6);
    /* Bytes 10 und 11 sind reserviert */
    for (int i = 0; i < 8; i++)
        out[12 + i] = 0xFF;                         /* Mehrfachfilter     */
    put32(out + 20, rx_ring_phys);
    put32(out + 24, tx_ring_phys);
}

bool pcnet_rx_ready(uint16_t status)
{
    return (status & PCNET_DESC_OWN) == 0;
}

bool pcnet_rx_usable(uint16_t status)
{
    if (status & (PCNET_DESC_OWN | PCNET_DESC_ERR))
        return false;
    return (status & (PCNET_DESC_STP | PCNET_DESC_ENP)) ==
           (PCNET_DESC_STP | PCNET_DESC_ENP);
}

uint32_t pcnet_rx_payload(uint32_t message)
{
    uint32_t length = message & 0x0FFFu;

    return length > 4 ? length - 4 : 0;
}
