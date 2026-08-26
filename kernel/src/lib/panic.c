/* panic.c - kontrollierter Notausstieg bei nicht behebbaren Fehlern. */

#include "retro.h"
#include "io.h"
#include "serial.h"

#include <stdarg.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

NORETURN void panic(const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    cli();

    va_start(ap, fmt);
    kvsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    serial_puts("\n*** KERNEL PANIC ***\n");
    serial_puts(msg);
    serial_puts("\nSystem angehalten.\n");

    for (;;)
        hlt();
}
