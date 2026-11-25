/* netzkarte_test.c - prueft die Wortformate der Netzwerkkarten, die
 * sich ohne Karte pruefen lassen.
 *
 * Bei diesen Karten ist ein falsches Bit selten ein Absturz: Eine
 * negativ gerechnete Pufferlaenge, die um vier Bit verschobene
 * Ringgroesse im Initialisierungsblock, ein vergessenes Eigentumsbit -
 * die Karte tut dann einfach nichts, und das Netz ist "kaputt", ohne
 * dass jemand sagen koennte, wo.
 */

#include <stdio.h>
#include <string.h>

#include <stddef.h>

#include "pcnethw.h"
#include "vmxnet3hw.h"

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

static void pruefe_zahl(const char *was, long soll, long ist)
{
    geprueft++;
    if (soll != ist) {
        printf("  FEHLER: %s: erwartet %ld (0x%lx), bekommen %ld (0x%lx)\n",
               was, soll, soll, ist, ist);
        fehler++;
    }
}

/* --- PCnet ------------------------------------------------------------ */

static void test_pcnet_laenge(void)
{
    printf("PCnet: Pufferlaenge\n");

    /* 2048 Byte: -2048 in zwoelf Bit ist 0x800, dazu vier Einsen. */
    pruefe_zahl("2048", 0xF800, pcnet_buffer_length(2048));
    pruefe_zahl("1", 0xFFFF, pcnet_buffer_length(1));
    pruefe_zahl("60", 0xFFC4, pcnet_buffer_length(60));
    pruefe_zahl("1518", 0xFA12, pcnet_buffer_length(1518));
    pruefe_zahl("4095", 0xF001, pcnet_buffer_length(4095));
    pruefe_zahl("null geht nicht", 0, pcnet_buffer_length(0));
    pruefe_zahl("4096 geht nicht", 0, pcnet_buffer_length(4096));

    /* Die Laenge als vorzeichenbehaftete Zahl ist das Negative. */
    for (uint32_t n = 1; n < 4096; n++) {
        int16_t negativ = (int16_t)pcnet_buffer_length(n);

        if (negativ != -(int32_t)n) {
            pruefe_zahl("negative Laenge", -(long)n, negativ);
            break;
        }
    }
    pruefe("alle 4095 Laengen stimmen", true);
}

static void test_pcnet_ring(void)
{
    printf("PCnet: Ringgroesse\n");

    pruefe_zahl("1", 0, pcnet_ring_log2(1));
    pruefe_zahl("16", 4, pcnet_ring_log2(16));
    pruefe_zahl("512", 9, pcnet_ring_log2(512));
    pruefe_zahl("0", -1, pcnet_ring_log2(0));
    pruefe_zahl("12", -1, pcnet_ring_log2(12));
    pruefe_zahl("1024", -1, pcnet_ring_log2(1024));
}

static void test_pcnet_block(void)
{
    printf("PCnet: Initialisierungsblock\n");

    const uint8_t mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
    uint8_t b[PCNET_INIT_BYTES];

    pcnet_build_init(b, mac, 4, 3, 0x11223344u, 0x55667788u);

    pruefe_zahl("Groesse", 28, PCNET_INIT_BYTES);
    pruefe_zahl("Modus", 0, b[0] | (b[1] << 8));
    pruefe_zahl("Empfangsring 16 = 2^4 oben im Byte", 0x40, b[2]);
    pruefe_zahl("Sendering 8 = 2^3 oben im Byte", 0x30, b[3]);
    pruefe("Adresse", memcmp(b + 4, mac, 6) == 0);
    pruefe_zahl("reserviert", 0, b[10] | (b[11] << 8));

    int alle = 1;
    for (int i = 12; i < 20; i++)
        alle &= b[i] == 0xFF;
    pruefe("Mehrfachfilter offen", alle);

    pruefe_zahl("Empfangsring niedrigstes Byte", 0x44, b[20]);
    pruefe_zahl("Empfangsring hoechstes Byte", 0x11, b[23]);
    pruefe_zahl("Sendering niedrigstes Byte", 0x88, b[24]);
    pruefe_zahl("Sendering hoechstes Byte", 0x55, b[27]);

    /* Die Logarithmen duerfen sich nicht gegenseitig beeinflussen,
     * auch nicht bei Unsinn. */
    pcnet_build_init(b, mac, 0x1F, 0x1F, 0, 0);
    pruefe_zahl("Empfangslaenge beschnitten", 0xF0, b[2]);
    pruefe_zahl("Sendelaenge beschnitten", 0xF0, b[3]);
}

