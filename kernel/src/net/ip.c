/* ip.c - IPv4: Kopfdaten, Pruefsumme, Verteilung an die Transportschicht. */

#include "net.h"
#include "kstring.h"

struct ip_header {
    uint8_t  version_ihl;
    uint8_t  tos;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} PACKED;

static uint16_t next_id = 1;

/* Einerkomplement-Summe ueber 16-Bit-Woerter - die Pruefsumme des
 * Internetprotokolls, seit den Siebzigern unveraendert. */
uint16_t ip_checksum(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint32_t sum = 0;

    while (length > 1) {
        sum += (uint32_t)((bytes[0] << 8) | bytes[1]);
        bytes += 2;
        length -= 2;
    }
    if (length == 1)
        sum += (uint32_t)(bytes[0] << 8);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

bool ip_send(ip_addr_t dst, uint8_t protocol, const void *payload,
             uint16_t length)
{
    uint8_t packet[ETH_MTU];

    if (sizeof(struct ip_header) + length > ETH_MTU)
        return false;

    struct mac_addr target;
    if (!arp_resolve(dst, &target))
        return false;

    struct ip_header *header = (struct ip_header *)packet;

    memset(header, 0, sizeof(*header));
    header->version_ihl  = 0x45;              /* IPv4, 20 Byte Kopf */
    header->total_length = htons((uint16_t)(sizeof(*header) + length));
    header->id           = htons(next_id++);
    header->flags_fragment = htons(0x4000);   /* nicht zerteilen    */
    header->ttl          = 64;
    header->protocol     = protocol;
    header->src          = g_netif.ip;
    header->dst          = dst;
    header->checksum     = htons(ip_checksum(header, sizeof(*header)));

    memcpy(packet + sizeof(*header), payload, length);

    return eth_send(&target, ETHERTYPE_IP, packet,
                    (uint16_t)(sizeof(*header) + length));
}

void ip_receive(const uint8_t *data, uint16_t length)
{
    if (length < sizeof(struct ip_header))
        return;

    const struct ip_header *header = (const struct ip_header *)data;

    if ((header->version_ihl >> 4) != 4)
        return;

    uint16_t header_length = (uint16_t)((header->version_ihl & 0x0F) * 4);
    uint16_t total = ntohs(header->total_length);

    if (header_length < sizeof(*header) || total > length || total < header_length)
        return;

    /* Pakete an fremde Adressen ignorieren - ausser Rundrufen, die etwa
     * die DHCP-Antwort tragen, solange wir noch keine Adresse haben. */
    if (g_netif.ip != 0 && header->dst != g_netif.ip &&
        header->dst != 0xFFFFFFFFu &&
        (header->dst | ~g_netif.netmask) != 0xFFFFFFFFu)
        return;

    const uint8_t *payload = data + header_length;
    uint16_t payload_length = (uint16_t)(total - header_length);

    switch (header->protocol) {
    case IP_PROTO_ICMP:
        icmp_receive(header->src, payload, payload_length);
        break;
    case IP_PROTO_UDP:
        udp_receive(header->src, payload, payload_length);
        break;
    case IP_PROTO_TCP:
        tcp_receive_segment(header->src, payload, payload_length);
        break;
    default:
        break;
    }
}
