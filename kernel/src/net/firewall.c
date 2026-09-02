/* firewall.c - Regeln pruefen, lesen und schreiben.
 *
 * Die Pruefung selbst ist eine Schleife ueber hoechstens
 * zweiunddreissig Eintraege und laeuft fuer jedes Paket. Das klingt
 * teuer, ist es aber nicht: Bei dieser Zahl passt die ganze Liste in
 * den Zwischenspeicher der CPU, und ein Baum oder eine Streutabelle
 * waere bei so wenigen Regeln langsamer als das schlichte Durchgehen.
 */

#include "firewall.h"

#include "audit.h"
#include "kstring.h"
#include "log.h"
#include "mm.h"
#include "vfs.h"

static struct fw_rule rules[FW_RULES_MAX];
static uint8_t        policy[FW_DIR_COUNT];
static uint64_t       dropped[FW_DIR_COUNT];
static uint64_t       passed[FW_DIR_COUNT];
static bool           active;

void fw_init(void)
{
    memset(rules, 0, sizeof(rules));
    memset(dropped, 0, sizeof(dropped));
    memset(passed, 0, sizeof(passed));

    /* Ohne Regeln laesst der Filter alles durch. Ein Rechner, der nach
     * dem Einschalten stumm ist, weil jemand die Grundeinstellung auf
     * "verwerfen" gesetzt hat, waere die unangenehmste Art, diese
     * Neuerung kennenzulernen. */
    policy[FW_IN]  = FW_ALLOW;
    policy[FW_OUT] = FW_ALLOW;
    active = false;
}

bool fw_enabled(void) { return active; }

void fw_enable(bool on)
{
    if (active == on)
        return;
    active = on;
    log_info("firewall", "%s", on ? "eingeschaltet" : "ausgeschaltet");
    audit(AUDIT_NETWORK, true, "Paketfilter %s",
          on ? "eingeschaltet" : "ausgeschaltet");
}

enum fw_action fw_policy(enum fw_dir dir)
{
    return dir < FW_DIR_COUNT ? (enum fw_action)policy[dir] : FW_ALLOW;
}

void fw_set_policy(enum fw_dir dir, enum fw_action action)
{
    if (dir >= FW_DIR_COUNT)
        return;
    policy[dir] = (uint8_t)action;
    log_info("firewall", "Grundeinstellung %s: %s",
             dir == FW_IN ? "eingehend" : "ausgehend",
             fw_action_name(action));
}

/* ------------------------------------------------------------------ */
/* Regeln fuehren                                                      */
/* ------------------------------------------------------------------ */

static ip_addr_t prefix_mask(uint8_t prefix)
{
    if (prefix == 0)
        return 0;
    if (prefix > 32)
        prefix = 32;

    uint32_t host = 0xFFFFFFFFu << (32 - prefix);

    /* Adressen liegen in Netzwerkreihenfolge - die Maske auch. */
    return htonl(host);
}

int fw_add(enum fw_dir dir, enum fw_action action, uint8_t proto,
           ip_addr_t addr, uint8_t prefix, uint16_t port_lo,
           uint16_t port_hi, const char *note)
{
    if (dir >= FW_DIR_COUNT || port_lo > port_hi)
        return -1;

    for (size_t i = 0; i < FW_RULES_MAX; i++) {
        if (rules[i].used)
            continue;

        struct fw_rule *r = &rules[i];

        memset(r, 0, sizeof(*r));
        r->used    = true;
        r->dir     = (uint8_t)dir;
        r->action  = (uint8_t)action;
        r->proto   = proto;
        r->mask    = prefix_mask(prefix);
        r->addr    = addr & r->mask;
        r->port_lo = port_lo;
        r->port_hi = port_hi;
        if (note)
            strlcpy(r->note, note, sizeof(r->note));

        char text[96];

        fw_rule_text(r, text, sizeof(text));
        log_info("firewall", "Regel %u: %s", (unsigned)i, text);
        audit(AUDIT_NETWORK, true, "Regel: %s", text);
        return (int)i;
    }
    return -1;
}

bool fw_remove(size_t index)
{
    if (index >= FW_RULES_MAX || !rules[index].used)
        return false;
    memset(&rules[index], 0, sizeof(rules[index]));
    log_info("firewall", "Regel %u entfernt", (unsigned)index);
    return true;
}

void fw_clear(void)
{
    memset(rules, 0, sizeof(rules));
    log_warn("firewall", "alle Regeln entfernt");
    audit(AUDIT_NETWORK, true, "alle Regeln entfernt");
}

size_t fw_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < FW_RULES_MAX; i++)
        if (rules[i].used)
            n++;
    return n;
}

