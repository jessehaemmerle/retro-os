/* ohcihw.h - Register und Wortformate des OHCI-Controllers.
 *
 * OHCI (Open Host Controller Interface, 1999) ist der alte USB-1.1-
 * Controller: Die Software baut im Speicher Listen aus Endpunkt- und
 * Transferbeschreibungen, der Controller arbeitet sie ab. Jedes Wort
 * einer solchen Beschreibung hat feste Bitfelder - und wer sich um ein
 * Bit irrt, bekommt keine Fehlermeldung, sondern einen Controller, der
 * nichts tut oder woanders hinschreibt.
 *
 * Deshalb steht das Zusammensetzen der Worte hier, ohne Zugriff auf
 * Hardware, und wird auf dem Entwicklungsrechner geprueft. Der Treiber
 * (ohci.c) setzt sie nur noch ein.
 */
#ifndef OHCIHW_H
#define OHCIHW_H

#include "retro.h"

/* --- Register (Versatz ab BAR 0) --- */
#define OHCI_REVISION        0x00
#define OHCI_CONTROL         0x04
#define OHCI_CMDSTATUS       0x08
#define OHCI_INTSTATUS       0x0C
#define OHCI_INTENABLE       0x10
#define OHCI_INTDISABLE      0x14
#define OHCI_HCCA            0x18
#define OHCI_CONTROL_HEAD    0x20
#define OHCI_BULK_HEAD       0x28
#define OHCI_FMINTERVAL      0x34
#define OHCI_PERIODIC_START  0x40
#define OHCI_LS_THRESHOLD    0x44
#define OHCI_RH_DESCA        0x48
#define OHCI_RH_STATUS       0x50
#define OHCI_RH_PORT(n)      (0x54 + 4 * (n))

/* HcControl */
#define OHCI_CTRL_CBSR_4_1   0x03u          /* vier Steuerauftraege je Masse */
#define OHCI_CTRL_PLE        (1u << 2)      /* periodische Liste           */
#define OHCI_CTRL_CLE        (1u << 4)      /* Steuerliste                 */
#define OHCI_CTRL_HCFS_MASK  0xC0u
#define OHCI_CTRL_HCFS_OPER  0x80u
#define OHCI_CTRL_IR         (1u << 8)      /* Unterbrechungen gehen an SMM */

/* HcCommandStatus */
#define OHCI_CMD_HCR         (1u << 0)      /* Rechner zuruecksetzen */
#define OHCI_CMD_CLF         (1u << 1)      /* Steuerliste gefuellt  */
#define OHCI_CMD_OCR         (1u << 3)      /* Besitz uebernehmen    */

/* HcInterruptStatus */
#define OHCI_INT_WDH         (1u << 1)      /* Ergebnisliste geschrieben */
#define OHCI_INT_MIE         (1u << 31)

/* Wurzelverteiler */
#define OHCI_RHS_LPSC        (1u << 16)     /* alle Anschluesse einschalten */
#define OHCI_PORT_CCS        (1u << 0)      /* Geraet angeschlossen         */
#define OHCI_PORT_PES        (1u << 1)      /* Anschluss freigegeben        */
#define OHCI_PORT_PRS        (1u << 4)      /* zuruecksetzen                */
#define OHCI_PORT_PPS        (1u << 8)      /* Strom ein                    */
#define OHCI_PORT_LSDA       (1u << 9)      /* langsames Geraet             */
#define OHCI_PORT_CSC        (1u << 16)     /* Aenderung: Anschluss         */
#define OHCI_PORT_PRSC       (1u << 20)     /* Aenderung: Reset beendet     */

/* Richtung im Endpunkt und Art der Transferbeschreibung */
#define OHCI_DIR_FROM_TD     0
#define OHCI_DIR_OUT         1
#define OHCI_DIR_IN          2

#define OHCI_PID_SETUP       0
#define OHCI_PID_OUT         1
#define OHCI_PID_IN          2

/* Wo der Datenwechsel (DATA0/DATA1) herkommt */
#define OHCI_TOGGLE_CARRY    0              /* vom Endpunkt weitergefuehrt */
#define OHCI_TOGGLE_DATA0    2
#define OHCI_TOGGLE_DATA1    3

/* Das Ergebnis einer Transferbeschreibung. 14 und 15 heissen "noch nicht
 * angefasst" - der Controller hat das Ding nicht erreicht. */
#define OHCI_CC_OK           0
#define OHCI_CC_STALL        4
#define OHCI_CC_NOT_ACCESSED 0xF

/* Wort 0 eines Endpunkts: Adresse, Nummer, Richtung, Geschwindigkeit,
 * Ueberspringen und groesstes Paket. */
uint32_t ohci_ed_word0(uint8_t function, uint8_t endpoint, uint8_t direction,
                       bool low_speed, bool skip, uint16_t max_packet);

/* Wort 0 einer Transferbeschreibung. Das Ergebnisfeld steht auf "noch
 * nicht angefasst", die Fehlerzahl auf null, und es wird keine
 * Unterbrechung verlangt - wir fragen nach. round heisst: Ein kuerzeres
 * Paket als erwartet ist kein Fehler. */
uint32_t ohci_td_word0(uint8_t pid, uint8_t toggle, bool round);

uint8_t     ohci_td_cc(uint32_t word0);
bool        ohci_td_done(uint32_t word0);
const char *ohci_cc_name(uint8_t cc);

/* Letztes Byte des Puffers; bei Laenge null ist es 0, so verlangt es das
 * Format fuer eine leere Beschreibung. */
uint32_t ohci_buffer_end(uint32_t phys, uint32_t length);

/* Wie viele Bytes uebertragen wurden. cbp ist der Zeiger, den der
 * Controller nach der Uebertragung hinterlaesst: 0, wenn alles ging,
 * sonst die erste Stelle, die nicht mehr drankam. */
size_t ohci_transferred(uint32_t cbp, uint32_t start, uint32_t length);

/* Der Wert fuer HcFmInterval nach dem Zuruecksetzen. Die Rahmenlaenge
 * steht vorher im Register - ihr Wert gilt, sonst die Normallaenge von
 * 11999 Takten (1 ms). Der Umschaltbit-Wert muss wechseln, damit der
 * Controller die neue Laenge uebernimmt. */
uint32_t ohci_frame_interval(uint32_t saved);

/* Ab wann im Rahmen die periodische Liste abgearbeitet wird: neun
 * Zehntel. */
uint32_t ohci_periodic_start(uint32_t fm_interval);

/* Aus dem Statuswort eines Anschlusses. Die Geschwindigkeit zaehlt
 * wie beim xHCI: 1 Full Speed, 2 Low Speed. */
bool    ohci_port_connected(uint32_t status);
uint8_t ohci_port_speed(uint32_t status);

#endif /* OHCIHW_H */
