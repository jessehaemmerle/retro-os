/* gdt.c - Global Descriptor Table und Task State Segment.
 *
 * Im Long Mode ist Segmentierung fast abgeschaltet, aber ein paar
 * Deskriptoren braucht es weiterhin: je einen fuer Code und Daten in
 * Ring 0 und in Ring 3, dazu das TSS. Aus dem TSS holt sich die CPU den
 * Kernel-Stapel, wenn ein Interrupt aus einem Benutzerprogramm kommt.
 *
 * Die Reihenfolge ist nicht frei waehlbar: der Befehl "sysret" leitet
 * die Segmente des Benutzers aus einem Wert im STAR-Register ab und
 * erwartet Daten vor Code.
 *
 *   0x00 leer     0x08 Kernel-Code   0x10 Kernel-Daten
 *   0x18 Benutzer-Daten              0x20 Benutzer-Code
 *   0x28 TSS (belegt zwei Eintraege)
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

/* Ein TSS-Deskriptor ist doppelt so lang wie ein normaler. */
struct tss_descriptor {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} PACKED;

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} PACKED;

static struct gdt_entry gdt[7];
static struct gdt_ptr   gdtr;
static struct tss       tss ALIGNED(16);

static void set_entry(int i, uint8_t access, uint8_t granularity)
{
    gdt[i].limit_low   = 0xFFFF;
    gdt[i].base_low    = 0;
    gdt[i].base_mid    = 0;
    gdt[i].access      = access;
    gdt[i].granularity = granularity;
    gdt[i].base_high   = 0;
}

void tss_set_kernel_stack(uint64_t top)
{
    tss.rsp0 = top;
}

void gdt_init(void)
{
    memset(gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));

    /* 0x08: Code Ring 0, ausfuehrbar, Long-Mode-Flag (L) gesetzt. */
    set_entry(1, 0x9A, 0xA0 | 0x0F);
    /* 0x10: Daten Ring 0, beschreibbar. */
    set_entry(2, 0x92, 0xC0 | 0x0F);
    /* 0x18: Daten Ring 3. */
    set_entry(3, 0xF2, 0xC0 | 0x0F);
    /* 0x20: Code Ring 3. */
    set_entry(4, 0xFA, 0xA0 | 0x0F);

    /* 0x28: das TSS, ueber zwei Eintraege verteilt. */
    struct tss_descriptor *desc = (struct tss_descriptor *)&gdt[5];
    uint64_t base = (uint64_t)&tss;

    desc->limit_low   = sizeof(tss) - 1;
    desc->base_low    = (uint16_t)(base & 0xFFFF);
    desc->base_mid    = (uint8_t)((base >> 16) & 0xFF);
    desc->access      = 0x89;          /* vorhanden, 64-Bit-TSS, frei */
    desc->granularity = 0x00;
    desc->base_high   = (uint8_t)((base >> 24) & 0xFF);
    desc->base_upper  = (uint32_t)(base >> 32);
    desc->reserved    = 0;

    tss.iomap_base = sizeof(tss);

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

    /* Das TSS in die Task-Register laden. */
    __asm__ volatile("ltr %0" :: "r"((uint16_t)0x28));
}
