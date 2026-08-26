/* dhcp.c - holt Adresse, Netzmaske, Gateway und Namensserver.
 *
 * Ablauf: DISCOVER als Rundruf, der Server antwortet mit OFFER, wir bitten
 * mit REQUEST um genau dieses Angebot, der Server bestaetigt mit ACK.
 */

#include "net.h"
#include "arch.h"
#include "kstring.h"

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

struct dhcp_message {
    uint8_t  op;             /* 1 = Anfrage, 2 = Antwort */
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[312];
} PACKED;

static volatile bool      got_offer, got_ack;
static ip_addr_t          offered_ip, server_ip;
static ip_addr_t          lease_netmask, lease_gateway, lease_dns;
static uint32_t           transaction_id;

static uint8_t *put_option(uint8_t *p, uint8_t code, uint8_t length,
                           const void *data)
{
    *p++ = code;
    *p++ = length;
    memcpy(p, data, length);
    return p + length;
}

static const uint8_t *find_option(const struct dhcp_message *msg, uint8_t code,
                                  uint8_t *out_length)
{
    const uint8_t *p = msg->options;
    const uint8_t *end = msg->options + sizeof(msg->options);

    while (p < end && *p != 0xFF) {
        if (*p == 0) {                 /* Fuellbyte */
            p++;
            continue;
        }
        uint8_t option = p[0];
        uint8_t length = p[1];

        if (p + 2 + length > end)
            break;
        if (option == code) {
            if (out_length)
                *out_length = length;
            return p + 2;
        }
        p += 2 + length;
    }
    return NULL;
}

static void build_common(struct dhcp_message *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->op    = 1;
    msg->htype = 1;
    msg->hlen  = ETH_ALEN;
    msg->xid   = transaction_id;
    msg->flags = htons(0x8000);        /* Antwort bitte als Rundruf */
    msg->magic = htonl(0x63825363);
    memcpy(msg->chaddr, g_netif.mac.b, ETH_ALEN);
}

static void handle(ip_addr_t src, uint16_t src_port, const uint8_t *data,
                   uint16_t length)
{
    UNUSED(src);
    UNUSED(src_port);

    if (length < 240)
        return;

    const struct dhcp_message *msg = (const struct dhcp_message *)data;

    if (msg->op != 2 || msg->xid != transaction_id)
        return;

    uint8_t len = 0;
    const uint8_t *type = find_option(msg, 53, &len);
    if (!type || len < 1)
        return;

    const uint8_t *value;

    switch (*type) {
    case DHCP_OFFER:
        offered_ip = msg->yiaddr;
        value = find_option(msg, 54, &len);
        server_ip = (value && len == 4) ? *(const uint32_t *)value : src;
        got_offer = true;
        break;

    case DHCP_ACK:
        offered_ip = msg->yiaddr;

        value = find_option(msg, 1, &len);
        if (value && len == 4)
            memcpy(&lease_netmask, value, 4);

        value = find_option(msg, 3, &len);
        if (value && len >= 4)
            memcpy(&lease_gateway, value, 4);

        value = find_option(msg, 6, &len);
        if (value && len >= 4)
            memcpy(&lease_dns, value, 4);

        got_ack = true;
        break;

    default:
        break;
    }
}

bool dhcp_configure(uint32_t timeout_ms)
{
    struct dhcp_message msg;
    uint8_t *p;

    transaction_id = (uint32_t)(timer_ticks() * 2654435761u) | 1;
    got_offer = got_ack = false;
    offered_ip = server_ip = 0;
    lease_netmask = lease_gateway = lease_dns = 0;

    udp_listen(DHCP_CLIENT_PORT, handle);

    /* Solange keine Adresse steht, gehen Pakete an alle. */
    g_netif.ip = 0;
    g_netif.netmask = 0;

    /* --- DISCOVER --- */
    build_common(&msg);
    p = msg.options;
    uint8_t type = DHCP_DISCOVER;
    p = put_option(p, 53, 1, &type);
    static const uint8_t wanted[] = { 1, 3, 6, 15 };   /* Maske, Router, DNS, Domain */
    p = put_option(p, 55, sizeof(wanted), wanted);
    p = put_option(p, 12, (uint8_t)strlen(g_netif.hostname), g_netif.hostname);
    *p++ = 0xFF;

    uint64_t deadline = timer_ms() + timeout_ms;

    for (int attempt = 0; attempt < 4 && !got_offer; attempt++) {
        udp_send(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                 &msg, (uint16_t)(240 + (p - msg.options)));

        uint64_t wait_until = timer_ms() + 500;
        while (timer_ms() < wait_until && !got_offer)
            net_poll();

        if (timer_ms() > deadline)
            break;
    }

    if (!got_offer) {
        udp_unlisten(DHCP_CLIENT_PORT);
        return false;
    }

    /* --- REQUEST --- */
    build_common(&msg);
    p = msg.options;
    type = DHCP_REQUEST;
    p = put_option(p, 53, 1, &type);
    p = put_option(p, 50, 4, &offered_ip);
    if (server_ip)
        p = put_option(p, 54, 4, &server_ip);
    p = put_option(p, 55, sizeof(wanted), wanted);
    p = put_option(p, 12, (uint8_t)strlen(g_netif.hostname), g_netif.hostname);
    *p++ = 0xFF;

    for (int attempt = 0; attempt < 4 && !got_ack; attempt++) {
        udp_send(0xFFFFFFFFu, DHCP_CLIENT_PORT, DHCP_SERVER_PORT,
                 &msg, (uint16_t)(240 + (p - msg.options)));

        uint64_t wait_until = timer_ms() + 500;
        while (timer_ms() < wait_until && !got_ack)
            net_poll();

        if (timer_ms() > deadline)
            break;
    }

    udp_unlisten(DHCP_CLIENT_PORT);

    if (!got_ack)
        return false;

    g_netif.ip      = offered_ip;
    g_netif.netmask = lease_netmask ? lease_netmask : ip_make(255, 255, 255, 0);
    g_netif.gateway = lease_gateway;
    g_netif.dns     = lease_dns ? lease_dns : lease_gateway;
    return true;
}