const struct fw_rule *fw_at(size_t index)
{
    if (index >= FW_RULES_MAX || !rules[index].used)
        return NULL;
    return &rules[index];
}

/* ------------------------------------------------------------------ */
/* Pruefen                                                             */
/* ------------------------------------------------------------------ */

static bool matches(const struct fw_rule *r, enum fw_dir dir, ip_addr_t peer,
                    uint8_t proto, uint16_t local_port)
{
    if (r->dir != (uint8_t)dir)
        return false;
    if (r->proto && r->proto != proto)
        return false;
    if (r->mask && (peer & r->mask) != r->addr)
        return false;

    /* Bei ICMP gibt es keine Ports. Eine Regel, die welche nennt, kann
     * darauf also nicht gemeint sein - eine ohne Einschraenkung
     * (0-65535) passt weiterhin. */
    if (r->port_lo != 0 || r->port_hi != 0xFFFF) {
        if (proto != IP_PROTO_TCP && proto != IP_PROTO_UDP)
            return false;
        if (local_port < r->port_lo || local_port > r->port_hi)
            return false;
    }
    return true;
}

bool fw_check(enum fw_dir dir, ip_addr_t peer, uint8_t proto,
              uint16_t local_port, uint16_t peer_port)
{
    UNUSED(peer_port);

    if (!active || dir >= FW_DIR_COUNT)
        return true;

    enum fw_action verdict = (enum fw_action)policy[dir];

    for (size_t i = 0; i < FW_RULES_MAX; i++) {
        if (!rules[i].used)
            continue;
        if (!matches(&rules[i], dir, peer, proto, local_port))
            continue;

        rules[i].hits++;
        verdict = (enum fw_action)rules[i].action;
        break;                     /* die erste passende entscheidet */
    }

    if (verdict == FW_ALLOW) {
        passed[dir]++;
        return true;
    }

    dropped[dir]++;

    /* Nicht jedes weggeworfene Paket ins Protokoll - ein Scan wuerde
     * den Ring in Sekunden leerlaufen lassen. Nur jedes hundertste,
     * das genuegt, um zu sehen, dass etwas anliegt. */
    if (dropped[dir] % 100 == 1) {
        char text[24];

        ip_format(peer, text, sizeof(text));
        log_warn("firewall", "%s %s von/nach %s Port %u (%u insgesamt)",
                 dir == FW_IN ? "eingehend" : "ausgehend",
                 fw_action_name(verdict), text, (unsigned)local_port,
                 (unsigned)dropped[dir]);
    }
    return false;
}

uint64_t fw_dropped(enum fw_dir dir)
{
    return dir < FW_DIR_COUNT ? dropped[dir] : 0;
}

