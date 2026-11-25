/* vmxnet3hw.c - siehe vmxnet3hw.h. */

#include "vmxnet3hw.h"
#include "kstring.h"

void vmx_ring_init(struct vmx_ring *ring, uint32_t size)
{
    ring->size = size;
    ring->next = 0;
    ring->gen = 1;
}

void vmx_ring_advance(struct vmx_ring *ring)
{
    ring->next++;
    if (ring->next >= ring->size) {
        ring->next = 0;
        ring->gen ^= 1;
    }
}

bool vmx_ring_size_ok(uint32_t size)
{
    return size != 0 && size <= 4096 && size % 32 == 0;
}

void vmx_tx_desc(uint32_t d[4], uint64_t addr, uint32_t length, uint8_t gen)
{
    d[0] = (uint32_t)addr;
    d[1] = (uint32_t)(addr >> 32);
    /* Laenge 0-13, Generation 14, Deskriptorart 16 (0 = normal). */
    d[2] = (length & 0x3FFFu) | ((uint32_t)(gen & 1) << 14);
    /* Ende des Rahmens 12, Meldung erbeten 13. */
    d[3] = (1u << 12) | (1u << 13);
}

void vmx_rx_desc(uint32_t d[4], uint64_t addr, uint32_t length, uint8_t gen)
{
    d[0] = (uint32_t)addr;
    d[1] = (uint32_t)(addr >> 32);
    /* Laenge 0-13, Pufferart 14 (0 = Kopf), Generation 31. */
    d[2] = (length & 0x3FFFu) | ((uint32_t)(gen & 1) << 31);
    d[3] = 0;
}

uint8_t vmx_comp_gen(const uint32_t d[4])
{
    return (uint8_t)(d[3] >> 31);
}

void vmx_rx_comp_decode(const uint32_t d[4], struct vmx_rx_comp *out)
{
    out->index = d[0] & 0xFFFu;
    out->eop = (d[0] >> 14) & 1u;
    out->sop = (d[0] >> 15) & 1u;
    out->queue_id = (d[0] >> 16) & 0x3FFu;
    out->length = d[2] & 0x3FFFu;
    out->error = (d[2] >> 14) & 1u;
}

uint32_t vmx_tx_comp_index(const uint32_t d[4])
{
    return d[0] & 0xFFFu;
}

uint32_t vmx_gos_word(void)
{
    /* Bits 0-1: 2 = 64 Bit; Bits 2-5: Art 1 = Linux; Bits 6-21: Version. */
    return 2u | (1u << 2) | (1u << 6);
}

bool vmx_link_up(uint32_t result)
{
    return (result & 1u) != 0;
}

uint32_t vmx_link_speed(uint32_t result)
{
    return result >> 16;
}
