/* ethernet.c - Rahmen zusammensetzen und eingehende Rahmen verteilen. */

#include "net.h"
#include "kstring.h"

struct eth_header {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;
} PACKED;

static const struct mac_addr broadcast = {
    { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
};

const struct mac_addr *eth_broadcast(void)
{
    return &broadcast;
}

bool eth_send(const struct mac_addr *dst, uint16_t ethertype,
              const void *payload, uint16_t length)
{
    uint8_t frame[ETH_FRAME_MAX];

    if (length > ETH_MTU)
        return false;

    struct eth_header *header = (struct eth_header *)frame;

    memcpy(header->dst, dst->b, ETH_ALEN);
    memcpy(header->src, g_netif.mac.b, ETH_ALEN);
    header->ethertype = htons(ethertype);

    memcpy(frame + sizeof(*header), payload, length);

    /* Rahmen unter 60 Byte muessen aufgefuellt werden. */
    uint16_t total = (uint16_t)(sizeof(*header) + length);
    if (total < 60) {
        memset(frame + total, 0, (size_t)(60 - total));
        total = 60;
    }

    return e1000_send(frame, total);
}

void eth_receive(const uint8_t *frame, uint16_t length)
{
    if (length < sizeof(struct eth_header))
        return;

    const struct eth_header *header = (const struct eth_header *)frame;

    /* Nur eigene Rahmen und Rundrufe beachten. */
    bool for_us = memcmp(header->dst, g_netif.mac.b, ETH_ALEN) == 0;
    bool is_broadcast = memcmp(header->dst, broadcast.b, ETH_ALEN) == 0;

    if (!for_us && !is_broadcast)
        return;

    const uint8_t *payload = frame + sizeof(*header);
    uint16_t payload_length = (uint16_t)(length - sizeof(*header));

    switch (ntohs(header->ethertype)) {
    case ETHERTYPE_ARP:
        arp_receive(payload, payload_length);
        break;
    case ETHERTYPE_IP:
        ip_receive(payload, payload_length);
        break;
    default:
        break;
    }
}