uint64_t fw_passed(enum fw_dir dir)
{
    return dir < FW_DIR_COUNT ? passed[dir] : 0;
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

const char *fw_action_name(enum fw_action action)
{
    switch (action) {
    case FW_DROP:   return "verwerfen";
    case FW_REJECT: return "ablehnen";
    default:        return "erlauben";
    }
}

const char *fw_proto_name(uint8_t proto)
{
    switch (proto) {
    case IP_PROTO_ICMP: return "icmp";
    case IP_PROTO_TCP:  return "tcp";
    case IP_PROTO_UDP:  return "udp";
    default:            return "alle";
    }
}

/* Wie viele Bits die Maske setzt - fuer die Anzeige als "/24". */
static uint8_t mask_prefix(ip_addr_t mask)
{
    uint32_t host = ntohl(mask);
    uint8_t  bits = 0;

    while (host & 0x80000000u) {
        bits++;
        host <<= 1;
    }
    return bits;
}

void fw_rule_text(const struct fw_rule *rule, char *out, size_t size)
{
    if (!rule || !out || !size)
        return;

    char addr[24];
    char ports[24];

    if (rule->mask) {
        char base[20];

        ip_format(rule->addr, base, sizeof(base));
        ksnprintf(addr, sizeof(addr), "%s/%u", base,
                  (unsigned)mask_prefix(rule->mask));
    } else {
        strlcpy(addr, "alle", sizeof(addr));
    }

    if (rule->port_lo == 0 && rule->port_hi == 0xFFFF)
        strlcpy(ports, "alle", sizeof(ports));
    else if (rule->port_lo == rule->port_hi)
        ksnprintf(ports, sizeof(ports), "%u", (unsigned)rule->port_lo);
    else
        ksnprintf(ports, sizeof(ports), "%u-%u", (unsigned)rule->port_lo,
                  (unsigned)rule->port_hi);

    ksnprintf(out, size, "%-9s %-9s %-5s %-18s Port %s%s%s",
              rule->dir == FW_IN ? "eingehend" : "ausgehend",
              fw_action_name((enum fw_action)rule->action),
              fw_proto_name(rule->proto), addr, ports,
              rule->note[0] ? "  # " : "", rule->note);
}

bool fw_parse_action(const char *text, enum fw_action *out)
{
    if (!text || !out)
        return false;
    if (strcasecmp(text, "erlauben") == 0) { *out = FW_ALLOW;  return true; }
    if (strcasecmp(text, "verwerfen") == 0) { *out = FW_DROP;  return true; }
    if (strcasecmp(text, "ablehnen") == 0) { *out = FW_REJECT; return true; }
    return false;
}

bool fw_parse_proto(const char *text, uint8_t *out)
{
    if (!text || !out)
        return false;
    if (strcasecmp(text, "alle") == 0)  { *out = 0;             return true; }
    if (strcasecmp(text, "tcp") == 0)   { *out = IP_PROTO_TCP;  return true; }
    if (strcasecmp(text, "udp") == 0)   { *out = IP_PROTO_UDP;  return true; }
    if (strcasecmp(text, "icmp") == 0)  { *out = IP_PROTO_ICMP; return true; }
    return false;
}

bool fw_parse_target(const char *text, ip_addr_t *addr, uint8_t *prefix)
{
    if (!text || !addr || !prefix)
        return false;

    if (strcasecmp(text, "alle") == 0) {
        *addr = 0;
        *prefix = 0;
        return true;
    }

    char  copy[32];
    char *slash;

    strlcpy(copy, text, sizeof(copy));
    slash = strchr(copy, '/');

    unsigned bits = 32;

    if (slash) {
        *slash = '\0';

        const char *p = slash + 1;

        if (!*p)
            return false;
        bits = 0;
        while (*p >= '0' && *p <= '9')
            bits = bits * 10 + (unsigned)(*p++ - '0');
        if (*p || bits > 32)
            return false;
    }

    if (!ip_parse(copy, addr))
        return false;
    *prefix = (uint8_t)bits;
    return true;
}

bool fw_parse_ports(const char *text, uint16_t *lo, uint16_t *hi)
{
    if (!text || !lo || !hi)
        return false;

    if (strcasecmp(text, "alle") == 0) {
        *lo = 0;
        *hi = 0xFFFF;
        return true;
    }

    uint32_t first = 0, second = 0;
    const char *p = text;
    bool digits = false;

    while (*p >= '0' && *p <= '9') {
        first = first * 10 + (uint32_t)(*p++ - '0');
        digits = true;
    }
    if (!digits || first > 65535)
        return false;

    if (!*p) {
        *lo = (uint16_t)first;
        *hi = (uint16_t)first;
        return true;
    }
    if (*p != '-')
        return false;
    p++;

    digits = false;
    while (*p >= '0' && *p <= '9') {
        second = second * 10 + (uint32_t)(*p++ - '0');
        digits = true;
    }
    if (*p || !digits || second > 65535 || second < first)
        return false;

    *lo = (uint16_t)first;
    *hi = (uint16_t)second;
    return true;
}

/* ------------------------------------------------------------------ */
/* Datei                                                               */
/* ------------------------------------------------------------------ */

static const char *next_word(const char *text, char *out, size_t size)
{
    size_t n = 0;

    while (*text == ' ' || *text == '\t')
        text++;
    while (*text && *text != ' ' && *text != '\t' && n + 1 < size)
        out[n++] = *text++;
    out[n] = '\0';
    while (*text && *text != ' ' && *text != '\t')
        text++;
    return text;
}

static void read_line(const char *line)
{
    char word[32];
    const char *rest = next_word(line, word, sizeof(word));

    if (!word[0] || word[0] == '#')
        return;

    if (strcasecmp(word, "an") == 0) {
        active = true;
        return;
    }
    if (strcasecmp(word, "aus") == 0) {
        active = false;
        return;
    }

    if (strcasecmp(word, "standard") == 0) {
        char dir[16], act[16];

        rest = next_word(rest, dir, sizeof(dir));
        next_word(rest, act, sizeof(act));

        enum fw_action action;

        if (!fw_parse_action(act, &action))
            return;
        if (strcasecmp(dir, "eingehend") == 0)
            policy[FW_IN] = (uint8_t)action;
        else if (strcasecmp(dir, "ausgehend") == 0)
            policy[FW_OUT] = (uint8_t)action;
        return;
    }

    if (strcasecmp(word, "regel") != 0)
        return;

    char dir[16], act[16], proto[16], target[40], ports[24];

    rest = next_word(rest, dir, sizeof(dir));
    rest = next_word(rest, act, sizeof(act));
    rest = next_word(rest, proto, sizeof(proto));
    rest = next_word(rest, target, sizeof(target));
    rest = next_word(rest, ports, sizeof(ports));

    enum fw_action action;
    uint8_t   protocol, prefix;
    ip_addr_t addr;
    uint16_t  lo, hi;

    if (!fw_parse_action(act, &action) || !fw_parse_proto(proto, &protocol) ||
        !fw_parse_target(target, &addr, &prefix) ||
        !fw_parse_ports(ports, &lo, &hi))
        return;

    while (*rest == ' ' || *rest == '\t')
        rest++;

    fw_add(strcasecmp(dir, "ausgehend") == 0 ? FW_OUT : FW_IN, action,
           protocol, addr, prefix, lo, hi, rest);
}

bool fw_load(void)
{
    struct fs_node *file = fs_lookup(NULL, FW_PATH);

    if (!file || file->type != FS_FILE || !fs_load(file) || !file->data ||
        !file->size || file->size > 16384)
        return false;

    char *text = kmalloc(file->size + 1);

    if (!text)
        return false;

    memcpy(text, file->data, file->size);
    text[file->size] = '\0';

    fw_clear();

    char *line = text;

    while (line && *line) {
        char *next = strchr(line, '\n');

        if (next)
            *next++ = '\0';
        read_line(line);
        line = next;
    }

    kfree(text);
    log_info("firewall", "%u Regeln aus %s, Filter %s",
             (unsigned)fw_count(), FW_PATH, active ? "an" : "aus");
    return true;
}

bool fw_save(void)
{
    if (!fs_disk_mounted())
        return false;

    size_t cap = 8192;
    char  *text = kmalloc(cap);

    if (!text)
        return false;

    size_t used = 0;

    #define ADD(...) do {                                        \
        if (used < cap - 1) {                                    \
            ksnprintf(text + used, cap - used, __VA_ARGS__);     \
            used += strlen(text + used);                         \
        }                                                        \
    } while (0)

    ADD("# Paketfilter von RetroOS\n"
        "#\n"
        "# an | aus                     Filter ein- oder ausschalten\n"
        "# standard <richtung> <tat>    was gilt, wenn keine Regel passt\n"
        "# regel <richtung> <tat> <protokoll> <ziel> <ports> [notiz]\n"
        "#\n"
        "# Richtung: eingehend, ausgehend\n"
        "# Tat     : erlauben, verwerfen, ablehnen\n"
        "# Ziel    : alle, 10.0.2.15, 10.0.2.0/24\n"
        "# Ports   : alle, 8080, 1000-2000 - gemeint ist der eigene Port\n\n");

    ADD("%s\n", active ? "an" : "aus");
    ADD("standard eingehend %s\n", fw_action_name((enum fw_action)policy[FW_IN]));
    ADD("standard ausgehend %s\n\n",
        fw_action_name((enum fw_action)policy[FW_OUT]));

    for (size_t i = 0; i < FW_RULES_MAX; i++) {
        const struct fw_rule *r = &rules[i];

        if (!r->used)
            continue;

        char addr[24], ports[24];

        if (r->mask) {
            char base[20];

            ip_format(r->addr, base, sizeof(base));
            ksnprintf(addr, sizeof(addr), "%s/%u", base,
                      (unsigned)mask_prefix(r->mask));
        } else {
            strlcpy(addr, "alle", sizeof(addr));
        }

        if (r->port_lo == 0 && r->port_hi == 0xFFFF)
            strlcpy(ports, "alle", sizeof(ports));
        else if (r->port_lo == r->port_hi)
            ksnprintf(ports, sizeof(ports), "%u", (unsigned)r->port_lo);
        else
            ksnprintf(ports, sizeof(ports), "%u-%u", (unsigned)r->port_lo,
                      (unsigned)r->port_hi);

        ADD("regel %s %s %s %s %s%s%s\n",
            r->dir == FW_IN ? "eingehend" : "ausgehend",
            fw_action_name((enum fw_action)r->action),
            fw_proto_name(r->proto), addr, ports,
            r->note[0] ? " " : "", r->note);
    }
    #undef ADD

    struct fs_node *file = fs_lookup(NULL, FW_PATH);

    if (!file)
        file = fs_create_path(NULL, FW_PATH, FS_FILE);

    bool ok = file && file->type == FS_FILE && fs_write(file, text, used);

    kfree(text);
    return ok;
}
