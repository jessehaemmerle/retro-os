/* tcp.c - TCP fuer ausgehende Verbindungen.
 *
 * Umgesetzt ist das, was eine Verbindung ueber ein echtes Netz braucht:
 *
 *   Verbindungsaufbau   Dreifach-Handschlag mit erneuten Versuchen
 *   Empfangen           Segmente ausserhalb der Reihenfolge werden
 *                       zwischengelegt und spaeter eingesetzt
 *   Fenstersteuerung    beide Seiten melden, wieviel Platz sie haben
 *   Ueberlastregelung   langsamer Start, dann vorsichtiges Wachsen;
 *                       nach Verlust zurueck auf die Haelfte
 *   Schnelles Senden    drei gleiche Bestaetigungen loesen die
 *                       Wiederholung aus, ohne auf die Frist zu warten
 *   Abbau               FIN mit Bestaetigung
 *
 * Nicht umgesetzt sind selektive Bestaetigungen (SACK), Zeitstempel und
 * das Zusammenfassen kleiner Sendungen (Nagle) - alles Verbesserungen,
 * die eine bestehende Verbindung schneller machen, aber nichts an ihrer
 * Verlaesslichkeit aendern.
 */

#include "net.h"
#include "arch.h"
#include "kstring.h"
#include "mm.h"
#include "thread.h"

#define TCP_MAX_SOCKETS   4
#define TCP_RX_CAPACITY   (128 * 1024)
#define TCP_MSS           1400
#define TCP_OOO_SLOTS     8
#define TCP_SYN_RETRIES   4
#define TCP_DATA_RETRIES  6
#define TCP_RTO_MIN_MS    300
#define TCP_RTO_MAX_MS    3000

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

/* Ein Segment, das zu frueh kam. */
struct ooo_segment {
    bool     used;
    uint32_t seq;
    uint16_t length;
    uint8_t  data[TCP_MSS];
};

struct tcp_socket {
    bool           used;
    enum tcp_state state;

    ip_addr_t remote_ip;
    uint16_t  remote_port;
    uint16_t  local_port;

    uint32_t send_next;
    uint32_t send_unacked;
    uint32_t recv_next;

    /* Fenster und Ueberlastregelung */
    uint32_t peer_window;
    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t dup_acks;
    bool     retransmit_wanted;

    uint8_t  *rx;
    uint32_t  rx_length;
    uint32_t  rx_read;

    struct ooo_segment *ooo;

    bool      remote_closed;
    bool      reset;

    uint64_t  last_progress_ms;
};

static struct tcp_socket sockets[TCP_MAX_SOCKETS];
static uint16_t          next_local_port = 40000;

