/* boot.h - was der Kernel vom Bootloader uebernommen hat. */
#ifndef BOOT_H
#define BOOT_H

#include "retro.h"

#define BOOT_MAX_MEMMAP 64

enum boot_mem_type {
    BOOT_MEM_USABLE,
    BOOT_MEM_RESERVED,
    BOOT_MEM_RECLAIMABLE,
};

struct boot_mem_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
};

struct boot_info {
    uint64_t fb_addr;
    uint64_t fb_width;
    uint64_t fb_height;
    uint64_t fb_pitch;   /* in Bytes */
    uint32_t fb_bpp;

    uint64_t hhdm_offset;
    uint64_t kernel_phys;
    uint64_t kernel_virt;

    uint32_t memmap_count;
    struct boot_mem_entry memmap[BOOT_MAX_MEMMAP];

    uint64_t rsdp;          /* Einstieg in die ACPI-Tabellen */

    uint64_t total_memory;
    uint64_t usable_memory;
};

/* Liest die Bootloader-Antworten aus; einmalig zu Beginn von kmain(). */
void boot_collect(void);

const struct boot_info *boot_info(void);

/* Prueft, ob der Bootloader das erwartete Protokoll spricht. */
bool boot_revision_ok(void);

#endif /* BOOT_H */
