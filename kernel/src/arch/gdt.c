/* gdt.c - eigene Global Descriptor Table.
 *
 * Im Long Mode ist Segmentierung praktisch abgeschaltet; es werden nur noch
 * ein Code- und ein Datensegment fuer Ring 0 benoetigt. RetroOS laeuft
 * vollstaendig im Kernel-Modus, daher gibt es keine Ring-3-Deskriptoren.
 */

#include "arch.h"
#include "kstring.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} PACKED;

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} PACKED;

static struct gdt_entry gdt[3];
static struct gdt_ptr   gdtr;

static void set_entry(int i, uint8_t access, uint8_t granularity)
{
    gdt[i].limit_low   = 0xFFFF;
    gdt[i].base_low    = 0;
    gdt[i].base_mid    = 0;
    gdt[i].access      = access;
    gdt[i].granularity = granularity;
    gdt[i].base_high   = 0;
}

void gdt_init(void)
{
    memset(gdt, 0, sizeof(gdt));

    /* 0x08: Code, ausfuehrbar, Long-Mode-Flag (L) gesetzt. */
    set_entry(1, 0x9A, 0xA0 | 0x0F);
    /* 0x10: Daten, beschreibbar. */
    set_entry(2, 0x92, 0xC0 | 0x0F);

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)&gdt;

    __asm__ volatile(
        "lgdt %0\n"
        /* Codesegment per Far-Return neu laden. */
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :: "m"(gdtr) : "rax", "memory");
}