/* Vergleich, der den Ueberlauf der Sequenznummern beruecksichtigt. */
static inline bool seq_after(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

static inline bool seq_before_or_equal(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}

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

/* Wieviel Platz haben wir noch? Das meldet jedes Segment mit. */
static uint16_t local_window(struct tcp_socket *sock)
{
    uint32_t free_space = TCP_RX_CAPACITY - sock->rx_length;

    return (uint16_t)MIN(free_space, 65535u);
}

static bool send_segment(struct tcp_socket *sock, uint32_t seq, uint8_t flags,
                         const void *data, uint16_t length)
{
    static uint8_t segment[ETH_MTU];
    struct mac_addr next_hop;

    /* Die Hardware-Adresse zuerst besorgen: das kann warten und darf
     * deshalb nicht im geschuetzten Abschnitt geschehen. */
    if (!arp_resolve(sock->remote_ip, &next_hop))
        return false;

    net_lock();

    struct tcp_header *header = (struct tcp_header *)segment;
    uint16_t header_size = sizeof(*header);

    memset(header, 0, sizeof(*header));
    header->src_port = htons(sock->local_port);
    header->dst_port = htons(sock->remote_port);
    header->seq      = htonl(seq);
    header->ack      = htonl(sock->recv_next);
    header->flags    = flags;
    header->window   = htons(local_window(sock));

    if (flags & FLAG_SYN) {
        uint8_t *option = segment + sizeof(*header);

        option[0] = 2;                     /* groesste Segmentgroesse */
        option[1] = 4;
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

/* --- Empfangen ------------------------------------------------------- */

static void deliver(struct tcp_socket *sock, const uint8_t *data,
                    uint16_t length)
{
    uint32_t room = TCP_RX_CAPACITY - sock->rx_length;
    uint32_t take = MIN((uint32_t)length, room);

    if (take == 0)
        return;

    memcpy(sock->rx + sock->rx_length, data, take);
    sock->rx_length += take;
    sock->recv_next += take;
}

/* Legt ein zu frueh gekommenes Segment beiseite. */
static void stash(struct tcp_socket *sock, uint32_t seq, const uint8_t *data,
                  uint16_t length)
{
    if (length > TCP_MSS || !sock->ooo)
        return;

    for (int i = 0; i < TCP_OOO_SLOTS; i++) {
        if (sock->ooo[i].used && sock->ooo[i].seq == seq)
            return;                       /* haben wir schon */
    }

    for (int i = 0; i < TCP_OOO_SLOTS; i++) {
        if (sock->ooo[i].used)
            continue;

        sock->ooo[i].used   = true;
        sock->ooo[i].seq    = seq;
        sock->ooo[i].length = length;
        memcpy(sock->ooo[i].data, data, length);
        return;
    }
    /* Kein Platz mehr - das Segment kommt noch einmal. */
}

/* Setzt beiseitegelegte Segmente ein, sobald die Luecke geschlossen ist. */
static void drain_stash(struct tcp_socket *sock)
{
    bool progress = true;

    while (progress && sock->ooo) {
        progress = false;

        for (int i = 0; i < TCP_OOO_SLOTS; i++) {
            struct ooo_segment *slot = &sock->ooo[i];

            if (!slot->used)
                continue;

            if (slot->seq == sock->recv_next) {
                deliver(sock, slot->data, slot->length);
                slot->used = false;
                progress = true;
            } else if (seq_before_or_equal(slot->seq + slot->length,
                                           sock->recv_next)) {
                slot->used = false;        /* laengst ueberholt */
            }
        }
    }
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
            sock->peer_window  = ntohs(header->window);
            sock->state        = TCP_ESTABLISHED;
            sock->last_progress_ms = timer_ms();
            send_segment(sock, sock->send_next, FLAG_ACK, NULL, 0);
        }
        return;
    }

    sock->peer_window = ntohs(header->window);

    if (header->flags & FLAG_ACK) {
        if (seq_after(ack, sock->send_unacked)) {
            uint32_t acked = ack - sock->send_unacked;

            sock->send_unacked = ack;
            sock->dup_acks = 0;
            sock->last_progress_ms = timer_ms();

            /* Langsamer Start, bis die Schwelle erreicht ist; danach
             * waechst das Fenster nur noch um etwa ein Segment je
             * Umlauf. */
            if (sock->cwnd < sock->ssthresh)
                sock->cwnd += MIN(acked, (uint32_t)TCP_MSS);
            else
                sock->cwnd += MAX((uint32_t)TCP_MSS * TCP_MSS / sock->cwnd, 1u);

            if (sock->cwnd > TCP_RX_CAPACITY)
                sock->cwnd = TCP_RX_CAPACITY;

        } else if (ack == sock->send_unacked && payload_length == 0 &&
                   !(header->flags & FLAG_FIN)) {
            /* Dieselbe Bestaetigung noch einmal: der Gegenseite fehlt
             * etwas. Beim dritten Mal wird sofort wiederholt, statt die
             * Frist abzuwarten. */
            if (++sock->dup_acks == 3) {
                sock->ssthresh = MAX(sock->cwnd / 2, (uint32_t)(2 * TCP_MSS));
                sock->cwnd = sock->ssthresh + 3 * TCP_MSS;
                sock->retransmit_wanted = true;
            }
        }
    }

    if (payload_length > 0) {
        if (seq == sock->recv_next) {
            deliver(sock, payload, payload_length);
            drain_stash(sock);
        } else if (seq_after(seq, sock->recv_next)) {
            stash(sock, seq, payload, payload_length);
        }
        /* Immer bestaetigen - auch bei einer Luecke, damit die Gegenseite
         * merkt, wo sie steht. */
        send_segment(sock, sock->send_next, FLAG_ACK, NULL, 0);
    }

    if (header->flags & FLAG_FIN) {
        sock->recv_next++;
        sock->remote_closed = true;
        send_segment(sock, sock->send_next, FLAG_ACK, NULL, 0);
    }
}

/* --- Verbinden ------------------------------------------------------- */

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
    sock->ooo = kzalloc(sizeof(struct ooo_segment) * TCP_OOO_SLOTS);

    if (!sock->rx || !sock->ooo) {
        kfree(sock->rx);
        kfree(sock->ooo);
        return NULL;
    }

    sock->used        = true;
    sock->state       = TCP_SYN_SENT;
    sock->remote_ip   = dst;
    sock->remote_port = port;
    sock->local_port  = next_local_port++;
    if (next_local_port < 40000)
        next_local_port = 40000;

    sock->cwnd        = TCP_MSS;
    sock->ssthresh    = 64 * TCP_MSS;
    sock->peer_window = TCP_MSS;

    sock->send_next    = (uint32_t)(timer_ms() * 2654435761u);
    sock->send_unacked = sock->send_next;

    uint64_t deadline = timer_ms() + timeout_ms;
    uint32_t wait_ms = TCP_RTO_MIN_MS;

    for (int attempt = 0; attempt < TCP_SYN_RETRIES; attempt++) {
        if (!send_segment(sock, sock->send_next, FLAG_SYN, NULL, 0)) {
            kfree(sock->rx);
            kfree(sock->ooo);
            sock->used = false;
            return NULL;
        }

        uint64_t wait_until = MIN(timer_ms() + wait_ms, deadline);
        while (timer_ms() < wait_until) {
            net_idle();
            if (sock->state == TCP_ESTABLISHED)
                return sock;
            if (sock->reset)
                break;
        }

        if (sock->reset || timer_ms() >= deadline)
            break;

        wait_ms = MIN(wait_ms * 2, TCP_RTO_MAX_MS);   /* Frist verdoppeln */
    }

    kfree(sock->rx);
    kfree(sock->ooo);
    sock->used = false;
    return NULL;
}

