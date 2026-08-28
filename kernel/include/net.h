/* net.h - Netzwerkschicht von RetroOS.
 *
 * Aufbau (von unten nach oben):
 *   nic       waehlt den Kartentreiber, sendet und empfaengt Rahmen
 *   ethernet  verteilt eingehende Rahmen an ARP oder IP
 *   arp       loest IP-Adressen in Hardware-Adressen auf
 *   ip        Pruefsumme, Weiterleitung an ICMP, UDP oder TCP
 *   udp/tcp   Transport; darauf setzen DHCP, DNS und HTTP auf
 */
#ifndef NET_H
#define NET_H

#include "retro.h"

#define ETH_ALEN        6
#define ETH_MTU         1500
#define ETH_FRAME_MAX   1518

#define ETHERTYPE_IP    0x0800
#define ETHERTYPE_ARP   0x0806

#define IP_PROTO_ICMP   1
#define IP_PROTO_TCP    6
#define IP_PROTO_UDP    17

typedef uint32_t ip_addr_t;      /* in Netzwerkreihenfolge (big endian) */

struct mac_addr {
    uint8_t b[ETH_ALEN];
};

/* --- Umrechnung zwischen Host- und Netzwerkreihenfolge --- */
static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }

static inline uint32_t htonl(uint32_t v)
{
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

static inline ip_addr_t ip_make(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return htonl(((uint32_t)a << 24) | ((uint32_t)b << 16) |
                 ((uint32_t)c << 8) | d);
}

/* --- Zustand der Schnittstelle --- */
struct netif {
    bool            up;
    struct mac_addr mac;
    ip_addr_t       ip;
    ip_addr_t       netmask;
    ip_addr_t       gateway;
    ip_addr_t       dns;
    char            hostname[32];

    uint64_t        rx_packets, tx_packets;
    uint64_t        rx_bytes, tx_bytes;
    uint64_t        rx_dropped;
};

extern struct netif g_netif;

/* --- Treiber ---
 * Die eigentlichen Karten stehen in nic.h; hier haengt der Kern nur an
 * der gemeinsamen Schnittstelle. */

/* --- Kern --- */
void net_init(void);
void net_poll(void);                 /* holt eingegangene Rahmen ab        */
bool net_ready(void);                /* IP-Adresse vorhanden?              */

/* Wartet bis zu timeout_ms und verarbeitet dabei eingehende Pakete. */
void net_pump(uint32_t timeout_ms);

/* Gibt die CPU kurz ab, damit der Netz-Thread arbeiten kann. Laeuft noch
 * kein Scheduler (beim Systemstart), wird direkt abgefragt. */
void net_idle(void);

/* Schuetzt kurze Abschnitte im Netzstapel gegen Threadwechsel. */
void net_lock(void);
void net_unlock(void);

/* True, wenn der aufrufende Code gerade ein eingegangenes Paket bearbeitet.
 * Dann darf nichts blockieren - sonst stuende die Paketverarbeitung. */
bool net_in_rx_context(void);

/* Startet den Thread, der die Netzwerkkarte abfragt. */
void net_start_thread(void);

void ip_format(ip_addr_t addr, char *buf, size_t size);
bool ip_parse(const char *text, ip_addr_t *out);
void mac_format(const struct mac_addr *mac, char *buf, size_t size);

/* --- Ethernet --- */
void eth_receive(const uint8_t *frame, uint16_t length);
bool eth_send(const struct mac_addr *dst, uint16_t ethertype,
              const void *payload, uint16_t length);

/* --- ARP --- */
void arp_init(void);
void arp_receive(const uint8_t *packet, uint16_t length);
/* Sucht die Hardware-Adresse; fragt bei Bedarf nach und wartet kurz. */
bool arp_resolve(ip_addr_t ip, struct mac_addr *out);
void arp_announce(void);

/* --- IP --- */
void ip_receive(const uint8_t *packet, uint16_t length);
bool ip_send(ip_addr_t dst, uint8_t protocol, const void *payload,
             uint16_t length);
/* Wie ip_send, aber mit bereits aufgeloester Hardware-Adresse. So kann der
 * Aufrufer die Aufloesung (die warten muss) ausserhalb eines geschuetzten
 * Abschnitts erledigen. */
bool ip_send_via(const struct mac_addr *next_hop, ip_addr_t dst,
                 uint8_t protocol, const void *payload, uint16_t length);
uint16_t ip_checksum(const void *data, size_t length);

/* --- ICMP --- */
void icmp_receive(ip_addr_t src, const uint8_t *packet, uint16_t length);
/* Sendet ein Echo und wartet auf die Antwort; Laufzeit in Millisekunden. */
bool icmp_ping(ip_addr_t target, uint32_t timeout_ms, uint32_t *rtt_ms);

/* --- UDP --- */
typedef void (*udp_handler_t)(ip_addr_t src, uint16_t src_port,
                              const uint8_t *data, uint16_t length);

void udp_receive(ip_addr_t src, const uint8_t *packet, uint16_t length);
bool udp_send(ip_addr_t dst, uint16_t src_port, uint16_t dst_port,
              const void *data, uint16_t length);
bool udp_listen(uint16_t port, udp_handler_t handler);
void udp_unlisten(uint16_t port);

/* --- DHCP --- */
bool dhcp_configure(uint32_t timeout_ms);

/* --- DNS --- */
bool dns_resolve(const char *name, ip_addr_t *out);

/* --- TCP --- */
struct tcp_socket;

struct tcp_socket *tcp_connect(ip_addr_t dst, uint16_t port, uint32_t timeout_ms);

/* Nimmt einen Port in Beschlag. Der zurueckgegebene Steckplatz traegt
 * keine Daten - er sammelt nur ankommende Verbindungen ein. */
struct tcp_socket *tcp_listen(uint16_t port);

/* Holt die naechste fertige Verbindung ab; NULL, wenn in der Zeit keine
 * kam. Die zurueckgegebene gehoert dem Aufrufer und will geschlossen
 * werden. */
struct tcp_socket *tcp_accept(struct tcp_socket *listener, uint32_t timeout_ms);

uint16_t  tcp_local_port(const struct tcp_socket *sock);
ip_addr_t tcp_remote_ip(const struct tcp_socket *sock);
int  tcp_send(struct tcp_socket *sock, const void *data, uint32_t length);
/* Liest, bis nichts mehr kommt oder die Zeit abgelaufen ist. */
int  tcp_receive(struct tcp_socket *sock, void *buffer, uint32_t capacity,
                 uint32_t timeout_ms);
void tcp_close(struct tcp_socket *sock);
bool tcp_finished(const struct tcp_socket *sock);
void tcp_receive_segment(ip_addr_t src, const uint8_t *segment, uint16_t length);

/* --- HTTP --- */
struct http_response {
    int      status;
    char    *body;          /* mit kfree() freigeben */
    uint32_t body_length;
    char     content_type[64];
    char     error[96];
    char     security[64];  /* leer bei http, sonst die Verschluesselung */
    bool     truncated;     /* der Rumpf ist kuerzer als angekuendigt    */
};

bool http_get(const char *url, struct http_response *out);
void http_response_free(struct http_response *response);

/* Zerlegt eine Adresse in ihre Bestandteile. secure meldet, ob es sich um
 * https handelt; ohne Portangabe gilt 80 bzw. 443. */
bool url_split(const char *url, char *host, size_t host_size,
               uint16_t *port, char *path, size_t path_size, bool *secure);

#endif /* NET_H */