static void test_pcnet_deskriptor(void)
{
    printf("PCnet: Deskriptoren\n");

    pruefe_zahl("Deskriptor 16 Byte", 16, sizeof(struct pcnet_desc));
    pruefe_zahl("Eigentum", 0x8000, PCNET_DESC_OWN);

    pruefe("der Karte: nicht fertig", !pcnet_rx_ready(PCNET_DESC_OWN));
    pruefe("dem Kern: fertig", pcnet_rx_ready(0));

    uint16_t gut = PCNET_DESC_STP | PCNET_DESC_ENP;

    pruefe("ganzer Rahmen brauchbar", pcnet_rx_usable(gut));
    pruefe("mit Fehlerbit nicht", !pcnet_rx_usable(gut | PCNET_DESC_ERR));
    pruefe("nur Anfang nicht", !pcnet_rx_usable(PCNET_DESC_STP));
    pruefe("nur Ende nicht", !pcnet_rx_usable(PCNET_DESC_ENP));
    pruefe("ohne alles nicht", !pcnet_rx_usable(0));
    pruefe("noch bei der Karte nicht", !pcnet_rx_usable(gut | PCNET_DESC_OWN));

    pruefe_zahl("Nutzlaenge", 60, pcnet_rx_payload(64));
    pruefe_zahl("nur Pruefbytes", 0, pcnet_rx_payload(4));
    pruefe_zahl("kuerzer", 0, pcnet_rx_payload(3));
    pruefe_zahl("null", 0, pcnet_rx_payload(0));
    pruefe_zahl("nur zwoelf Bit zaehlen", 60, pcnet_rx_payload(0xF000u | 64));
    pruefe_zahl("groesster Rahmen", 4091, pcnet_rx_payload(0xFFF));
}

/* --- vmxnet3 ----------------------------------------------------------- */

