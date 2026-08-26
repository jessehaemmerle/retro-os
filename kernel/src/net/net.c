/* net.c - Kern der Netzwerkschicht: Schnittstelle, Abfrage, Adressformate. */

#include "net.h"
#include "arch.h"
#include "kstring.h"

struct netif g_netif;

static uint8_t rx_frame[ETH_FRAME_MAX];

void net_init(void)
{
    memset(&g_netif, 0, sizeof(g_netif));
    strlcpy(g_netif.hostname, "retroos", sizeof(g_netif.hostname));

    if (!e1000_init())
        return;

    g_netif.up = true;
    arp_init();

    /* Adresse per DHCP holen - in den ersten Sekunden nach dem Start. */
    if (dhcp_configure(4000)) {
        char ip[16], gw[16], dns[16];

        ip_format(g_netif.ip, ip, sizeof(ip));
        ip_format(g_netif.gateway, gw, sizeof(gw));
        ip_format(g_netif.dns, dns, sizeof(dns));
        kprintf("Netzwerk    : %s, Gateway %s, DNS %s\n", ip, gw, dns);
        arp_announce();
    } else {
        kprintf("Netzwerk    : keine Adresse per DHCP erhalten\n");
    }
}

void net_poll(void)
{
    if (!g_netif.up)
        return;

    /* Mehrere Rahmen je Durchgang, damit der Ring nicht volllaeuft. */
    for (int i = 0; i < 16; i++) {
        uint16_t length = e1000_receive(rx_frame, sizeof(rx_frame));

        if (length == 0)
            break;
        eth_receive(rx_frame, length);
    }
}

void net_pump(uint32_t timeout_ms)
{
    uint64_t deadline = timer_ms() + timeout_ms;

    while (timer_ms() < deadline)
        net_poll();
}

bool net_ready(void)
{
    return g_netif.up && g_netif.ip != 0;
}

void ip_format(ip_addr_t addr, char *buf, size_t size)
{
    uint32_t host = ntohl(addr);

    ksnprintf(buf, size, "%u.%u.%u.%u",
              (unsigned)((host >> 24) & 0xFF), (unsigned)((host >> 16) & 0xFF),
              (unsigned)((host >> 8) & 0xFF), (unsigned)(host & 0xFF));
}

bool ip_parse(const char *text, ip_addr_t *out)
{
    uint32_t parts[4] = { 0, 0, 0, 0 };
    int index = 0;
    bool digit_seen = false;

    for (const char *p = text; ; p++) {
        if (*p >= '0' && *p <= '9') {
            parts[index] = parts[index] * 10 + (uint32_t)(*p - '0');
            if (parts[index] > 255)
                return false;
            digit_seen = true;
        } else if (*p == '.') {
            if (!digit_seen || ++index > 3)
                return false;
            digit_seen = false;
        } else if (*p == '\0') {
            break;
        } else {
            return false;
        }
    }

    if (index != 3 || !digit_seen)
        return false;

    *out = ip_make((uint8_t)parts[0], (uint8_t)parts[1],
                   (uint8_t)parts[2], (uint8_t)parts[3]);
    return true;
}

void mac_format(const struct mac_addr *mac, char *buf, size_t size)
{
    ksnprintf(buf, size, "%02x:%02x:%02x:%02x:%02x:%02x",
              mac->b[0], mac->b[1], mac->b[2],
              mac->b[3], mac->b[4], mac->b[5]);
}
