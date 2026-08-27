/* net.c - Kern der Netzwerkschicht: Schnittstelle, Abfrage, Adressformate. */

#include "net.h"
#include "nic.h"
#include "arch.h"
#include "kstring.h"
#include "thread.h"

struct netif g_netif;

static uint8_t rx_frame[ETH_FRAME_MAX];
static struct thread *net_thread;
static volatile bool  in_rx;

void net_init(void)
{
    memset(&g_netif, 0, sizeof(g_netif));
    strlcpy(g_netif.hostname, "retroos", sizeof(g_netif.hostname));

    if (!nic_init())
        return;

    g_netif.up = true;
    arp_init();

    /* Erst den Thread starten, der die Karte abfragt - sonst wartet die
     * DHCP-Anfrage auf eine Antwort, die niemand abholt. */
    net_start_thread();

    /* Adresse per DHCP holen - in den ersten Sekunden nach dem Start. */
    if (dhcp_configure(4000)) {
        char ip[16], gw[16], dns[16];

        ip_format(g_netif.ip, ip, sizeof(ip));
        ip_format(g_netif.gateway, gw, sizeof(gw));
        ip_format(g_netif.dns, dns, sizeof(dns));
        kprintf("Netzwerk    : %s, Gateway %s, DNS %s\n", ip, gw, dns);
        arp_announce();

        /* Die Adresse des Gateways gleich lernen - sonst wartet die erste
         * Verbindung auf die Aufloesung. */
        struct mac_addr unused;
        if (g_netif.gateway)
            arp_resolve(g_netif.gateway, &unused);
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
        preempt_disable();

        uint16_t length = nic_receive(rx_frame, sizeof(rx_frame));

        if (length == 0) {
            preempt_enable();
            break;
        }

        in_rx = true;
        eth_receive(rx_frame, length);
        in_rx = false;

        preempt_enable();
    }
}

void net_idle(void)
{
    if (scheduler_running())
        thread_sleep(1);
    else
        net_poll();
}

void net_pump(uint32_t timeout_ms)
{
    uint64_t deadline = timer_ms() + timeout_ms;

    while (timer_ms() < deadline)
        net_idle();
}

void net_lock(void)   { preempt_disable(); }
void net_unlock(void) { preempt_enable(); }

bool net_in_rx_context(void)
{
    return in_rx;
}

/* Der Netz-Thread ist die einzige Stelle, die Rahmen von der Karte holt. */
static void net_thread_entry(void *argument)
{
    UNUSED(argument);

    for (;;) {
        net_poll();
        thread_sleep(1);
    }
}

void net_start_thread(void)
{
    if (!g_netif.up || net_thread)
        return;

    net_thread = thread_create("netz", net_thread_entry, NULL, PRIO_NORMAL);
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