/* --- Senden ---------------------------------------------------------- */

int tcp_send(struct tcp_socket *sock, const void *data, uint32_t length)
{
    if (!sock || !sock->used || sock->state != TCP_ESTABLISHED)
        return -1;
    if (length == 0)
        return 0;

    const uint8_t *bytes = data;
    uint32_t base = sock->send_next;
    uint32_t sent = 0;          /* schon auf die Leitung gegeben */
    uint32_t rto  = TCP_RTO_MIN_MS;
    int      timeouts = 0;

    while (sock->send_unacked - base < length) {
        /* Fenster fuellen: soviel unterwegs sein lassen, wie beide Seiten
         * erlauben - die Gegenseite ueber ihr Fenster, wir selbst ueber
         * die Ueberlastregelung. */
        for (;;) {
            uint32_t acked    = sock->send_unacked - base;
            uint32_t inflight = sent - acked;
            uint32_t window   = MIN(sock->cwnd, MAX(sock->peer_window, 1u));

            if (sent >= length || inflight >= window)
                break;

            uint32_t chunk = MIN(length - sent, (uint32_t)TCP_MSS);
            chunk = MIN(chunk, window - inflight);
            if (chunk == 0)
                break;

            if (!send_segment(sock, base + sent, FLAG_ACK | FLAG_PSH,
                              bytes + sent, (uint16_t)chunk))
                return -1;
            sent += chunk;
        }

        uint32_t before = sock->send_unacked;
        uint64_t deadline = timer_ms() + rto;

        while (timer_ms() < deadline) {
            net_idle();

            if (sock->reset)
                return -1;
            if (sock->send_unacked != before)
                break;
            if (sock->retransmit_wanted)
                break;
        }

        if (sock->retransmit_wanted) {
            /* Schnelle Wiederholung: ab der ersten unbestaetigten Stelle. */
            sock->retransmit_wanted = false;
            sent = sock->send_unacked - base;
            continue;
        }

        if (sock->send_unacked == before) {
            if (++timeouts > TCP_DATA_RETRIES)
                return -1;

            /* Verlust: Schwelle halbieren und wieder ganz klein anfangen. */
            sock->ssthresh = MAX(sock->cwnd / 2, (uint32_t)(2 * TCP_MSS));
            sock->cwnd = TCP_MSS;
            sent = sock->send_unacked - base;
            rto = MIN(rto * 2, TCP_RTO_MAX_MS);
        } else {
            timeouts = 0;
            rto = TCP_RTO_MIN_MS;
        }
    }

    sock->send_next = base + length;
    return (int)length;
}

/* --- Empfangen aus Sicht des Aufrufers ------------------------------- */

int tcp_receive(struct tcp_socket *sock, void *buffer, uint32_t capacity,
                uint32_t timeout_ms)
{
    if (!sock || !sock->used)
        return -1;

    uint64_t deadline = timer_ms() + timeout_ms;
    uint64_t quiet_since = timer_ms();

    while (timer_ms() < deadline) {
        uint32_t before = sock->rx_length;

        net_idle();

        if (sock->rx_length != before)
            quiet_since = timer_ms();
        if (sock->remote_closed || sock->reset)
            break;
        if (sock->rx_length - sock->rx_read >= capacity)
            break;
        if (sock->rx_length > sock->rx_read && timer_ms() - quiet_since > 400)
            break;
    }

    uint32_t available = sock->rx_length - sock->rx_read;
    uint32_t take = MIN(available, capacity);

    if (take > 0) {
        memcpy(buffer, sock->rx + sock->rx_read, take);
        sock->rx_read += take;

        /* Gelesenes wegräumen, damit das gemeldete Fenster wieder waechst. */
        if (sock->rx_read > TCP_RX_CAPACITY / 2) {
            uint32_t rest = sock->rx_length - sock->rx_read;

            memmove(sock->rx, sock->rx + sock->rx_read, rest);
            sock->rx_length = rest;
            sock->rx_read = 0;
        }
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
        send_segment(sock, sock->send_next, FLAG_FIN | FLAG_ACK, NULL, 0);
        sock->send_next++;
        sock->state = TCP_FIN_SENT;

        uint64_t deadline = timer_ms() + 500;
        while (timer_ms() < deadline && !sock->remote_closed)
            net_idle();
    }

    kfree(sock->rx);
    kfree(sock->ooo);
    sock->rx = NULL;
    sock->ooo = NULL;
    sock->used = false;
    sock->state = TCP_CLOSED;
}
