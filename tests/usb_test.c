/* usb_test.c - prueft, was am USB ohne Hardware pruefbar ist: das Lesen
 * einer Konfigurationsbeschreibung und das Zusammensetzen der OHCI-
 * Worte.
 *
 * Beides sind Stellen, an denen ein Irrtum keine Fehlermeldung ergibt:
 * ein falsch gesetztes Bit im Endpunktwort macht aus einem Eingang einen
 * Ausgang, eine verrutschte Laenge laesst den Parser in den Bytes des
 * naechsten Deskriptors lesen. Der Controller tut dann einfach nichts -
 * und ein Geraet, das nicht auftaucht, sieht von aussen aus wie ein
 * leerer Anschluss.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ohcihw.h"
#include "usbconfig.h"
#include "usbhub.h"

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
        printf("  FEHLER: %s: erwartet %ld, bekommen %ld\n", was, soll, ist);
        fehler++;
    }
}

/* --- Konfigurationen, wie echte Geraete sie liefern ------------------- */

/* usb-tablet: eine HID-Schnittstelle ohne Boot-Protokoll, Berichts-
 * beschreibung 74 Byte, Unterbrechungseingang 0x81, 8 Byte, 10 ms. */
static const uint8_t tablett[] = {
    9, 2, 34, 0, 1, 1, 4, 0xA0, 50,
    9, 4, 0, 0, 1, 3, 0, 0, 0,
    9, 0x21, 0x01, 0x00, 0, 1, 0x22, 74, 0,
    7, 5, 0x81, 3, 8, 0, 10,
};

/* Tastatur mit Boot-Protokoll: Klasse 3, Unterklasse 1, Protokoll 1. */
static const uint8_t tastatur[] = {
    9, 2, 34, 0, 1, 1, 4, 0xA0, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 63, 0,
    7, 5, 0x81, 3, 8, 0, 10,
};

/* Speicher: Klasse 8, nur Massenendpunkte, keiner mit Unterbrechung. */
static const uint8_t speicher[] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 8, 6, 0x50, 0,
    7, 5, 0x81, 2, 0, 2, 0,
    7, 5, 0x02, 2, 0, 2, 0,
};

/* Zwei Schnittstellen (Tastatur und Zeiger in einem Geraet): die erste
 * mit Unterbrechungseingang gilt, die zweite nicht mehr. */
static const uint8_t doppelt[] = {
    9, 2, 59, 0, 2, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 1, 3, 1, 1, 0,
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 63, 0,
    7, 5, 0x81, 3, 8, 0, 10,
    9, 4, 1, 0, 1, 3, 1, 2, 0,
    7, 5, 0x82, 3, 4, 0, 10,
};

/* Ein Geraet mit Unterbrechungsausgang hinter dem Eingang (so haben es
 * manche Tastaturen fuer die Leuchtanzeigen): Der Ausgang darf nicht
 * als Eingang gelten, sonst wird auf ihm vergeblich gelesen. */
static const uint8_t mit_ausgang[] = {
    9, 2, 41, 0, 1, 1, 0, 0xA0, 50,
    9, 4, 0, 0, 2, 3, 1, 1, 0,
    7, 5, 0x81, 3, 8, 0, 10,
    7, 5, 0x01, 3, 8, 0, 4,
};

static void test_ausgang(void)
{
    struct usb_config c;

    printf("Unterbrechungsausgang\n");
    pruefe("gelesen", usb_parse_config(mit_ausgang, sizeof(mit_ausgang), &c));
    pruefe_zahl("nur der Eingang zaehlt", 0x81, c.int_address);
    pruefe_zahl("sein Abstand", 10, c.int_interval);
}

