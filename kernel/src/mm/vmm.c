/* vmm.c - Seitentabellen anlegen und pflegen.
 *
 * Der Long Mode kennt vier Ebenen: PML4, PDPT, PD und PT. Jede Ebene ist
 * eine Seite mit 512 Eintraegen zu je acht Byte; neun Bit der Adresse
 * waehlen den Eintrag der jeweiligen Ebene aus, die untersten zwölf Bit
 * bleiben als Versatz innerhalb der Seite.
 *
 *   47      39 38      30 29      21 20      12 11         0
 *  +----------+----------+----------+----------+------------+
 *  |   PML4   |   PDPT   |    PD    |    PT    |   Versatz  |
 *  +----------+----------+----------+----------+------------+
 */

#include "vmm.h"
#include "kstring.h"
#include "mm.h"
#include "thread.h"

static uint64_t kernel_pml4;

static inline uint64_t *table_at(uint64_t phys)
{
    return phys_to_virt(phys & PTE_ADDR_MASK);
}

static inline size_t index_of(uint64_t virt, int level)
{
    /* level 4 = PML4, 1 = PT */
    return (size_t)((virt >> (12 + 9 * (level - 1))) & 0x1FF);
}

void vmm_init(void)
{
    __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_pml4));
    kernel_pml4 &= PTE_ADDR_MASK;

    kprintf("Adressraum  : Kernel-Tabelle bei %p\n", (void *)kernel_pml4);
}

uint64_t vmm_kernel_pml4(void)
{
    return kernel_pml4;
}

/* Sucht (und legt bei Bedarf an) die naechsttiefere Tabelle. */
static uint64_t *walk(uint64_t table_phys, uint64_t virt, int level,
                      bool create, uint64_t flags)
{
    uint64_t *table = table_at(table_phys);
    size_t index = index_of(virt, level);
    uint64_t entry = table[index];

    if (!(entry & PTE_PRESENT)) {
        if (!create)
            return NULL;

        uint64_t page = pmm_alloc_page();
        if (!page)
            return NULL;

        memset(phys_to_virt(page), 0, PAGE_SIZE);
        table[index] = page | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
        return table_at(page);
    }

    /* Auf dem Weg nach unten muessen alle Ebenen fuer den Benutzer
     * freigegeben sein, sonst greift die Sperre der obersten Ebene. */
    if (flags & PTE_USER)
        table[index] |= PTE_USER;

    return table_at(entry);
}

bool vmm_create(struct address_space *space)
{
    uint64_t page = pmm_alloc_page();

    if (!page)
        return false;

    uint64_t *fresh = phys_to_virt(page);
    uint64_t *kernel = table_at(kernel_pml4);

    memset(fresh, 0, PAGE_SIZE);

    /* Obere Haelfte uebernehmen: Kernel und die direkte Abbildung. */
    for (size_t i = 256; i < 512; i++)
        fresh[i] = kernel[i];

    space->pml4_phys = page;
    space->heap_break = USER_HEAP_BASE;
    return true;
}

/* Gibt die Tabellen der unteren Haelfte samt der Seiten darin frei. */
static void free_level(uint64_t table_phys, int level)
{
    uint64_t *table = table_at(table_phys);
    size_t limit = (level == 4) ? 256 : 512;

    for (size_t i = 0; i < limit; i++) {
        uint64_t entry = table[i];

        if (!(entry & PTE_PRESENT))
            continue;
        if (entry & PTE_HUGE)
            continue;

        if (level > 1)
            free_level(entry & PTE_ADDR_MASK, level - 1);
        else
            pmm_free_page(entry & PTE_ADDR_MASK);
    }

    pmm_free_page(table_phys);
}

void vmm_destroy(struct address_space *space)
{
    if (!space->pml4_phys)
        return;

    free_level(space->pml4_phys, 4);
    space->pml4_phys = 0;
}

