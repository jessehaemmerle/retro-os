/* arp.c - Zuordnung von IP- zu Hardware-Adressen.
 *
 * Bevor ein IP-Paket auf die Leitung darf, muss die Hardware-Adresse des
 * naechsten Rechners bekannt sein. Dafuer wird gefragt ("wer hat diese
 * IP?") und die Antwort in einer kleinen Tabelle gemerkt.
 */

#include "net.h"
#include "arch.h"
#include "kstring.h"

#define ARP_CACHE_SIZE  16
#define ARP_TIMEOUT_MS  1500

struct arp_packet {
    uint16_t hardware_type;
    uint16_t protocol_type;
    uint8_t  hardware_size;
    uint8_t  protocol_size;
    uint16_t opcode;
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;
} PACKED;

struct arp_entry {
    ip_addr_t       ip;
    struct mac_addr mac;
    uint64_t        learned_ms;
    bool            valid;
};

static struct arp_entry cache[ARP_CACHE_SIZE];
static size_t           next_slot;

const struct mac_addr *eth_broadcast(void);

void arp_init(void)
{
    memset(cache, 0, sizeof(cache));
    next_slot = 0;
}

static void cache_put(ip_addr_t ip, const struct mac_addr *mac)
{
    for (size_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].ip == ip) {
            cache[i].mac = *mac;
            cache[i].learned_ms = timer_ms();
            return;
        }
    }

    struct arp_entry *e = &cache[next_slot];

    next_slot = (next_slot + 1) % ARP_CACHE_SIZE;
    e->ip = ip;
    e->mac = *mac;
    e->learned_ms = timer_ms();
    e->valid = true;
}

static bool cache_get(ip_addr_t ip, struct mac_addr *out)
{
    for (size_t i = 0; i < ARP_CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].ip == ip) {
            *out = cache[i].mac;
            return true;
        }
    }
    return false;
}

static void send_packet(uint16_t opcode, const struct mac_addr *target_mac,
                        ip_addr_t target_ip)
{
    struct arp_packet packet;

    memset(&packet, 0, sizeof(packet));
    packet.hardware_type = htons(1);            /* Ethernet   */
    packet.protocol_type = htons(ETHERTYPE_IP);
    packet.hardware_size = ETH_ALEN;
    packet.protocol_size = 4;
    packet.opcode        = htons(opcode);
    memcpy(packet.sender_mac, g_netif.mac.b, ETH_ALEN);
    packet.sender_ip = g_netif.ip;
    memcpy(packet.target_mac, target_mac->b, ETH_ALEN);
    packet.target_ip = target_ip;

    eth_send(opcode == 1 ? eth_broadcast() : target_mac,
             ETHERTYPE_ARP, &packet, sizeof(packet));
}

void arp_announce(void)
{
    /* Kostenlose Auskunft: sagt allen im Netz, wem diese Adresse gehoert. */
    send_packet(1, eth_broadcast(), g_netif.ip);
}

void arp_receive(const uint8_t *data, uint16_t length)
{
    if (length < sizeof(struct arp_packet))
        return;

    const struct arp_packet *packet = (const struct arp_packet *)data;

    if (ntohs(packet->protocol_type) != ETHERTYPE_IP)
        return;

    struct mac_addr sender;
    memcpy(sender.b, packet->sender_mac, ETH_ALEN);
    cache_put(packet->sender_ip, &sender);

    /* Fragt jemand nach unserer Adresse, wird geantwortet. */
    if (ntohs(packet->opcode) == 1 && packet->target_ip == g_netif.ip &&
        g_netif.ip != 0)
        send_packet(2, &sender, packet->sender_ip);
}

bool arp_resolve(ip_addr_t ip, struct mac_addr *out)
{
    /* Rundrufe gehen an alle - da ist nichts aufzuloesen. */
    if (ip == 0xFFFFFFFFu ||
        (g_netif.netmask && (ip | ~g_netif.netmask) == 0xFFFFFFFFu)) {
        *out = *eth_broadcast();
        return true;
    }

    /* Ausserhalb des eigenen Netzes geht alles ueber das Gateway. */
    if (g_netif.netmask && (ip & g_netif.netmask) != (g_netif.ip & g_netif.netmask))
        ip = g_netif.gateway;

    if (ip == 0)
        return false;
    if (cache_get(ip, out))
        return true;

    for (int attempt = 0; attempt < 3; attempt++) {
        send_packet(1, eth_broadcast(), ip);

        uint64_t deadline = timer_ms() + ARP_TIMEOUT_MS;
        while (timer_ms() < deadline) {
            net_poll();
            if (cache_get(ip, out))
                return true;
        }
    }
    return false;
}
