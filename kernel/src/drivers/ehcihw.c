/* ehcihw.c - siehe ehcihw.h. */

#include "ehcihw.h"

uint8_t ehci_speed_code(uint8_t usb_speed)
{
    switch (usb_speed) {
    case 2:  return 1;      /* Low  */
    case 3:  return 2;      /* High */
    default: return 0;      /* Full */
    }
}

uint32_t ehci_qh_chars(uint8_t address, uint8_t endpoint, uint8_t usb_speed,
                       bool dtc, bool head, uint16_t max_packet,
                       bool control_slow)
{
    return ((uint32_t)address & 0x7Fu) |
           (((uint32_t)endpoint & 0x0Fu) << 8) |
           ((uint32_t)ehci_speed_code(usb_speed) << 12) |
           ((dtc ? 1u : 0u) << 14) |
           ((head ? 1u : 0u) << 15) |
           (((uint32_t)max_packet & 0x7FFu) << 16) |
           ((control_slow ? 1u : 0u) << 27);
}

uint32_t ehci_qh_caps(uint8_t smask, uint8_t cmask, uint8_t hub_address,
                      uint8_t hub_port, uint8_t mult)
{
    return (uint32_t)smask |
           ((uint32_t)cmask << 8) |
           (((uint32_t)hub_address & 0x7Fu) << 16) |
           (((uint32_t)hub_port & 0x7Fu) << 23) |
           (((uint32_t)mult & 3u) << 30);
}

uint32_t ehci_qtd_token(uint8_t pid, uint32_t bytes, bool toggle, bool ioc)
{
    return EHCI_TOKEN_ACTIVE |
           (((uint32_t)pid & 3u) << 8) |
           (3u << 10) |                              /* Fehlversuche   */
           ((ioc ? 1u : 0u) << 15) |
           ((bytes & 0x7FFFu) << 16) |
           ((toggle ? 1u : 0u) << 31);
}

bool ehci_token_active(uint32_t token)
{
    return (token & EHCI_TOKEN_ACTIVE) != 0;
}

bool ehci_token_failed(uint32_t token)
{
    return (token & EHCI_TOKEN_ERRORS) != 0;
}

uint32_t ehci_token_remaining(uint32_t token)
{
    return (token >> 16) & 0x7FFFu;
}

const char *ehci_token_error(uint32_t token)
{
    if (token & EHCI_TOKEN_DBERR)  return "Datenpuffer";
    if (token & EHCI_TOKEN_BABBLE) return "zu viele Daten";
    if (token & EHCI_TOKEN_XACT)   return "Uebertragungsfehler";
    if (token & EHCI_TOKEN_HALTED) return "Endpunkt blockiert";
    return "kein Fehler";
}

size_t ehci_fill_buffers(uint32_t buf[EHCI_QTD_BUFFERS], uint32_t phys,
                         uint32_t length)
{
    for (int i = 0; i < EHCI_QTD_BUFFERS; i++)
        buf[i] = 0;

    if (length == 0) {
        buf[0] = phys;
        return 1;
    }

    uint32_t offset = phys & 0xFFFu;
    uint32_t pages = (offset + length + 4095u) / 4096u;

    if (pages > EHCI_QTD_BUFFERS)
        return 0;

    buf[0] = phys;                       /* mit dem Versatz in der Seite */
    for (uint32_t i = 1; i < pages; i++)
        buf[i] = (phys & ~0xFFFu) + i * 4096u;
    return pages;
}

uint32_t ehci_transferred(uint32_t requested, uint32_t token)
{
    uint32_t left = ehci_token_remaining(token);

    return left > requested ? 0 : requested - left;
}