static void test_vmx_layout(void)
{
    printf("vmxnet3: Speicheraufbau\n");

    /* Das Geraet liest die Beschreibung an festen Versaetzen. Die Zahlen
     * sind die der VMware-Definition; ein Abweichen um vier Byte laesst
     * das Geraet mit zufaelligen Ringgroessen arbeiten. */
    pruefe_zahl("Treiberinfo", 16, sizeof(struct vmx_driver_info));
    pruefe_zahl("Hauptkonfiguration", 72, sizeof(struct vmx_misc_conf));
    pruefe_zahl("Unterbrechungen", 40, sizeof(struct vmx_intr_conf));
    pruefe_zahl("Empfangsfilter", 528, sizeof(struct vmx_rx_filter_conf));
    pruefe_zahl("laenge variable Beschreibung", 16, sizeof(struct vmx_var_conf));
    pruefe_zahl("Geraetelesebereich", 688, sizeof(struct vmx_dev_read));
    pruefe_zahl("Gesamtbeschreibung", 720, sizeof(struct vmx_shared));

    pruefe_zahl("Magic", 0, offsetof(struct vmx_shared, magic));
    pruefe_zahl("Lesebereich ab 8", 8, offsetof(struct vmx_shared, dev_read));
    pruefe_zahl("Version", 8, offsetof(struct vmx_shared, dev_read.misc.info.version));
    pruefe_zahl("Warteschlangenadresse", 8 + 32,
                offsetof(struct vmx_shared, dev_read.misc.queue_desc_pa));
    pruefe_zahl("MTU", 8 + 48, offsetof(struct vmx_shared, dev_read.misc.mtu));
    pruefe_zahl("Sendewarteschlangen", 8 + 54,
                offsetof(struct vmx_shared, dev_read.misc.num_tx_queues));
    pruefe_zahl("Unterbrechungen ab 80", 80, offsetof(struct vmx_shared, dev_read.intr));
    pruefe_zahl("Sperre", 80 + 28, offsetof(struct vmx_shared, dev_read.intr.intr_ctrl));
    pruefe_zahl("Filter ab 120", 120, offsetof(struct vmx_shared, dev_read.rx_filter));
    pruefe_zahl("Empfangsmodus", 120, offsetof(struct vmx_shared, dev_read.rx_filter.rx_mode));
    pruefe_zahl("RSS ab 648", 648, offsetof(struct vmx_shared, dev_read.rss));
    pruefe_zahl("Plugin ab 680", 680, offsetof(struct vmx_shared, dev_read.plugin));

    pruefe_zahl("Sendewarteschlange 256", 256, sizeof(struct vmx_tx_queue_desc));
    pruefe_zahl("Empfangswarteschlange 256", 256, sizeof(struct vmx_rx_queue_desc));
    pruefe_zahl("Sendering-Adresse", 16, offsetof(struct vmx_tx_queue_desc, tx_ring_pa));
    pruefe_zahl("Sendegroesse", 16 + 40, offsetof(struct vmx_tx_queue_desc, tx_ring_size));
    pruefe_zahl("Sende-Unterbrechung", 16 + 56, offsetof(struct vmx_tx_queue_desc, intr_idx));
    pruefe_zahl("Empfangsring-Adresse", 16, offsetof(struct vmx_rx_queue_desc, rx_ring_pa));
    pruefe_zahl("Empfangsgroesse", 16 + 40, offsetof(struct vmx_rx_queue_desc, rx_ring_size));
    pruefe_zahl("Empfangs-Unterbrechung", 16 + 56, offsetof(struct vmx_rx_queue_desc, intr_idx));
}

static void test_vmx_ring(void)
{
    printf("vmxnet3: Ringe\n");

    struct vmx_ring r;

    vmx_ring_init(&r, 32);
    pruefe_zahl("beginnt bei null", 0, r.next);
    pruefe_zahl("Generation beginnt bei eins", 1, r.gen);

    for (int i = 0; i < 31; i++)
        vmx_ring_advance(&r);
    pruefe_zahl("vor dem Ende", 31, r.next);
    pruefe_zahl("Generation unveraendert", 1, r.gen);

    vmx_ring_advance(&r);
    pruefe_zahl("Umlauf", 0, r.next);
    pruefe_zahl("Generation gekippt", 0, r.gen);

    for (int i = 0; i < 32; i++)
        vmx_ring_advance(&r);
    pruefe_zahl("zweiter Umlauf", 0, r.next);
    pruefe_zahl("Generation wieder eins", 1, r.gen);

    pruefe("64 erlaubt", vmx_ring_size_ok(64));
    pruefe("32 erlaubt", vmx_ring_size_ok(32));
    pruefe("4096 erlaubt", vmx_ring_size_ok(4096));
    pruefe("0 nicht", !vmx_ring_size_ok(0));
    pruefe("48 nicht", !vmx_ring_size_ok(48));
    pruefe("4128 nicht", !vmx_ring_size_ok(4128));
    pruefe("31 nicht", !vmx_ring_size_ok(31));
}

