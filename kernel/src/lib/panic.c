/* panic.c - kontrollierter Notausstieg bei nicht behebbaren Fehlern. */

#include "retro.h"
#include "io.h"
#include "serial.h"

#include <stdarg.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* Mit mehreren Kernen faellt oft mehr als einer zugleich um - meist,
 * weil sie am selben Schaden haengen. Nur der Erste darf berichten;
 * die anderen halten still, sonst schieben sich die Zeilen ineinander
 * und die Meldung ist nicht mehr lesbar. */
static volatile uint32_t panicking;

NORETURN void panic(const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    cli();

    if (__atomic_fetch_add(&panicking, 1, __ATOMIC_ACQ_REL) != 0) {
        for (;;)
            hlt();
    }

    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serial_puts("\n*** KERNEL PANIC ***\n");
    serial_puts(msg);
    serial_puts("\nSystem angehalten.\n");

    for (;;)
        hlt();
}
