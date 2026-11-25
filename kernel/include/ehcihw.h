/* ehcihw.h - Register und Wortformate des EHCI-Controllers (USB 2.0).
 *
 * EHCI arbeitet wie OHCI mit Listen im Speicher, nur dichter gepackt:
 * Die Software baut Warteschlangenkoepfe (QH, je Endpunkt einer) und
 * daran Ketten von Transferbeschreibungen (qTD). Anders als bei OHCI
 * traegt der Kopf selbst einen Arbeitsbereich ("Overlay"), in den der
 * Controller die gerade laufende qTD hineinkopiert - wer ihn anfasst,
 * muss wissen, wann er still liegt.
 *
 * Fast jedes Wort hat Bitfelder, die sich um ein Bit irren lassen, ohne
 * dass etwas auffaellt: ein Endpunkt, der als langsam gilt, ein Datenwechsel,
 * der von der falschen Stelle kommt, ein Pufferzeiger, der die Seite
 * verfehlt. Das Zusammensetzen steht darum hier, ohne Zugriff auf
 * Hardware, und wird auf dem Entwicklungsrechner geprueft.
 *
 * EHCI spricht nur High Speed. Alles Langsamere (Tastatur, Maus der alten
 * Art) uebergibt der Controller an den "Begleiter" - meist einen OHCI
 * oder UHCI im selben Chipsatz. Dieser Treiber gibt solche Anschluesse
 * weiter und kuemmert sich nur um das, was bleibt.
 */
#ifndef EHCIHW_H
#define EHCIHW_H

#include "retro.h"

/* --- Register: Faehigkeiten (ab BAR 0) --- */
#define EHCI_CAPLENGTH   0x00     /* 8 Bit: wo der Betriebsbereich anfaengt */
#define EHCI_HCSPARAMS   0x04
#define EHCI_HCCPARAMS   0x08

/* --- Register: Betrieb (ab CAPLENGTH) --- */
#define EHCI_USBCMD      0x00
#define EHCI_USBSTS      0x04
#define EHCI_USBINTR     0x08
#define EHCI_FRINDEX     0x0C
#define EHCI_CTRLDSSEG   0x10
#define EHCI_PERIODICBASE 0x14
#define EHCI_ASYNCLIST   0x18
#define EHCI_CONFIGFLAG  0x40
#define EHCI_PORTSC(n)   (0x44 + 4 * (n))

#define EHCI_CMD_RUN     (1u << 0)
#define EHCI_CMD_RESET   (1u << 1)
#define EHCI_CMD_PSE     (1u << 4)       /* periodische Liste an */
#define EHCI_CMD_ASE     (1u << 5)       /* asynchrone Liste an  */
#define EHCI_CMD_ITC8    (8u << 16)      /* Unterbrechungsschwelle: 1 ms */

#define EHCI_STS_HALTED  (1u << 12)
#define EHCI_STS_PSS     (1u << 14)
#define EHCI_STS_ASS     (1u << 15)

/* PORTSC. Die drei Aenderungsbits werden durch Schreiben einer Eins
 * geloescht - wer das Register zurueckschreibt, ohne sie auszublenden,
 * quittiert Ereignisse, die er nie gesehen hat. */
#define EHCI_PORT_CCS    (1u << 0)
#define EHCI_PORT_CSC    (1u << 1)
#define EHCI_PORT_PED    (1u << 2)
#define EHCI_PORT_PEC    (1u << 3)
#define EHCI_PORT_OCC    (1u << 5)
#define EHCI_PORT_RESET  (1u << 8)
#define EHCI_PORT_LS     (3u << 10)      /* Leitungszustand */
#define EHCI_PORT_POWER  (1u << 12)
#define EHCI_PORT_OWNER  (1u << 13)      /* gehoert dem Begleiter */
#define EHCI_PORT_CHANGES (EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC)

/* Leitungszustand "K": ein Geraet mit niedriger Geschwindigkeit. */
#define EHCI_LINE_K      1u

/* --- Verknuepfungen --- */
#define EHCI_LINK_END    1u              /* Bit 0: Ende der Liste        */
#define EHCI_LINK_QH     (1u << 1)       /* Art: Warteschlangenkopf      */

/* --- Transferbeschreibung (qTD) --- */
#define EHCI_PID_OUT     0
#define EHCI_PID_IN      1
#define EHCI_PID_SETUP   2

