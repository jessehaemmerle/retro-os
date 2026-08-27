/* smp.c - die uebrigen Kerne in Gang setzen.
 *
 * Das muehsame Stueck - Trampolin im unteren Speicher, INIT, zwei
 * Startbefehle, Umschalten von Real Mode ueber Protected Mode in den
 * Long Mode - nimmt uns der Bootloader ab. Limine haelt die Kerne
 * angehalten bereit und springt sie dorthin, wohin wir zeigen; sie
 * kommen fertig eingerichtet an, mit unseren Seitentabellen.
 *
 * Hier bleibt: sich anmelden, die eigenen CPU-Strukturen laden, den
 * Zeitgeber starten und in den Scheduler gehen.
 */

#include "cpu.h"
#include "apic.h"
#include "arch.h"
#include "io.h"
#include "kstring.h"
#include "limine.h"
#include "thread.h"
#include "vmm.h"

__attribute__((used, section(".limine_requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0,
};

static volatile uint32_t ready;

/* Hier kommt ein zusaetzlicher Kern an. Ab dieser Zeile laeuft er
 * neben dem Bootkern - alles, was er anfasst, muss gesperrt sein. */
static void ap_entry(struct limine_smp_info *info)
{
    cli();

    /* Der Bootloader gibt jedem Kern eigene Tabellen mit; wir wollen
     * unsere. */
    vmm_switch_kernel();

    gdt_init_ap();
    idt_load();

    struct cpu *self = cpu_register((uint32_t)info->lapic_id);

    if (!self) {
        for (;;)
            hlt();
    }

    apic_init_ap();

    __atomic_fetch_add(&ready, 1, __ATOMIC_SEQ_CST);

    /* Der Leerlauffaden dieses Kerns; von hier aus uebernimmt der
     * Scheduler. */
    thread_enter_ap();
}

uint32_t smp_start(void)
{
    struct limine_smp_response *response = smp_request.response;

    if (!response || response->cpu_count <= 1)
        return 1;

    uint32_t started = 1;
    uint32_t wanted = 0;

    for (uint64_t i = 0; i < response->cpu_count; i++) {
        struct limine_smp_info *info = response->cpus[i];

        if (!info || info->lapic_id == response->bsp_lapic_id)
            continue;
        if (cpu_count() + wanted >= CPU_MAX)
            break;

        wanted++;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_SEQ_CST);
    }

    /* Kurz warten, bis sie sich gemeldet haben - laenger als eine
     * Zehntelsekunde sollte kein Kern brauchen. */
    for (int guard = 0; guard < 200 && ready < wanted; guard++)
        timer_wait_ms(1);

    started += ready;
    return started;
}
