/* ohcihw.c - siehe ohcihw.h. */

#include "ohcihw.h"

#define DEFAULT_FRAME_INTERVAL 11999u     /* 12 MHz * 1 ms - 1 */
#define MAX_OVERHEAD           210u

uint32_t ohci_ed_word0(uint8_t function, uint8_t endpoint, uint8_t direction,
                       bool low_speed, bool skip, uint16_t max_packet)
{
    return ((uint32_t)function & 0x7Fu) |
           (((uint32_t)endpoint & 0x0Fu) << 7) |
           (((uint32_t)direction & 0x03u) << 11) |
           ((low_speed ? 1u : 0u) << 13) |
           ((skip ? 1u : 0u) << 14) |
           /* Bit 15 (Format) bleibt null: gewoehnliche Beschreibungen */
           (((uint32_t)max_packet & 0x7FFu) << 16);
}

uint32_t ohci_td_word0(uint8_t pid, uint8_t toggle, bool round)
{
    return ((round ? 1u : 0u) << 18) |
           (((uint32_t)pid & 0x03u) << 19) |
           (7u << 21) |                              /* keine Unterbrechung */
           (((uint32_t)toggle & 0x03u) << 24) |
           (0u << 26) |                              /* Fehlerzahl         */
           ((uint32_t)OHCI_CC_NOT_ACCESSED << 28);
}

uint8_t ohci_td_cc(uint32_t word0)
{
    return (uint8_t)(word0 >> 28);
}

bool ohci_td_done(uint32_t word0)
{
    /* 14 und 15 sind beide "nicht angefasst". */
    return ohci_td_cc(word0) < 0xE;
}

const char *ohci_cc_name(uint8_t cc)
{
    switch (cc) {
    case 0:  return "kein Fehler";
    case 1:  return "Pruefsumme falsch";
    case 2:  return "Bitstopfen";
    case 3:  return "Datenwechsel falsch";
    case 4:  return "Endpunkt blockiert";
    case 5:  return "Geraet antwortet nicht";
    case 6:  return "Kennung falsch";
    case 7:  return "Kennung unerwartet";
    case 8:  return "zu viele Daten";
    case 9:  return "zu wenig Daten";
    case 12: return "Puffer uebergelaufen";
    case 13: return "Puffer leergelaufen";
    case 14:
    case 15: return "nicht erreicht";
    default: return "unbekannt";
    }
}

uint32_t ohci_buffer_end(uint32_t phys, uint32_t length)
{
    if (length == 0)
        return 0;
    return phys + length - 1;
}

size_t ohci_transferred(uint32_t cbp, uint32_t start, uint32_t length)
{
    if (cbp == 0)
        return length;
    if (cbp < start || cbp - start > length)
        return 0;
    return cbp - start;
}

uint32_t ohci_frame_interval(uint32_t saved)
{
    uint32_t fi = saved & 0x3FFFu;

    /* Eine Rahmenlaenge, die nicht einmal den Verwaltungsaufwand
     * fasst, ist keine - gerechnet wuerde sonst mit einer Zahl, die
     * unter null liegt. */
    if (fi <= MAX_OVERHEAD)
        fi = DEFAULT_FRAME_INTERVAL;

    uint32_t largest = ((fi - MAX_OVERHEAD) * 6u) / 7u;
    uint32_t toggle = (~saved) & (1u << 31);

    return toggle | (largest << 16) | fi;
}

uint32_t ohci_periodic_start(uint32_t fm_interval)
{
    uint32_t fi = fm_interval & 0x3FFFu;

    return (fi * 9u) / 10u;
}

bool ohci_port_connected(uint32_t status)
{
    return (status & OHCI_PORT_CCS) != 0;
}

uint8_t ohci_port_speed(uint32_t status)
{
    return (status & OHCI_PORT_LSDA) ? 2 : 1;
}
