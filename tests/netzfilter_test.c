/* netzfilter_test.c - prueft den Paketfilter.
 *
 * Die Regelpruefung ist die Stelle, an der ein Filter falsch sein kann,
 * ohne dass es jemand merkt: Eine Regel, die zu viel durchlaesst, faellt
 * erst auf, wenn es zu spaet ist. Deshalb wird hier jede Achse einzeln
 * geprueft - Richtung, Protokoll, Adresse samt Maske, Portbereich - und
 * danach die Reihenfolge, in der Regeln entscheiden.
 */

#include <stdio.h>
#include <string.h>

#include "firewall.h"
#include "vfs.h"

static int fehler;
static int geprueft;

static void pruefe(const char *was, bool bedingung)
{
    geprueft++;
    if (!bedingung) {
        printf("  FEHLER: %s\n", was);
        fehler++;
    }
}

static void pruefe_text(const char *was, const char *soll, const char *ist)
{
    geprueft++;
    if (strcmp(soll, ist) != 0) {
        printf("  FEHLER: %s - erwartet \"%s\", bekommen \"%s\"\n",
               was, soll, ist);
        fehler++;
    }
}

/* Das Netz ist hier nicht angeschlossen; der Filter braucht davon nur
 * die Adressumwandlung, und die steht in net.h als inline. */
struct netif g_netif;

void ip_format(ip_addr_t addr, char *buf, size_t size)
{
    uint32_t host = ntohl(addr);

    snprintf(buf, size, "%u.%u.%u.%u", (host >> 24) & 0xFF,
             (host >> 16) & 0xFF, (host >> 8) & 0xFF, host & 0xFF);
}