static void test_tablett(void)
{
    struct usb_config c;

    printf("Tablett\n");
    pruefe("gelesen", usb_parse_config(tablett, sizeof(tablett), &c));
    pruefe_zahl("Konfigurationswert", 1, c.configuration);
    pruefe_zahl("Klasse", 3, c.interface_class);
    pruefe_zahl("Unterklasse", 0, c.interface_subclass);
    pruefe_zahl("Berichtslaenge", 74, c.hid_report_length);
    pruefe_zahl("Eingang", 0x81, c.int_address);
    pruefe_zahl("Paketgroesse", 8, c.int_packet);
    pruefe_zahl("Abstand", 10, c.int_interval);
    pruefe_zahl("kein Massenendpunkt", 0, c.bulk_in + c.bulk_out);
}

static void test_tastatur(void)
{
    struct usb_config c;

    printf("Tastatur\n");
    pruefe("gelesen", usb_parse_config(tastatur, sizeof(tastatur), &c));
    pruefe_zahl("Unterklasse Boot", 1, c.interface_subclass);
    pruefe_zahl("Protokoll Tastatur", 1, c.interface_protocol);
    pruefe_zahl("Berichtslaenge", 63, c.hid_report_length);
}

static void test_speicher(void)
{
    struct usb_config c;

    printf("Speicher\n");
    pruefe("gelesen", usb_parse_config(speicher, sizeof(speicher), &c));
    pruefe_zahl("Klasse", 8, c.interface_class);
    pruefe_zahl("kein Unterbrechungseingang", 0, c.int_address);
    pruefe_zahl("Massen ein", 1, c.bulk_in);
    pruefe_zahl("Massen aus", 2, c.bulk_out);
    pruefe_zahl("Paket ein", 512, c.bulk_in_packet);
    pruefe_zahl("Paket aus", 512, c.bulk_out_packet);
}

static void test_doppelt(void)
{
    struct usb_config c;

    printf("zwei Schnittstellen\n");
    pruefe("gelesen", usb_parse_config(doppelt, sizeof(doppelt), &c));
    pruefe_zahl("erste gilt", 0, c.interface_number);
    pruefe_zahl("ihr Eingang", 0x81, c.int_address);
    pruefe_zahl("Protokoll bleibt das der ersten", 1, c.interface_protocol);
}

static void test_kaputt(void)
{
    struct usb_config c;

    printf("kaputte Eingaben\n");
    pruefe("leer", !usb_parse_config(NULL, 0, &c));
    pruefe("zu kurz", !usb_parse_config(tablett, 5, &c));
    pruefe("ohne Ausgabe", !usb_parse_config(tablett, sizeof(tablett), NULL));

    /* Laenge null darf nicht endlos kreisen. */
    uint8_t null_laenge[] = { 9, 2, 12, 0, 1, 1, 0, 0, 0, 0, 0, 0 };
    pruefe("Laenge null endet", !usb_parse_config(null_laenge, sizeof(null_laenge), &c));

    /* Ein Deskriptor, der ueber das Ende hinausreicht, wird nicht mehr
     * gelesen: die Schnittstelle ist da, der Endpunkt nicht. */
    pruefe("abgeschnitten",
           usb_parse_config(tablett, sizeof(tablett) - 3, &c));
    pruefe_zahl("kein Eingang aus dem Abgeschnittenen", 0, c.int_address);
}

/* Zufallsbytes: Es darf nichts ausserhalb lesen (unter ASan geprueft)
 * und nichts haengen. */
static void test_zufall(void)
{
    uint8_t buf[96];
    struct usb_config c;
    unsigned seed = 12345;

    printf("Zufall\n");
    for (int n = 0; n < 20000; n++) {
        size_t len = (size_t)(n % (int)sizeof(buf));
        uint8_t *heap = malloc(len ? len : 1);

        for (size_t i = 0; i < len; i++) {
            seed = seed * 1103515245u + 12345u;
            buf[i] = (uint8_t)(seed >> 16);
            /* oft echte Deskriptorarten, damit die Zweige drankommen */
            if (i % 7 == 1)
                buf[i] = (uint8_t)(4 + (seed >> 24) % 3);
            heap[i] = buf[i];
        }
        usb_parse_config(heap, len, &c);
        free(heap);
    }
    pruefe("ohne Absturz", true);
}

/* --- OHCI-Worte --------------------------------------------------------- */