bool vmm_map(struct address_space *space, uint64_t virt, uint64_t phys,
             uint64_t flags)
{
    uint64_t root = space ? space->pml4_phys : kernel_pml4;

    preempt_disable();

    uint64_t *pdpt = walk(root, virt, 4, true, flags);
    if (!pdpt) { preempt_enable(); return false; }

    uint64_t *pd = walk((uint64_t)pdpt - g_hhdm_offset, virt, 3, true, flags);
    if (!pd) { preempt_enable(); return false; }

    uint64_t *pt = walk((uint64_t)pd - g_hhdm_offset, virt, 2, true, flags);
    if (!pt) { preempt_enable(); return false; }

    pt[index_of(virt, 1)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    preempt_enable();
    return true;
}

void vmm_unmap(struct address_space *space, uint64_t virt)
{
    uint64_t root = space ? space->pml4_phys : kernel_pml4;

    preempt_disable();

    uint64_t *pdpt = walk(root, virt, 4, false, 0);
    if (pdpt) {
        uint64_t *pd = walk((uint64_t)pdpt - g_hhdm_offset, virt, 3, false, 0);
        if (pd) {
            uint64_t *pt = walk((uint64_t)pd - g_hhdm_offset, virt, 2, false, 0);
            if (pt)
                pt[index_of(virt, 1)] = 0;
        }
    }

    preempt_enable();
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

bool vmm_alloc_range(struct address_space *space, uint64_t virt, size_t bytes,
                     uint64_t flags)
{
    uint64_t start = ALIGN_DOWN(virt, PAGE_SIZE);
    uint64_t end   = ALIGN_UP(virt + bytes, PAGE_SIZE);

    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        if (vmm_resolve(space, page))
            continue;              /* schon vorhanden */

        uint64_t phys = pmm_alloc_page();
        if (!phys)
            return false;

        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        if (!vmm_map(space, page, phys, flags))
            return false;
    }
    return true;
}

uint64_t vmm_resolve(struct address_space *space, uint64_t virt)
{
    uint64_t root = space ? space->pml4_phys : kernel_pml4;

    uint64_t *pdpt = walk(root, virt, 4, false, 0);
    if (!pdpt)
        return 0;

    uint64_t *pd = walk((uint64_t)pdpt - g_hhdm_offset, virt, 3, false, 0);
    if (!pd)
        return 0;

    uint64_t *pt = walk((uint64_t)pd - g_hhdm_offset, virt, 2, false, 0);
    if (!pt)
        return 0;

    uint64_t entry = pt[index_of(virt, 1)];
    if (!(entry & PTE_PRESENT))
        return 0;

    return (entry & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

void vmm_switch(const struct address_space *space)
{
    uint64_t root = space && space->pml4_phys ? space->pml4_phys : kernel_pml4;

    __asm__ volatile("mov %0, %%cr3" :: "r"(root) : "memory");
}

void vmm_switch_kernel(void)
{
    __asm__ volatile("mov %0, %%cr3" :: "r"(kernel_pml4) : "memory");
}

/* Kopiert seitenweise ueber die direkte Abbildung - so muss der Kernel
 * nicht in den Adressraum des Prozesses wechseln. */
bool vmm_copy_to_user(struct address_space *space, uint64_t dst,
                      const void *src, size_t length)
{
    const uint8_t *in = src;

    while (length > 0) {
        uint64_t phys = vmm_resolve(space, dst);

        if (!phys)
            return false;

        size_t room = PAGE_SIZE - (dst & (PAGE_SIZE - 1));
        size_t take = MIN(room, length);

        memcpy(phys_to_virt(phys), in, take);
        in += take;
        dst += take;
        length -= take;
    }
    return true;
}

bool vmm_copy_from_user(struct address_space *space, void *dst, uint64_t src,
                        size_t length)
{
    uint8_t *out = dst;

    while (length > 0) {
        uint64_t phys = vmm_resolve(space, src);

        if (!phys)
            return false;

        size_t room = PAGE_SIZE - (src & (PAGE_SIZE - 1));
        size_t take = MIN(room, length);

        memcpy(out, phys_to_virt(phys), take);
        out += take;
        src += take;
        length -= take;
    }
    return true;
}