bool ip_parse(const char *text, ip_addr_t *out)
{
    unsigned a, b, c, d;

    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    *out = ip_make((uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d);
    return true;
}

/* Protokoll und Pruefspur laufen im Test ins Leere. */
void log_write(int level, const char *source, const char *fmt, ...)
{
    (void)level; (void)source; (void)fmt;
}
void audit(int kind, bool ok, const char *fmt, ...)
{
    (void)kind; (void)ok; (void)fmt;
}

/* Der Dateibaum wird nur beim Sichern und Lesen angefasst; hier ist er
 * ein Puffer und ein einzelner Knoten. */
static char           datei[16384];
static size_t         datei_laenge;
static struct fs_node der_knoten;

bool fs_disk_mounted(void) { return true; }

struct fs_node *fs_lookup(struct fs_node *base, const char *path)
{
    (void)base; (void)path;
    if (!datei_laenge)
        return NULL;
    der_knoten.type = FS_FILE;
    der_knoten.data = (uint8_t *)datei;
    der_knoten.size = datei_laenge;
    return &der_knoten;
}

struct fs_node *fs_create_path(struct fs_node *base, const char *path,
                               enum fs_type type)
{
    (void)base; (void)path;
    der_knoten.type = (uint8_t)type;
    return &der_knoten;
}

bool fs_write(struct fs_node *file, const void *data, size_t size)
{
    (void)file;
    if (size >= sizeof(datei))
        return false;
    memcpy(datei, data, size);
    datei[size] = '\0';
    datei_laenge = size;
    der_knoten.data = (uint8_t *)datei;
    der_knoten.size = size;
    return true;
}

bool fs_load(struct fs_node *file) { (void)file; return true; }

/* ------------------------------------------------------------------ */

static ip_addr_t adr(const char *text)
{
    ip_addr_t a = 0;

    ip_parse(text, &a);
    return a;
}

static void test_grundeinstellung(void)
{
    printf("Grundeinstellung\n");

    fw_init();

    pruefe("Frisch ist der Filter aus", !fw_enabled());
    pruefe("Und laesst alles durch",
           fw_check(FW_IN, adr("1.2.3.4"), IP_PROTO_TCP, 22, 5000));

    /* Ausgeschaltet gilt auch eine strenge Grundeinstellung nicht -
     * sonst waere "aus" keine Aussage, sondern eine Falle. */
    fw_set_policy(FW_IN, FW_DROP);
    pruefe("Ausgeschaltet bleibt alles offen",
           fw_check(FW_IN, adr("1.2.3.4"), IP_PROTO_TCP, 22, 5000));

    fw_enable(true);
    pruefe("Eingeschaltet greift sie",
           !fw_check(FW_IN, adr("1.2.3.4"), IP_PROTO_TCP, 22, 5000));
    pruefe("Die andere Richtung bleibt frei",
           fw_check(FW_OUT, adr("1.2.3.4"), IP_PROTO_TCP, 5000, 80));

    pruefe("Sie wird auch gezaehlt", fw_dropped(FW_IN) == 1);
    pruefe("Und das Durchgelassene auch", fw_passed(FW_OUT) == 1);
}

static void test_regeln(void)
{
    printf("Regeln\n");

    fw_init();
    fw_enable(true);
    fw_set_policy(FW_IN, FW_DROP);

    /* Ein einziges offenes Tor: Port 8080 fuer alle. */
    pruefe("Regel angelegt",
           fw_add(FW_IN, FW_ALLOW, IP_PROTO_TCP, 0, 0, 8080, 8080,
                  "Webserver") >= 0);

    pruefe("8080 kommt durch",
           fw_check(FW_IN, adr("9.9.9.9"), IP_PROTO_TCP, 8080, 40000));
    pruefe("8081 nicht",
           !fw_check(FW_IN, adr("9.9.9.9"), IP_PROTO_TCP, 8081, 40000));
    pruefe("UDP auf 8080 auch nicht",
           !fw_check(FW_IN, adr("9.9.9.9"), IP_PROTO_UDP, 8080, 40000));

    /* Eine Regel mit Ports kann fuer ICMP nicht gemeint sein. */
    pruefe("ICMP faellt unter die Grundeinstellung",
           !fw_check(FW_IN, adr("9.9.9.9"), IP_PROTO_ICMP, 0, 0));

    /* Adresse mit Maske. */
    fw_init();
    fw_enable(true);
    fw_set_policy(FW_IN, FW_DROP);
    fw_add(FW_IN, FW_ALLOW, 0, adr("10.0.2.0"), 24, 0, 65535, "eigenes Netz");

    pruefe("Aus dem eigenen Netz",
           fw_check(FW_IN, adr("10.0.2.15"), IP_PROTO_TCP, 22, 1234));
    pruefe("Auch das andere Ende davon",
           fw_check(FW_IN, adr("10.0.2.254"), IP_PROTO_ICMP, 0, 0));
    pruefe("Von nebenan nicht",
           !fw_check(FW_IN, adr("10.0.3.1"), IP_PROTO_TCP, 22, 1234));

    /* Eine einzelne Adresse. */
    fw_init();
    fw_enable(true);
    fw_add(FW_IN, FW_DROP, 0, adr("192.168.1.7"), 32, 0, 65535, "Stoerenfried");

    pruefe("Der Stoerenfried kommt nicht durch",
           !fw_check(FW_IN, adr("192.168.1.7"), IP_PROTO_TCP, 80, 1));
    pruefe("Sein Nachbar schon",
           fw_check(FW_IN, adr("192.168.1.8"), IP_PROTO_TCP, 80, 1));

    /* Portbereiche. */
    fw_init();
    fw_enable(true);
    fw_set_policy(FW_OUT, FW_DROP);
    fw_add(FW_OUT, FW_ALLOW, IP_PROTO_TCP, 0, 0, 1024, 65535, "hohe Ports");

    pruefe("1023 nicht",  !fw_check(FW_OUT, adr("1.1.1.1"), IP_PROTO_TCP,
                                    1023, 80));
    pruefe("1024 schon",  fw_check(FW_OUT, adr("1.1.1.1"), IP_PROTO_TCP,
                                   1024, 80));
    pruefe("65535 auch",  fw_check(FW_OUT, adr("1.1.1.1"), IP_PROTO_TCP,
                                   65535, 80));
}

static void test_reihenfolge(void)
{
    printf("Reihenfolge\n");

    fw_init();
    fw_enable(true);
    fw_set_policy(FW_IN, FW_ALLOW);

    /* Die erste passende Regel entscheidet - auch wenn eine spaetere
     * das Gegenteil sagt. Genau darauf verlaesst sich jeder, der eine
     * Ausnahme vor eine allgemeine Sperre setzt. */
    fw_add(FW_IN, FW_ALLOW, IP_PROTO_TCP, adr("10.0.0.5"), 32, 22, 22,
           "der eine darf");
    fw_add(FW_IN, FW_DROP, IP_PROTO_TCP, 0, 0, 22, 22, "sonst niemand");

    pruefe("Die Ausnahme greift",
           fw_check(FW_IN, adr("10.0.0.5"), IP_PROTO_TCP, 22, 1234));
    pruefe("Die Sperre dahinter auch",
           !fw_check(FW_IN, adr("10.0.0.6"), IP_PROTO_TCP, 22, 1234));

    const struct fw_rule *erste = fw_at(0);
    const struct fw_rule *zweite = fw_at(1);

    pruefe("Die erste zaehlt ihre Treffer", erste && erste->hits == 1);
    pruefe("Die zweite ihre",               zweite && zweite->hits == 1);

    pruefe("Entfernen geht",     fw_remove(0));
    pruefe("Danach ist sie weg", fw_at(0) == NULL);
    pruefe("Und die Sperre gilt fuer alle",
           !fw_check(FW_IN, adr("10.0.0.5"), IP_PROTO_TCP, 22, 1234));

    pruefe("Zweimal entfernen geht nicht", !fw_remove(0));
    pruefe("Eine Regel ist uebrig",        fw_count() == 1);

    fw_clear();
    pruefe("Nach dem Leeren keine mehr", fw_count() == 0);
}

static void test_text(void)
{
    printf("Regeln als Text\n");

    enum fw_action action;
    uint8_t   proto, prefix;
    ip_addr_t addr;
    uint16_t  lo, hi;

    pruefe("erlauben",  fw_parse_action("erlauben", &action) &&
                        action == FW_ALLOW);
    pruefe("verwerfen", fw_parse_action("verwerfen", &action) &&
                        action == FW_DROP);
    pruefe("ablehnen",  fw_parse_action("ablehnen", &action) &&
                        action == FW_REJECT);
    pruefe("Unsinn nicht", !fw_parse_action("vielleicht", &action));

    pruefe("tcp",  fw_parse_proto("tcp", &proto) && proto == IP_PROTO_TCP);
    pruefe("udp",  fw_parse_proto("udp", &proto) && proto == IP_PROTO_UDP);
    pruefe("icmp", fw_parse_proto("icmp", &proto) && proto == IP_PROTO_ICMP);
    pruefe("alle", fw_parse_proto("alle", &proto) && proto == 0);
    pruefe("sctp nicht", !fw_parse_proto("sctp", &proto));

    pruefe("Einzelne Adresse",
           fw_parse_target("10.0.2.15", &addr, &prefix) && prefix == 32);
    pruefe("Mit Maske",
           fw_parse_target("10.0.2.0/24", &addr, &prefix) && prefix == 24);
    pruefe("alle",
           fw_parse_target("alle", &addr, &prefix) && prefix == 0 && addr == 0);
    pruefe("33 Bit gibt es nicht", !fw_parse_target("10.0.0.0/33", &addr,
                                                    &prefix));
    pruefe("Ohne Zahl hinter dem Strich auch nicht",
           !fw_parse_target("10.0.0.0/", &addr, &prefix));
    pruefe("Kein Unsinn", !fw_parse_target("hier", &addr, &prefix));

    pruefe("Ein Port",   fw_parse_ports("22", &lo, &hi) && lo == 22 && hi == 22);
    pruefe("Ein Bereich", fw_parse_ports("1000-2000", &lo, &hi) &&
                          lo == 1000 && hi == 2000);
    pruefe("alle",       fw_parse_ports("alle", &lo, &hi) && lo == 0 &&
                         hi == 65535);
    pruefe("Verkehrt herum nicht", !fw_parse_ports("2000-1000", &lo, &hi));
    pruefe("Zu gross nicht",       !fw_parse_ports("70000", &lo, &hi));
    pruefe("Leer nicht",           !fw_parse_ports("", &lo, &hi));

    /* Die Textform soll man vorlesen koennen. */
    fw_init();
    fw_add(FW_IN, FW_DROP, IP_PROTO_TCP, adr("10.0.2.0"), 24, 22, 22, "kein SSH");

    char text[96];

    fw_rule_text(fw_at(0), text, sizeof(text));
    pruefe_text("Die ganze Regel",
                "eingehend verwerfen tcp   10.0.2.0/24        "
                "Port 22  # kein SSH", text);
}

static void test_datei(void)
{
    printf("Regeln schreiben und lesen\n");

    fw_init();
    fw_enable(true);
    fw_set_policy(FW_IN, FW_DROP);
    fw_add(FW_IN, FW_ALLOW, IP_PROTO_TCP, 0, 0, 8080, 8080, "Webserver");
    fw_add(FW_IN, FW_ALLOW, 0, adr("10.0.2.0"), 24, 0, 65535, "eigenes Netz");
    fw_add(FW_OUT, FW_DROP, IP_PROTO_UDP, adr("8.8.8.8"), 32, 53, 53, NULL);

    datei_laenge = 0;
    pruefe("Schreiben geht", fw_save());
    pruefe("Es steht etwas darin", datei_laenge > 0);
    pruefe("Der Zustand auch", strstr(datei, "\nan\n") != NULL);
    pruefe("Und die Regeln",
           strstr(datei, "regel eingehend erlauben tcp alle 8080") != NULL);

    fw_init();
    pruefe("Nach dem Zuruecksetzen keine Regeln", fw_count() == 0);
    pruefe("Und der Filter ist aus", !fw_enabled());

    pruefe("Lesen geht", fw_load());
    pruefe("Drei Regeln wieder da", fw_count() == 3);
    pruefe("Der Filter ist wieder an", fw_enabled());
    pruefe("Die Grundeinstellung auch", fw_policy(FW_IN) == FW_DROP);

    /* Und sie wirken wieder wie vorher. */
    pruefe("8080 kommt durch",
           fw_check(FW_IN, adr("9.9.9.9"), IP_PROTO_TCP, 8080, 1));
    pruefe("Der Rest nicht",
           !fw_check(FW_IN, adr("9.9.9.9"), IP_PROTO_TCP, 22, 1));
    pruefe("Aus dem eigenen Netz schon",
           fw_check(FW_IN, adr("10.0.2.3"), IP_PROTO_TCP, 22, 1));

    const struct fw_rule *dritte = fw_at(2);

    pruefe("Die dritte ist ausgehend", dritte && dritte->dir == FW_OUT);
    pruefe("Und trifft nur den einen Namensserver",
           dritte && dritte->port_lo == 53 && dritte->port_hi == 53);
}

int main(void)
{
    printf("=== Paketfilter ===\n");

    test_grundeinstellung();
    test_regeln();
    test_reihenfolge();
    test_text();
    test_datei();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
