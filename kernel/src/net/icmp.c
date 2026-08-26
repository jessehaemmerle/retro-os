/* icmp.c - Echoanfragen beantworten und selbst welche stellen ("ping"). */

#include "net.h"
#include "arch.h"
#include "kstring.h"

struct icmp_header {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} PACKED;

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

static uint16_t ping_id = 0x5245;   /* "RE" */
static uint16_t ping_sequence;
static volatile bool     reply_seen;
static volatile uint16_t reply_sequence;

void icmp_receive(ip_addr_t src, const uint8_t *data, uint16_t length)
{
    if (length < sizeof(struct icmp_header))
        return;

    const struct icmp_header *header = (const struct icmp_header *)data;

    if (header->type == ICMP_ECHO_REQUEST) {
        /* Antwort ist dieselbe Nachricht mit anderem Typ. */
        uint8_t reply[ETH_MTU];

        if (length > sizeof(reply))
            return;

        memcpy(reply, data, length);
        struct icmp_header *out = (struct icmp_header *)reply;

        out->type     = ICMP_ECHO_REPLY;
        out->checksum = 0;
        out->checksum = htons(ip_checksum(reply, length));

        ip_send(src, IP_PROTO_ICMP, reply, length);
        return;
    }

    if (header->type == ICMP_ECHO_REPLY && ntohs(header->id) == ping_id) {
        reply_sequence = ntohs(header->sequence);
        reply_seen = true;
    }
}

bool icmp_ping(ip_addr_t target, uint32_t timeout_ms, uint32_t *rtt_ms)
{
    uint8_t packet[64];
    struct icmp_header *header = (struct icmp_header *)packet;
    uint16_t sequence = ++ping_sequence;

    memset(packet, 0, sizeof(packet));
    header->type     = ICMP_ECHO_REQUEST;
    header->id       = htons(ping_id);
    header->sequence = htons(sequence);

    for (size_t i = sizeof(*header); i < sizeof(packet); i++)
        packet[i] = (uint8_t)('a' + (i % 26));

    header->checksum = htons(ip_checksum(packet, sizeof(packet)));

    reply_seen = false;
    uint64_t start = timer_ms();

    if (!ip_send(target, IP_PROTO_ICMP, packet, sizeof(packet)))
        return false;

    while (timer_ms() - start < timeout_ms) {
        net_poll();

        if (reply_seen && reply_sequence == sequence) {
            if (rtt_ms)
                *rtt_ms = (uint32_t)(timer_ms() - start);
            return true;
        }
    }
    return false;
}
