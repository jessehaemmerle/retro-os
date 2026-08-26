/* tcp.c - eine schlanke TCP-Umsetzung fuer ausgehende Verbindungen.
 *
 * Enthalten ist, was ein Client braucht: Verbindungsaufbau mit dem
 * Dreifach-Handschlag, Senden mit Bestaetigung und erneutem Versuch,
 * geordnetes Empfangen und ein sauberer Abbau. Nicht enthalten sind
 * Ueberlastregelung, selektive Bestaetigungen und das Zusammensetzen
 * ausserhalb der Reihenfolge eingetroffener Daten - fuer das Abrufen
 * einer Seite ueber HTTP wird davon nichts gebraucht.
 */

#include "net.h"
#include "arch.h"
#include "thread.h"
#include "kstring.h"
#include "mm.h"

#define TCP_MAX_SOCKETS   4
#define TCP_RX_CAPACITY   (64 * 1024)
#define TCP_MSS           1400
#define TCP_RETRIES       4

#define FLAG_FIN 0x01
#define FLAG_SYN 0x02
#define FLAG_RST 0x04
#define FLAG_PSH 0x08
#define FLAG_ACK 0x10

enum tcp_state {
    TCP_CLOSED,
    TCP_SYN_SENT,
    TCP_ESTABLISHED,
    TCP_FIN_SENT,
};

struct tcp_header {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  offset;          /* obere vier Bit: Kopflaenge in Woertern */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} PACKED;

struct tcp_socket {
    bool           used;
    enum tcp_state state;

    ip_addr_t remote_ip;
    uint16_t  remote_port;
    uint16_t  local_port;

    uint32_t send_next;
    uint32_t send_unacked;
    uint32_t recv_next;

    uint8_t  *rx;
    uint32_t  rx_length;
    uint32_t  rx_read;

    bool      remote_closed;
    bool      reset;
};

static struct tcp_socket sockets[TCP_MAX_SOCKETS];
static uint16_t          next_local_port = 40000;

