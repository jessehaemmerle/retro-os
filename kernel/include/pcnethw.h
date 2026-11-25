/* pcnethw.h - Wortformate der AMD-PCnet-Karte.
 *
 * Die PCnet ist die Standardkarte, die VirtualBox einer VM fuer aeltere
 * Gastsysteme gibt ("PCnet-FAST III"), und eine der ersten
 * Netzwerkkarten fuer den PCI-Bus. Sie arbeitet mit einem
 * Initialisierungsblock und zwei Ringen aus Deskriptoren; Laengen
 * stehen als Zweierlogarithmus oder als negative Zahl darin - lauter
 * Stellen, an denen ein Vorzeichen oder eine Verschiebung um vier Bit
 * nicht auffaellt, sondern eine Karte ergibt, die stumm bleibt.
 *
 * Das Zusammensetzen steht darum hier, ohne Zugriff auf die Karte, und
 * wird auf dem Entwicklungsrechner geprueft.
 */
#ifndef PCNETHW_H
#define PCNETHW_H

#include "retro.h"

/* Register im Ein-/Ausgabebereich (32-Bit-Zugriff, "DWIO") */
#define PCNET_APROM     0x00      /* die ersten sechs Byte: MAC-Adresse */
#define PCNET_RDP       0x10      /* Datenregister der CSR              */
#define PCNET_RAP       0x14      /* Adressregister                     */
#define PCNET_RESET     0x18
#define PCNET_BDP       0x1C      /* Datenregister der BCR              */

/* Im 16-Bit-Zustand nach dem Einschalten liegt das Zuruecksetzen
 * woanders. */
#define PCNET_WIO_RESET 0x14

#define PCNET_CSR0_INIT (1u << 0)
#define PCNET_CSR0_STRT (1u << 1)
#define PCNET_CSR0_STOP (1u << 2)
#define PCNET_CSR0_TDMD (1u << 3)
#define PCNET_CSR0_IDON (1u << 8)

#define PCNET_BCR_MSRDA 0
#define PCNET_BCR_MC    2         /* Medienwahl: Bit 1 = automatisch    */
#define PCNET_BCR_BSBC  18        /* Bit 7 = 32-Bit-Zugriff            */
#define PCNET_BCR_SWS   20        /* Software-Stil                      */

#define PCNET_RING_LOG2   4       /* 16 Plaetze je Ring                 */
#define PCNET_BUF_BYTES   2048

/* Ein Deskriptor ist im 32-Bit-Stil 16 Byte lang. */
struct pcnet_desc {
    uint32_t base;            /* Adresse des Puffers                  */
    uint16_t length;          /* negative Pufferlaenge                */
    uint16_t status;          /* Zustand, Bit 15: gehoert der Karte   */
    uint32_t message;         /* Empfang: Laenge; Senden: Fehler      */
    uint32_t reserved;
} __attribute__((packed));

#define PCNET_DESC_OWN  0x8000u
#define PCNET_DESC_ERR  0x4000u
#define PCNET_DESC_STP  0x0200u
#define PCNET_DESC_ENP  0x0100u

/* Der Initialisierungsblock im 32-Bit-Stil: 28 Byte. */
#define PCNET_INIT_BYTES 28

/* Die Laenge eines Puffers, wie sie im Deskriptor stehen muss: als
 * negative Zahl in zwoelf Bit, die obersten vier Bit sind Einsen.
 * Mehr als 4095 Byte lassen sich nicht ausdruecken - dann kommt 0. */
uint16_t pcnet_buffer_length(uint32_t bytes);

/* Zweierlogarithmus einer Ringgroesse; nur Zweierpotenzen von 1 bis
 * 512 sind erlaubt, sonst -1. */
int pcnet_ring_log2(uint32_t count);

/* Baut den Initialisierungsblock. Modus 0: eigene Adresse und
 * Rundsendungen annehmen. Der Filter fuer Mehrfachadressen laesst
 * alles durch. */
void pcnet_build_init(uint8_t out[PCNET_INIT_BYTES], const uint8_t mac[6],
                      int rx_log2, int tx_log2,
                      uint32_t rx_ring_phys, uint32_t tx_ring_phys);

/* Ein Deskriptor am Empfang: fertig und brauchbar? Brauchbar heisst:
 * Anfang und Ende desselben Rahmens, kein Fehler. */
bool pcnet_rx_ready(uint16_t status);
bool pcnet_rx_usable(uint16_t status);

/* Die Nutzlaenge aus dem Meldewort: zwoelf Bit, die vier Pruefbytes am
 * Ende abgezogen. Kuerzer als vier Byte ist unbrauchbar (0). */
uint32_t pcnet_rx_payload(uint32_t message);

#endif /* PCNETHW_H */