static void test_endpunktwort(void)
{
    printf("Endpunktwort\n");

    uint32_t w = ohci_ed_word0(5, 1, OHCI_DIR_IN, true, true, 8);

    pruefe_zahl("Adresse", 5, w & 0x7F);
    pruefe_zahl("Nummer", 1, (w >> 7) & 0xF);
    pruefe_zahl("Richtung", OHCI_DIR_IN, (w >> 11) & 3);
    pruefe_zahl("langsam", 1, (w >> 13) & 1);
    pruefe_zahl("ueberspringen", 1, (w >> 14) & 1);
    pruefe_zahl("Format null", 0, (w >> 15) & 1);
    pruefe_zahl("Paketgroesse", 8, (w >> 16) & 0x7FF);

    /* Nichts darf ins Nachbarfeld laufen. */
    w = ohci_ed_word0(0xFF, 0xFF, 0xFF, false, false, 0xFFFF);
    pruefe_zahl("Adresse beschnitten", 0x7F, w & 0x7F);
    pruefe_zahl("Nummer beschnitten", 0xF, (w >> 7) & 0xF);
    pruefe_zahl("Richtung beschnitten", 3, (w >> 11) & 3);
    pruefe_zahl("nicht langsam", 0, (w >> 13) & 1);
    pruefe_zahl("Paket beschnitten", 0x7FF, (w >> 16) & 0xFFFF);
}

static void test_transferwort(void)
{
    printf("Transferwort\n");

    uint32_t w = ohci_td_word0(OHCI_PID_IN, OHCI_TOGGLE_DATA1, true);

    pruefe_zahl("Runden", 1, (w >> 18) & 1);
    pruefe_zahl("Kennung", OHCI_PID_IN, (w >> 19) & 3);
    pruefe_zahl("keine Unterbrechung", 7, (w >> 21) & 7);
    pruefe_zahl("Datenwechsel", OHCI_TOGGLE_DATA1, (w >> 24) & 3);
    pruefe_zahl("Fehlerzahl", 0, (w >> 26) & 3);
    pruefe_zahl("noch nicht angefasst", OHCI_CC_NOT_ACCESSED, ohci_td_cc(w));
    pruefe("nicht fertig", !ohci_td_done(w));

    w = (w & 0x0FFFFFFFu) | ((uint32_t)OHCI_CC_STALL << 28);
    pruefe("fertig mit Fehler", ohci_td_done(w));
    pruefe_zahl("Ergebnis", OHCI_CC_STALL, ohci_td_cc(w));

    pruefe("14 ist nicht fertig", !ohci_td_done(0xE0000000u));
    pruefe("13 ist fertig", ohci_td_done(0xD0000000u));
    pruefe("0 ist fertig", ohci_td_done(0));
    pruefe("Name 0", strcmp(ohci_cc_name(0), "kein Fehler") == 0);
    pruefe("Name unbekannt", strcmp(ohci_cc_name(10), "unbekannt") == 0);
}

static void test_puffer(void)
{
    printf("Puffer\n");
    pruefe_zahl("Ende bei Laenge null", 0, ohci_buffer_end(0x1000, 0));
    pruefe_zahl("Ende letztes Byte", 0x1007, ohci_buffer_end(0x1000, 8));

    pruefe_zahl("alles uebertragen", 8, ohci_transferred(0, 0x1000, 8));
    pruefe_zahl("halb", 3, ohci_transferred(0x1003, 0x1000, 8));
    pruefe_zahl("nichts", 0, ohci_transferred(0x1000, 0x1000, 8));
    pruefe_zahl("Zeiger davor", 0, ohci_transferred(0x0F00, 0x1000, 8));
    pruefe_zahl("Zeiger dahinter", 0, ohci_transferred(0x2000, 0x1000, 8));
}

