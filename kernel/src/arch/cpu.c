/* cpu.c - die Kerne anmelden und starten. */

#include "cpu.h"
#include "apic.h"
#include "arch.h"
#include "boot.h"
#include "kstring.h"
#include "spinlock.h"
#include "thread.h"
#include "vmm.h"

static struct cpu cpus[CPU_MAX];
static uint32_t   count;
static struct spinlock table_lock = SPINLOCK_INIT("cpu-tabelle");

void cpu_init_bsp(void)
{
    memset(cpus, 0, sizeof(cpus));
    count = 0;
    cpu_register(apic_available() ? apic_id() : 0);
}

struct cpu *cpu_register(uint32_t id)
{
    uint64_t flags = spin_lock_irq(&table_lock);
    struct cpu *entry = NULL;

    for (uint32_t i = 0; i < count; i++) {
        if (cpus[i].apic_id == id) {
            entry = &cpus[i];
            break;
        }
    }

    if (!entry && count < CPU_MAX) {
        entry = &cpus[count];
        entry->apic_id = id;
        entry->index = count;
        entry->online = true;
        entry->slice_remaining = 0;
        count++;
    }

    spin_unlock_irq(&table_lock, flags);
    return entry;
}

struct cpu *cpu_current(void)
{
    uint32_t id = apic_available() ? apic_id() : 0;

    for (uint32_t i = 0; i < count; i++) {
        if (cpus[i].apic_id == id)
            return &cpus[i];
    }

    /* Vor dem Anmelden - das kann nur der Bootkern sein. */
    return &cpus[0];
}

uint32_t cpu_count(void) { return count; }

struct cpu *cpu_at(uint32_t index)
{
    return index < count ? &cpus[index] : NULL;
}
