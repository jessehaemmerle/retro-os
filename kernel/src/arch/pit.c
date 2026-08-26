/* pit.c - Programmable Interval Timer als Systemtakt.
 *
 * RetroOS laeuft mit 1000 Hz: fein genug fuer fluessige Mausbewegung und
 * einfache Zeitmessung, ohne die Maschine mit Interrupts zu ueberfluten.
 */

#include "arch.h"
#include "io.h"
#include "thread.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_HZ  1193182u

static volatile uint64_t ticks;
static uint32_t          hz = 1000;

static void pit_irq(struct registers *regs)
{
    UNUSED(regs);
    ticks++;
    scheduler_tick();
}

void pit_init(uint32_t frequency_hz)
{
    if (frequency_hz == 0)
        frequency_hz = 1000;

    hz = frequency_hz;

    uint32_t divisor = PIT_BASE_HZ / frequency_hz;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;

    outb(PIT_COMMAND, 0x36);                      /* Kanal 0, Modus 3 */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)(divisor >> 8));

    irq_install(0, pit_irq);
}

uint64_t timer_ticks(void)
{
    return ticks;
}

uint64_t timer_ms(void)
{
    return (ticks * 1000) / hz;
}

void timer_sleep(uint32_t ms)
{
    uint64_t target = timer_ms() + ms;

    while (timer_ms() < target)
        __asm__ volatile("hlt");
}
