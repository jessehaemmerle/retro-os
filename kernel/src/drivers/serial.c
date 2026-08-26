/* serial.c - 16550-UART auf COM1, ausschliesslich fuer Debug-Ausgaben.
 *
 * Der Bildschirm gehoert der grafischen Oberflaeche; alles, was der Kernel
 * ueber sich selbst zu sagen hat, landet daher auf der seriellen Schnittstelle
 * (in QEMU sichtbar via "-serial stdio").
 */

#include "serial.h"
#include "io.h"

#define COM1 0x3F8

static bool ready;

void serial_init(void)
{
    outb(COM1 + 1, 0x00); /* Interrupts aus                */
    outb(COM1 + 3, 0x80); /* DLAB setzen                   */
    outb(COM1 + 0, 0x01); /* Teiler 1 => 115200 Baud       */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8N1                           */
    outb(COM1 + 2, 0xC7); /* FIFO an, 14-Byte-Schwelle     */
    outb(COM1 + 4, 0x0B); /* RTS/DSR gesetzt               */

    ready = true;
}

static void wait_tx(void)
{
    /* Warten, bis der Sendepuffer leer ist - mit Notausstieg, damit ein
     * fehlender UART den Kernel nicht blockiert. */
    for (int i = 0; i < 100000; i++) {
        if (inb(COM1 + 5) & 0x20)
            return;
    }
}

void serial_putc(char c)
{
    if (!ready)
        return;

    if (c == '\n') {
        wait_tx();
        outb(COM1, '\r');
    }
    wait_tx();
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}