/* Pruefsumme ueber den Pseudokopf und das Segment. */
static uint16_t tcp_checksum(ip_addr_t src, ip_addr_t dst,
                             const uint8_t *segment, uint16_t length)
{
    uint32_t sum = 0;

    const uint8_t *s = (const uint8_t *)&src;
    const uint8_t *d = (const uint8_t *)&dst;

    sum += (uint32_t)((s[0] << 8) | s[1]);
    sum += (uint32_t)((s[2] << 8) | s[3]);
    sum += (uint32_t)((d[0] << 8) | d[1]);
    sum += (uint32_t)((d[2] << 8) | d[3]);
    sum += IP_PROTO_TCP;
    sum += length;

    for (uint16_t i = 0; i + 1 < length; i += 2)
        sum += (uint32_t)((segment[i] << 8) | segment[i + 1]);
    if (length & 1)
        sum += (uint32_t)(segment[length - 1] << 8);

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

static bool send_segment(struct tcp_socket *sock, uint8_t flags,
                         const void *data, uint16_t length)
{
    static uint8_t segment[ETH_MTU];   /* zu gross fuer den Stapel */
    struct mac_addr next_hop;

    /* Zuerst die Hardware-Adresse besorgen. Das kann warten und darf
     * deshalb nicht innerhalb des geschuetzten Abschnitts geschehen -
     * sonst stuende das ganze System, bis die Antwort da ist. */
    if (!arp_resolve(sock->remote_ip, &next_hop))
        return false;

    net_lock();
    struct tcp_header *header = (struct tcp_header *)segment;
    uint16_t header_size = sizeof(*header);

    memset(header, 0, sizeof(*header));
    header->src_port = htons(sock->local_port);
    header->dst_port = htons(sock->remote_port);
    header->seq      = htonl(sock->send_next);
    header->ack      = htonl(sock->recv_next);
    header->flags    = flags;
    header->window   = htons(8192);

    /* Beim Verbindungsaufbau die groesste Segmentgroesse mitteilen. */
    if (flags & FLAG_SYN) {
        uint8_t *option = segment + sizeof(*header);

        option[0] = 2;                     /* MSS      */
        option[1] = 4;                     /* Laenge   */
        option[2] = (uint8_t)(TCP_MSS >> 8);
        option[3] = (uint8_t)(TCP_MSS & 0xFF);
        header_size += 4;
    }

    header->offset = (uint8_t)((header_size / 4) << 4);

    if (length > 0)
        memcpy(segment + header_size, data, length);

    uint16_t total = (uint16_t)(header_size + length);
    header->checksum = htons(tcp_checksum(g_netif.ip, sock->remote_ip,
                                          segment, total));

    bool ok = ip_send_via(&next_hop, sock->remote_ip, IP_PROTO_TCP,
                          segment, total);
    net_unlock();

    return ok;
}

static struct tcp_socket *find_socket(ip_addr_t src, uint16_t src_port,
                                      uint16_t dst_port)
{
    for (size_t i = 0; i < TCP_MAX_SOCKETS; i++) {
        struct tcp_socket *s = &sockets[i];

        if (s->used && s->remote_ip == src && s->remote_port == src_port &&
            s->local_port == dst_port)
            return s;
    }
    return NULL;
}

void tcp_receive_segment(ip_addr_t src, const uint8_t *data, uint16_t length)
{
    if (length < sizeof(struct tcp_header))
        return;

    const struct tcp_header *header = (const struct tcp_header *)data;
    uint16_t header_size = (uint16_t)((header->offset >> 4) * 4);

    if (header_size < sizeof(*header) || header_size > length)
        return;

    struct tcp_socket *sock = find_socket(src, ntohs(header->src_port),
                                          ntohs(header->dst_port));
    if (!sock)
        return;

    uint32_t seq = ntohl(header->seq);
    uint32_t ack = ntohl(header->ack);
    const uint8_t *payload = data + header_size;
    uint16_t payload_length = (uint16_t)(length - header_size);

    if (header->flags & FLAG_RST) {
        sock->reset = true;
        sock->state = TCP_CLOSED;
        return;
    }

    if (sock->state == TCP_SYN_SENT) {
        if ((header->flags & (FLAG_SYN | FLAG_ACK)) == (FLAG_SYN | FLAG_ACK)) {
            sock->recv_next    = seq + 1;
            sock->send_unacked = ack;
            sock->send_next    = ack;
            sock->state        = TCP_ESTABLISHED;
            send_segment(sock, FLAG_ACK, NULL, 0);
        }
        return;
    }

    if (header->flags & FLAG_ACK)
        sock->send_unacked = ack;

    /* Nur Daten in der erwarteten Reihenfolge annehmen. */
    if (payload_length > 0 && seq == sock->recv_next) {
        uint32_t room = TCP_RX_CAPACITY - sock->rx_length;
        uint32_t take = MIN((uint32_t)payload_length, room);

        if (take > 0) {
            memcpy(sock->rx + sock->rx_length, payload, take);
            sock->rx_length += take;
            sock->recv_next += take;
        }
        send_segment(sock, FLAG_ACK, NULL, 0);
    } else if (payload_length > 0) {
        /* Erneut bestaetigen, was wir haben - der Gegner schickt nach. */
        send_segment(sock, FLAG_ACK, NULL, 0);
    }

    if (header->flags & FLAG_FIN) {
        sock->recv_next++;
        sock->remote_closed = true;
        send_segment(sock, FLAG_ACK, NULL, 0);
    }
}

struct tcp_socket *tcp_connect(ip_addr_t dst, uint16_t port, uint32_t timeout_ms)
{
    if (!net_ready())
        return NULL;

    struct tcp_socket *sock = NULL;
    for (size_t i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!sockets[i].used) {
            sock = &sockets[i];
            break;
        }
    }
    if (!sock)
        return NULL;

    memset(sock, 0, sizeof(*sock));
    sock->rx = kmalloc(TCP_RX_CAPACITY);
    if (!sock->rx)
        return NULL;

    sock->used        = true;
    sock->state       = TCP_SYN_SENT;
    sock->remote_ip   = dst;
    sock->remote_port = port;
    sock->local_port  = next_local_port++;
    if (next_local_port < 40000)
        next_local_port = 40000;

    /* Anfangsnummer aus der Systemzeit - besser als eine feste Zahl. */
    sock->send_next    = (uint32_t)(timer_ms() * 2654435761u);
    sock->send_unacked = sock->send_next;

    uint64_t deadline = timer_ms() + timeout_ms;

    for (int attempt = 0; attempt < TCP_RETRIES; attempt++) {
        uint32_t seq_backup = sock->send_next;

        if (!send_segment(sock, FLAG_SYN, NULL, 0)) {
            kfree(sock->rx);
            sock->used = false;
            return NULL;
        }
        sock->send_next = seq_backup;

        uint64_t wait_until = MIN(timer_ms() + 700, deadline);
        while (timer_ms() < wait_until) {
            net_idle();
            if (sock->state == TCP_ESTABLISHED)
                return sock;
            if (sock->reset)
                break;
        }

        if (sock->reset || timer_ms() >= deadline)
            break;
    }

    kfree(sock->rx);
    sock->used = false;
    return NULL;
}

