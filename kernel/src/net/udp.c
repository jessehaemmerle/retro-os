/* udp.c - Datagramme ohne Verbindung. Grundlage fuer DHCP und DNS. */

#include "net.h"
#include "kstring.h"

#define UDP_MAX_LISTENERS 8

struct udp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} PACKED;

struct listener {
    uint16_t      port;
    udp_handler_t handler;
};

static struct listener listeners[UDP_MAX_LISTENERS];

bool udp_listen(uint16_t port, udp_handler_t handler)
{
    for (size_t i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (listeners[i].port == 0 || listeners[i].port == port) {
            listeners[i].port    = port;
            listeners[i].handler = handler;
            return true;
        }
    }
    return false;
}

void udp_unlisten(uint16_t port)
{
    for (size_t i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (listeners[i].port == port) {
            listeners[i].port    = 0;
            listeners[i].handler = NULL;
        }
    }
}

bool udp_send(ip_addr_t dst, uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t length)
{
    uint8_t packet[ETH_MTU];

    if (sizeof(struct udp_header) + length > ETH_MTU)
        return false;

    struct udp_header *header = (struct udp_header *)packet;

    header->src_port = htons(src_port);
    header->dst_port = htons(dst_port);
    header->length   = htons((uint16_t)(sizeof(*header) + length));
    header->checksum = 0;      /* bei IPv4 zulaessig und hier ausreichend */

    memcpy(packet + sizeof(*header), data, length);

    return ip_send(dst, IP_PROTO_UDP, packet,
                   (uint16_t)(sizeof(*header) + length));
}

void udp_receive(ip_addr_t src, const uint8_t *data, uint16_t length)
{
    if (length < sizeof(struct udp_header))
        return;

    const struct udp_header *header = (const struct udp_header *)data;
    uint16_t declared = ntohs(header->length);

    if (declared < sizeof(*header) || declared > length)
        return;

    uint16_t port = ntohs(header->dst_port);
    const uint8_t *payload = data + sizeof(*header);
    uint16_t payload_length = (uint16_t)(declared - sizeof(*header));

    for (size_t i = 0; i < UDP_MAX_LISTENERS; i++) {
        if (listeners[i].port == port && listeners[i].handler) {
            listeners[i].handler(src, ntohs(header->src_port),
                                 payload, payload_length);
            return;
        }
    }
}
