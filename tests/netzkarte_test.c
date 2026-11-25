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

#include "pcnethw.h"

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

int main(void)
{
    printf("=== Netzwerkkarten ===\n");

    test_pcnet_laenge();
    test_pcnet_ring();
    test_pcnet_block();
    test_pcnet_deskriptor();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