static void test_rahmen(void)
{
    printf("Rahmen\n");

    /* Normallaenge, wenn das Register nichts Brauchbares enthaelt. */
    uint32_t w = ohci_frame_interval(0);

    pruefe_zahl("Normallaenge", 11999, w & 0x3FFF);
    pruefe_zahl("Umschaltbit wechselt", 1, w >> 31);
    pruefe_zahl("groesstes Paket", ((11999 - 210) * 6) / 7, (w >> 16) & 0x7FFF);

    w = ohci_frame_interval(1u << 31);
    pruefe_zahl("Umschaltbit wechselt zurueck", 0, w >> 31);

    w = ohci_frame_interval(2000);
    pruefe_zahl("eigene Laenge gilt", 2000, w & 0x3FFF);

    w = ohci_frame_interval(100);
    pruefe_zahl("zu kurz gilt nicht", 11999, w & 0x3FFF);

    pruefe_zahl("neun Zehntel", 10799, ohci_periodic_start(11999));
}

static void test_anschluss(void)
{
    printf("Anschluss\n");
    pruefe("angeschlossen", ohci_port_connected(OHCI_PORT_CCS));
    pruefe("leer, nur Aenderungen",
           !ohci_port_connected(OHCI_PORT_CSC | OHCI_PORT_PRSC | OHCI_PORT_PPS));
    pruefe_zahl("Full Speed", 1, ohci_port_speed(OHCI_PORT_CCS));
    pruefe_zahl("Low Speed", 2, ohci_port_speed(OHCI_PORT_CCS | OHCI_PORT_LSDA));
}

/* --- Verteiler ---------------------------------------------------------- */