int tcp_send(struct tcp_socket *sock, const void *data, uint32_t length)
{
    if (!sock || !sock->used || sock->state != TCP_ESTABLISHED)
        return -1;

    const uint8_t *p = data;
    uint32_t remaining = length;

    while (remaining > 0) {
        uint16_t chunk = (uint16_t)MIN(remaining, (uint32_t)TCP_MSS);
        uint32_t expected = sock->send_next + chunk;
        bool acked = false;

        for (int attempt = 0; attempt < TCP_RETRIES && !acked; attempt++) {
            if (!send_segment(sock, FLAG_ACK | FLAG_PSH, p, chunk))
                return -1;

            uint64_t deadline = timer_ms() + 800;
            while (timer_ms() < deadline) {
                net_idle();
                if ((int32_t)(sock->send_unacked - expected) >= 0) {
                    acked = true;
                    break;
                }
                if (sock->reset)
                    return -1;
            }
        }

        if (!acked)
            return -1;

        sock->send_next = expected;
        p += chunk;
        remaining -= chunk;
    }
    return (int)length;
}

int tcp_receive(struct tcp_socket *sock, void *buffer, uint32_t capacity,
                uint32_t timeout_ms)
{
    if (!sock || !sock->used)
        return -1;

    uint64_t deadline = timer_ms() + timeout_ms;
    uint64_t quiet_since = timer_ms();

    /* Warten, bis die Gegenseite schliesst, der Puffer voll ist oder
     * eine Weile nichts mehr kommt. */
    while (timer_ms() < deadline) {
        uint32_t before = sock->rx_length;

        net_idle();

        if (sock->rx_length != before)
            quiet_since = timer_ms();
        if (sock->remote_closed || sock->reset)
            break;
        if (sock->rx_length >= capacity)
            break;
        if (sock->rx_length > 0 && timer_ms() - quiet_since > 600)
            break;
    }

    uint32_t available = sock->rx_length - sock->rx_read;
    uint32_t take = MIN(available, capacity);

    if (take > 0) {
        memcpy(buffer, sock->rx + sock->rx_read, take);
        sock->rx_read += take;
    }
    return (int)take;
}

bool tcp_finished(const struct tcp_socket *sock)
{
    return !sock || sock->remote_closed || sock->reset ||
           sock->state != TCP_ESTABLISHED;
}

void tcp_close(struct tcp_socket *sock)
{
    if (!sock || !sock->used)
        return;

    if (sock->state == TCP_ESTABLISHED && !sock->reset) {
        send_segment(sock, FLAG_FIN | FLAG_ACK, NULL, 0);
        sock->send_next++;
        sock->state = TCP_FIN_SENT;

        uint64_t deadline = timer_ms() + 500;
        while (timer_ms() < deadline && !sock->remote_closed)
            net_idle();
    }

    kfree(sock->rx);
    sock->rx = NULL;
    sock->used = false;
    sock->state = TCP_CLOSED;
}
