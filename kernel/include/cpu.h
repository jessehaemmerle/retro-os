/* cpu.h - was jeder Kern fuer sich hat.
 *
 * Mit mehreren Kernen darf "der laufende Thread" keine einzelne
 * Variable mehr sein - jeder Kern hat seinen eigenen. Dasselbe gilt
 * fuer den Leerlauffaden, die Restzeit der Zeitscheibe und die
 * Schachtelungstiefe der geschuetzten Abschnitte.
 *
 * Welcher Kern gerade laeuft, sagt die Nummer im lokalen APIC. Das ist
 * ein Registerzugriff und damit billig genug, um ihn jedes Mal zu
 * machen - der Umweg ueber GS und swapgs waere schneller, aber auch
 * eine Fehlerquelle mehr.
 */
#ifndef CPU_H
#define CPU_H

#include "retro.h"

#define CPU_MAX 16

struct thread;

struct cpu {
    bool     online;
    uint32_t apic_id;
    uint32_t index;

    struct thread *current;
    struct thread *idle;
    struct thread *departed;  /* eben verlassen, Stapel noch zu loesen */

    int32_t  slice_remaining;
    uint32_t preempt_depth;
    bool     resched_wanted;

    uint64_t ticks;           /* wie oft dieser Kern den Zeitgeber sah */
    uint64_t switches;        /* wie oft er den Thread gewechselt hat  */
};

/* Traegt den Bootkern ein; die uebrigen melden sich selbst. */
void cpu_init_bsp(void);

/* Meldet den aufrufenden Kern an und gibt seinen Eintrag zurueck. */
struct cpu *cpu_register(uint32_t apic_id);

/* Der Eintrag des Kerns, auf dem dieser Aufruf laeuft. */
struct cpu *cpu_current(void);

uint32_t    cpu_count(void);
struct cpu *cpu_at(uint32_t index);

/* Startet die uebrigen Kerne. Gibt zurueck, wie viele nun laufen. */
uint32_t smp_start(void);

#endif /* CPU_H */