static void test_verteiler(void)
{
    printf("Verteiler\n");

    /* Der Verteiler von QEMU: 8 Anschluesse, Einschaltzeit 50 * 2 ms. */
    const uint8_t qemu[] = { 9, 0x29, 8, 0x0A, 0x00, 50, 0, 0, 0xFF };
    struct usb_hub_info info;

    pruefe("QEMU-Verteiler gelesen", usb_parse_hub_descriptor(qemu, sizeof(qemu), &info));
    pruefe_zahl("Anschluesse", 8, info.ports);
    pruefe_zahl("Einschaltzeit 100 ms", 100, info.power_good_ms);

    const uint8_t schnell[] = { 9, 0x29, 4, 0, 0, 1, 0, 0, 0xFF };
    pruefe("gelesen", usb_parse_hub_descriptor(schnell, sizeof(schnell), &info));
    pruefe_zahl("zu kurze Zeit wird verlaengert", 20, info.power_good_ms);

    const uint8_t langsam[] = { 9, 0x29, 4, 0, 0, 255, 0, 0, 0xFF };
    pruefe("gelesen", usb_parse_hub_descriptor(langsam, sizeof(langsam), &info));
    pruefe_zahl("zu lange Zeit wird gekappt", 250, info.power_good_ms);

    const uint8_t leer[] = { 9, 0x29, 0, 0, 0, 50, 0, 0, 0xFF };
    const uint8_t viele[] = { 9, 0x29, 16, 0, 0, 50, 0, 0, 0xFF };
    const uint8_t falsch[] = { 9, 0x01, 4, 0, 0, 50, 0, 0, 0xFF };
    const uint8_t kurz[] = { 9, 0x29, 4 };
    const uint8_t winzig[] = { 5, 0x29, 4, 0, 0, 50, 0, 0, 0 };

    pruefe("ohne Anschluss", !usb_parse_hub_descriptor(leer, sizeof(leer), &info));
    pruefe("zu viele Anschluesse", !usb_parse_hub_descriptor(viele, sizeof(viele), &info));
    pruefe("falsche Art", !usb_parse_hub_descriptor(falsch, sizeof(falsch), &info));
    pruefe("zu kurz", !usb_parse_hub_descriptor(kurz, sizeof(kurz), &info));
    pruefe("Laengenfeld zu klein", !usb_parse_hub_descriptor(winzig, sizeof(winzig), &info));
    pruefe("ohne Beschreibung", !usb_parse_hub_descriptor(NULL, 0, &info));
    pruefe("ohne Ausgabe", !usb_parse_hub_descriptor(qemu, sizeof(qemu), NULL));
    pruefe_zahl("Ausgabe nach Fehler leer", 0, info.ports);

    /* Anschlusszustand: angeschlossen, freigegeben, Strom - Full Speed. */
    struct usb_hub_port p;
    const uint8_t voll[4] = { 0x03, 0x01, 0x11, 0x00 };

    usb_decode_hub_port(voll, &p);
    pruefe("verbunden", p.connected);
    pruefe("freigegeben", p.enabled);
    pruefe("Strom", p.powered);
    pruefe("nicht zuruecksetzend", !p.reset);
    pruefe_zahl("Full Speed", 1, usb_hub_port_speed(&p));
    pruefe("Verbindung geaendert", p.changed_connection);
    pruefe("Reset beendet", p.changed_reset);
    pruefe("Freigabe nicht geaendert", !p.changed_enable);

    const uint8_t langsam_p[4] = { 0x03, 0x03, 0, 0 };

    usb_decode_hub_port(langsam_p, &p);
    pruefe("langsam", p.low_speed);
    pruefe_zahl("Low Speed", 2, usb_hub_port_speed(&p));

    const uint8_t hoch[4] = { 0x03, 0x05, 0, 0 };

    usb_decode_hub_port(hoch, &p);
    pruefe("schnell", p.high_speed && !p.low_speed);
    pruefe_zahl("High Speed", 3, usb_hub_port_speed(&p));

    const uint8_t zurueck[4] = { 0x13, 0x01, 0x00, 0x00 };

    usb_decode_hub_port(zurueck, &p);
    pruefe("Reset laeuft", p.reset);
    pruefe("Aenderungsbits beim Reset noch leer", !p.changed_reset);

    const uint8_t strom[4] = { 0x08, 0x01, 0, 0 };

    usb_decode_hub_port(strom, &p);
    pruefe("Ueberstrom", p.overcurrent);
    pruefe("nicht verbunden", !p.connected);

    const uint8_t stillgelegt[4] = { 0x07, 0x01, 0, 0 };

    usb_decode_hub_port(stillgelegt, &p);
    pruefe("ruhend", p.suspended);

    /* Die Anfragen. */
    struct usb_setup s;

    usb_hub_get_descriptor_setup(&s);
    pruefe_zahl("Beschreibung: Typ", 0xA0, s.request_type);
    pruefe_zahl("Beschreibung: Anfrage", 6, s.request);
    pruefe_zahl("Beschreibung: Wert", 0x2900, s.value);
    pruefe_zahl("Beschreibung: Laenge", 9, s.length);

    usb_hub_port_feature_setup(&s, 3, HUB_FEAT_PORT_RESET, true);
    pruefe_zahl("Reset: Typ", 0x23, s.request_type);
    pruefe_zahl("Reset: SET_FEATURE", 3, s.request);
    pruefe_zahl("Reset: Merkmal", 4, s.value);
    pruefe_zahl("Reset: Anschluss", 3, s.index);
    pruefe_zahl("Reset: ohne Daten", 0, s.length);

    usb_hub_port_feature_setup(&s, 2, HUB_FEAT_C_RESET, false);
    pruefe_zahl("Loeschen: CLEAR_FEATURE", 1, s.request);
    pruefe_zahl("Loeschen: Merkmal 20", 20, s.value);

    usb_hub_port_status_setup(&s, 5);
    pruefe_zahl("Zustand: Typ", 0xA3, s.request_type);
    pruefe_zahl("Zustand: GET_STATUS", 0, s.request);
    pruefe_zahl("Zustand: Anschluss", 5, s.index);
    pruefe_zahl("Zustand: vier Byte", 4, s.length);
}

int main(void)
{
    printf("=== USB ===\n");

    test_tablett();
    test_tastatur();
    test_speicher();
    test_doppelt();
    test_ausgang();
    test_kaputt();
    test_zufall();
    test_endpunktwort();
    test_transferwort();
    test_puffer();
    test_rahmen();
    test_anschluss();
    test_verteiler();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
