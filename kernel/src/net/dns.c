/* dns.c - Namen in Adressen aufloesen.
 *
 * Nur das Noetigste: eine Frage nach einem A-Eintrag, eine Antwort, ein
 * kleiner Zwischenspeicher. Namensverweise (CNAME) werden mitgelesen.
 */

#include "net.h"
#include "arch.h"
#include "kstring.h"

#define DNS_PORT        53
#define DNS_CLIENT_PORT 5353
#define DNS_CACHE_SIZE  16
#define DNS_TIMEOUT_MS  3000

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
} PACKED;

struct dns_cache_entry {
    char      name[64];
    ip_addr_t ip;
    bool      valid;
};

static struct dns_cache_entry cache[DNS_CACHE_SIZE];
static size_t                 cache_next;

static volatile bool      answer_seen;
static volatile ip_addr_t answer_ip;
static uint16_t           query_id;

/* "www.example.com" wird zu 3www7example3com0 */
static size_t encode_name(const char *name, uint8_t *out, size_t capacity)
{
    size_t pos = 0;

    while (*name && pos + 1 < capacity) {
        const char *dot = strchr(name, '.');
        size_t part = dot ? (size_t)(dot - name) : strlen(name);

        if (part == 0 || part > 63 || pos + part + 2 > capacity)
            return 0;

        out[pos++] = (uint8_t)part;
        memcpy(out + pos, name, part);
        pos += part;

        name += part;
        if (*name == '.')
            name++;
    }
    out[pos++] = 0;
    return pos;
}

/* Springt ueber einen Namen im Antwortteil - er kann ein Verweis sein. */
static const uint8_t *skip_name(const uint8_t *p, const uint8_t *end)
{
    while (p < end) {
        if ((*p & 0xC0) == 0xC0)
            return p + 2;                 /* Verweis, zwei Byte lang */
        if (*p == 0)
            return p + 1;
        p += 1 + *p;
    }
    return end;
}

static void handle(ip_addr_t src, uint16_t src_port, const uint8_t *data,
                   uint16_t length)
{
    UNUSED(src);
    UNUSED(src_port);

    if (length < sizeof(struct dns_header))
        return;

    const struct dns_header *header = (const struct dns_header *)data;
    if (ntohs(header->id) != query_id)
        return;

    const uint8_t *p   = data + sizeof(*header);
    const uint8_t *end = data + length;

    for (uint16_t i = 0; i < ntohs(header->questions); i++) {
        p = skip_name(p, end);
        p += 4;                            /* Typ und Klasse */
    }

    for (uint16_t i = 0; i < ntohs(header->answers) && p + 10 <= end; i++) {
        p = skip_name(p, end);
        if (p + 10 > end)
            break;

        uint16_t type = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t data_length = (uint16_t)((p[8] << 8) | p[9]);
        p += 10;

        if (p + data_length > end)
            break;

        if (type == 1 && data_length == 4) {      /* A-Eintrag */
            memcpy((void *)&answer_ip, p, 4);
            answer_seen = true;
            return;
        }
        p += data_length;
    }
}

static bool cache_lookup(const char *name, ip_addr_t *out)
{
    for (size_t i = 0; i < DNS_CACHE_SIZE; i++) {
        if (cache[i].valid && strcasecmp(cache[i].name, name) == 0) {
            *out = cache[i].ip;
            return true;
        }
    }
    return false;
}

static void cache_store(const char *name, ip_addr_t ip)
{
    struct dns_cache_entry *e = &cache[cache_next];

    cache_next = (cache_next + 1) % DNS_CACHE_SIZE;
    strlcpy(e->name, name, sizeof(e->name));
    e->ip = ip;
    e->valid = true;
}

bool dns_resolve(const char *name, ip_addr_t *out)
{
    if (!name || !name[0])
        return false;

    /* Steht da schon eine Adresse, ist nichts aufzuloesen. */
    if (ip_parse(name, out))
        return true;
    if (cache_lookup(name, out))
        return true;
    if (!net_ready() || g_netif.dns == 0)
        return false;

    uint8_t packet[512];
    struct dns_header *header = (struct dns_header *)packet;

    query_id = (uint16_t)(timer_ticks() & 0xFFFF) | 1;

    memset(header, 0, sizeof(*header));
    header->id        = htons(query_id);
    header->flags     = htons(0x0100);      /* Standardanfrage, Rekursion */
    header->questions = htons(1);

    size_t pos = sizeof(*header);
    size_t n = encode_name(name, packet + pos, sizeof(packet) - pos - 4);
    if (n == 0)
        return false;
    pos += n;

    packet[pos++] = 0; packet[pos++] = 1;   /* Typ A     */
    packet[pos++] = 0; packet[pos++] = 1;   /* Klasse IN */

    answer_seen = false;
    answer_ip   = 0;

    udp_listen(DNS_CLIENT_PORT, handle);

    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        udp_send(g_netif.dns, DNS_CLIENT_PORT, DNS_PORT, packet, (uint16_t)pos);

        uint64_t deadline = timer_ms() + DNS_TIMEOUT_MS;
        while (timer_ms() < deadline && !answer_seen)
            net_poll();

        ok = answer_seen;
    }

    udp_unlisten(DNS_CLIENT_PORT);

    if (!ok)
        return false;

    *out = answer_ip;
    cache_store(name, answer_ip);
    return true;
}
