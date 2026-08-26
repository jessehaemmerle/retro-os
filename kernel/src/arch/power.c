/* power.c - Neustart und Abschalten des Rechners.
 *
 * Ein sauberes Herunterfahren erfordert eigentlich einen ACPI-Interpreter.
 * RetroOS begnuegt sich mit den bekannten Kurzwegen, die Emulatoren und
 * viele echte Rechner verstehen - und haelt die Maschine sonst einfach an.
 */

#include "power.h"
#include "io.h"

NORETURN void power_reboot(void)
{
    cli();

    /* Ueber den Tastaturcontroller die CPU zuruecksetzen. */
    for (int i = 0; i < 10; i++) {
        while (inb(0x64) & 0x02)
            ;
        outb(0x64, 0xFE);
    }

    /* Falls das nicht wirkt: ungueltige IDT laden und einen Fehler ausloesen
     * - das erzwingt einen Triple Fault und damit den Neustart. */
    struct { uint16_t limit; uint64_t base; } PACKED null_idt = { 0, 0 };
    __asm__ volatile("lidt %0; int $0x03" :: "m"(null_idt));

    for (;;)
        hlt();
}

NORETURN void power_shutdown(void)
{
    cli();

    /* QEMU (neuere Versionen), Bochs/aeltere QEMU, VirtualBox. */
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);

    for (;;)
        hlt();
}