static void test_vmx_deskriptoren(void)
{
    printf("vmxnet3: Deskriptoren\n");

    uint32_t d[4];

    vmx_tx_desc(d, 0x1122334455667788ull, 1514, 1);
    pruefe_zahl("Sende: Adresse unten", 0x55667788, d[0]);
    pruefe_zahl("Sende: Adresse oben", 0x11223344, d[1]);
    pruefe_zahl("Sende: Laenge", 1514, d[2] & 0x3FFF);
    pruefe_zahl("Sende: Generation Bit 14", 1, (d[2] >> 14) & 1);
    pruefe_zahl("Sende: sonst nichts im Wort", 0, d[2] >> 15);
    pruefe_zahl("Sende: Ende und Meldung", (1 << 12) | (1 << 13), d[3]);

    vmx_tx_desc(d, 0, 60, 0);
    pruefe_zahl("Sende: Generation null", 0, (d[2] >> 14) & 1);

    vmx_tx_desc(d, 0, 0xFFFF, 1);
    pruefe_zahl("Sende: Laenge beschnitten", 0x3FFF, d[2] & 0x3FFF);
    pruefe_zahl("Sende: Laenge lappt nicht in die Generation", 1, (d[2] >> 14) & 1);

    vmx_rx_desc(d, 0xAABBCCDD00112233ull, 2048, 1);
    pruefe_zahl("Empfang: Adresse unten", 0x00112233, d[0]);
    pruefe_zahl("Empfang: Adresse oben", (long)0xAABBCCDDu, d[1]);
    pruefe_zahl("Empfang: Laenge", 2048, d[2] & 0x3FFF);
    pruefe_zahl("Empfang: Pufferart Kopf", 0, (d[2] >> 14) & 1);
    pruefe_zahl("Empfang: Generation Bit 31", 1, d[2] >> 31);
    pruefe_zahl("Empfang: Rest null", 0, d[3]);

    vmx_rx_desc(d, 0, 2048, 0);
    pruefe_zahl("Empfang: Generation null", 0, d[2] >> 31);

    uint32_t c[4] = { 0, 0, 0, 0x80000000u };

    pruefe_zahl("Meldung Generation 1", 1, vmx_comp_gen(c));
    c[3] = 0x7FFFFFFFu;
    pruefe_zahl("Meldung Generation 0", 0, vmx_comp_gen(c));

    /* Eintrag 37, Anfang und Ende, Warteschlange 1, Laenge 1500. */
    uint32_t r[4] = { 37u | (1u << 14) | (1u << 15) | (1u << 16), 0,
                      1500u, 0x80000000u };
    struct vmx_rx_comp info;

    vmx_rx_comp_decode(r, &info);
    pruefe_zahl("Meldung: Eintrag", 37, info.index);
    pruefe_zahl("Meldung: Laenge", 1500, info.length);
    pruefe_zahl("Meldung: Warteschlange", 1, info.queue_id);
    pruefe("Meldung: Anfang", info.sop);
    pruefe("Meldung: Ende", info.eop);
    pruefe("Meldung: kein Fehler", !info.error);

    /* Anfang und Ende einzeln: nur Bit 15 ist der Anfang, nur Bit 14 das Ende. */
    r[0] = 5u | (1u << 15);
    vmx_rx_comp_decode(r, &info);
    pruefe("nur Anfang", info.sop && !info.eop);
    r[0] = 5u | (1u << 14);
    vmx_rx_comp_decode(r, &info);
    pruefe("nur Ende", !info.sop && info.eop);

    r[0] = 4095u;
    r[2] = 60u | (1u << 14);
    vmx_rx_comp_decode(r, &info);
    pruefe_zahl("groesster Eintrag", 4095, info.index);
    pruefe("Fehlerbit", info.error);
    pruefe("weder Anfang noch Ende", !info.sop && !info.eop);

    uint32_t t[4] = { 0xFFFFF000u | 63u, 0, 0, 0x80000000u };

    pruefe_zahl("Sendemeldung: nur zwoelf Bit", 63, vmx_tx_comp_index(t));

    pruefe_zahl("Gastsystem 64 Bit", 2, vmx_gos_word() & 3);
    pruefe_zahl("Gastsystem Linux", 1, (vmx_gos_word() >> 2) & 15);

    pruefe("Verbindung da", vmx_link_up(0x03E80001u));
    pruefe("Verbindung weg", !vmx_link_up(0x03E80000u));
    pruefe_zahl("Geschwindigkeit", 1000, vmx_link_speed(0x03E80001u));
}

int main(void)
{
    printf("=== Netzwerkkarten ===\n");

    test_pcnet_laenge();
    test_pcnet_ring();
    test_pcnet_block();
    test_pcnet_deskriptor();
    test_vmx_layout();
    test_vmx_ring();
    test_vmx_deskriptoren();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
