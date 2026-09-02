/* firewall.h - der Paketfilter.
 *
 * Bisher nahm RetroOS jedes Paket an, das an seine Adresse ging, und
 * schickte jedes los, das ein Programm abgab. Fuer einen Rechner am
 * Netz ist das zu wenig: Wer einen Webserver betreibt, will Port 8080
 * offen haben und sonst nichts, und wer ihn nicht betreibt, will gar
 * nichts offen haben.
 *
 * Der Filter ist eine Liste von Regeln je Richtung. Geprueft wird von
 * oben nach unten, die erste passende entscheidet; passt keine, gilt
 * die Grundeinstellung dieser Richtung. Das ist das Modell von
 * nftables und der Windows-Firewall - es ist deshalb so verbreitet,
 * weil man eine Regelliste von oben lesen und dabei laut mitsprechen
 * kann.
 *
 * Eine Regel trifft auf die Gegenstelle zu, nicht auf "Quelle" und
 * "Ziel": Bei einem eingehenden Paket ist die Gegenstelle der Absender,
 * bei einem ausgehenden der Empfaenger. Das spart die Haelfte der
 * Felder und die immer wiederkehrende Frage, welche Seite gerade
 * gemeint ist.
 *
 * Der Filter sitzt an genau zwei Stellen: in ip_receive(), bevor das
 * Paket an ICMP, UDP oder TCP geht, und in ip_send_via(), bevor es die
 * Karte erreicht. Alles darueber muss nichts davon wissen.
 */
#ifndef FIREWALL_H
#define FIREWALL_H

#include "retro.h"
#include "net.h"

#define FW_RULES_MAX 32
#define FW_NOTE_MAX  31
#define FW_PATH      "/Festplatte/firewall.conf"

enum fw_dir {
    FW_IN,
    FW_OUT,
    FW_DIR_COUNT
};

enum fw_action {
    FW_ALLOW,       /* durchlassen                                  */
    FW_DROP,        /* wegwerfen, ohne etwas zu sagen               */
    FW_REJECT       /* wegwerfen und dem Absender Bescheid geben     */
};

struct fw_rule {
    bool      used;
    uint8_t   dir;
    uint8_t   action;
    uint8_t   proto;        /* 0 = jedes, sonst IP_PROTO_*          */
    ip_addr_t addr;         /* Gegenstelle                          */
    ip_addr_t mask;         /* 0 = jede Adresse                     */
    uint16_t  port_lo;      /* eigener Port; 0-65535 = jeder        */
    uint16_t  port_hi;
    uint64_t  hits;
    char      note[FW_NOTE_MAX + 1];
};

void fw_init(void);

bool fw_enabled(void);
void fw_enable(bool on);

enum fw_action fw_policy(enum fw_dir dir);
void           fw_set_policy(enum fw_dir dir, enum fw_action action);

/* Haengt eine Regel hinten an. Liefert ihre Nummer oder -1. Das
 * Praefix ist die Laenge der Netzmaske; 0 heisst "jede Adresse". */
int  fw_add(enum fw_dir dir, enum fw_action action, uint8_t proto,
            ip_addr_t addr, uint8_t prefix, uint16_t port_lo,
            uint16_t port_hi, const char *note);
bool fw_remove(size_t index);
void fw_clear(void);

size_t                fw_count(void);
const struct fw_rule *fw_at(size_t index);

/* Der Pruefpunkt. local_port ist der Port auf dieser Maschine,
 * peer_port der auf der anderen; bei ICMP sind beide 0. */
bool fw_check(enum fw_dir dir, ip_addr_t peer, uint8_t proto,
              uint16_t local_port, uint16_t peer_port);

uint64_t fw_dropped(enum fw_dir dir);
uint64_t fw_passed(enum fw_dir dir);

/* Text fuer die Anzeige: "verwerfen tcp 0.0.0.0/0 Port 22". */
void fw_rule_text(const struct fw_rule *rule, char *out, size_t size);
const char *fw_action_name(enum fw_action action);
const char *fw_proto_name(uint8_t proto);
bool fw_parse_action(const char *text, enum fw_action *out);
bool fw_parse_proto(const char *text, uint8_t *out);
/* "10.0.2.0/24", "10.0.2.15" oder "alle". */
bool fw_parse_target(const char *text, ip_addr_t *addr, uint8_t *prefix);
/* "22", "1000-2000" oder "alle". */
bool fw_parse_ports(const char *text, uint16_t *lo, uint16_t *hi);

bool fw_load(void);
bool fw_save(void);

#endif /* FIREWALL_H */