#define EHCI_TOKEN_ACTIVE  (1u << 7)
#define EHCI_TOKEN_HALTED  (1u << 6)
#define EHCI_TOKEN_DBERR   (1u << 5)     /* Datenpuffer */
#define EHCI_TOKEN_BABBLE  (1u << 4)
#define EHCI_TOKEN_XACT    (1u << 3)     /* Uebertragungsfehler */
#define EHCI_TOKEN_MMF     (1u << 2)
#define EHCI_TOKEN_ERRORS  (EHCI_TOKEN_HALTED | EHCI_TOKEN_DBERR | \
                            EHCI_TOKEN_BABBLE | EHCI_TOKEN_XACT)
#define EHCI_TOKEN_TOGGLE  (1u << 31)

/* Eine qTD hat fuenf Seitenzeiger: 20 KiB bei Seitenanfang, 16 KiB
 * sicher bei jeder Ausrichtung. */
#define EHCI_QTD_BUFFERS 5
#define EHCI_QTD_MAX     (4 * 4096)

struct ehci_qtd {
    volatile uint32_t next;
    volatile uint32_t alt;
    volatile uint32_t token;
    volatile uint32_t buf[EHCI_QTD_BUFFERS];
    volatile uint32_t buf_hi[EHCI_QTD_BUFFERS];
    uint32_t          pad[3];
} __attribute__((aligned(32)));

/* --- Warteschlangenkopf (QH) --- */
struct ehci_qh {
    volatile uint32_t horiz;           /* naechster Kopf               */
    volatile uint32_t chars;           /* Endpunkt-Eigenschaften       */
    volatile uint32_t caps;            /* Endpunkt-Faehigkeiten        */
    volatile uint32_t current;         /* aktuelle qTD (Controller)    */
    /* Arbeitsbereich, die Gestalt einer qTD */
    volatile uint32_t ov_next;
    volatile uint32_t ov_alt;
    volatile uint32_t ov_token;
    volatile uint32_t ov_buf[EHCI_QTD_BUFFERS];
    volatile uint32_t ov_buf_hi[EHCI_QTD_BUFFERS];
    uint32_t          pad[11];
} __attribute__((aligned(32)));

/* Geschwindigkeit im Sinne von usb_speed_name (1 Full, 2 Low, 3 High) in
 * die Kodierung des Kopfes: 0 Full, 1 Low, 2 High. */
uint8_t ehci_speed_code(uint8_t usb_speed);

/* Wort 1 des Kopfes. dtc: Datenwechsel kommt aus der qTD (Steuerkanal),
 * sonst bleibt er im Kopf. head markiert den Kopf der Liste. control_slow:
 * ein Steuerendpunkt eines Geraets, das nicht High Speed spricht. */
uint32_t ehci_qh_chars(uint8_t address, uint8_t endpoint, uint8_t usb_speed,
                       bool dtc, bool head, uint16_t max_packet,
                       bool control_slow);

/* Wort 2: Teilnehmer im Mikrorahmen, Verteileradresse und -anschluss,
 * Vielfachheit. */
uint32_t ehci_qh_caps(uint8_t smask, uint8_t cmask, uint8_t hub_address,
                      uint8_t hub_port, uint8_t mult);

/* Das Statuswort einer qTD: aktiv, drei Fehlversuche, Seite 0, Laenge. */
uint32_t ehci_qtd_token(uint8_t pid, uint32_t bytes, bool toggle, bool ioc);

bool     ehci_token_active(uint32_t token);
bool     ehci_token_failed(uint32_t token);
/* Wie viele Byte der Auftrag noch uebrig hatte, als er endete. */
uint32_t ehci_token_remaining(uint32_t token);
const char *ehci_token_error(uint32_t token);

/* Fuellt die fuenf Seitenzeiger fuer einen Puffer. Gibt zurueck, wie
 * viele benutzt werden, oder 0, wenn er nicht hineinpasst. */
size_t ehci_fill_buffers(uint32_t buf[EHCI_QTD_BUFFERS], uint32_t phys,
                         uint32_t length);

/* Wie viele Byte eine Antwort lang war: bestellte minus uebrige. */
uint32_t ehci_transferred(uint32_t requested, uint32_t token);

#endif /* EHCIHW_H */
