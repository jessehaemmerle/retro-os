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
#include "spinlock.h"
#include "thread.h"

/* Seitentabellen werden von allen Kernen geteilt. */
static struct spinlock vmm_lock = SPINLOCK_INIT("vmm");

static uint64_t kernel_pml4;
static bool     no_execute_available;

static inline uint64_t *table_at(uint64_t phys)
{
    return phys_to_virt(phys & PTE_ADDR_MASK);
}

static inline size_t index_of(uint64_t virt, int level)
{
    /* level 4 = PML4, 1 = PT */
    return (size_t)((virt >> (12 + 9 * (level - 1))) & 0x1FF);
}

/* Das oberste Bit eines Seiteneintrags sperrt die Ausfuehrung - aber
 * nur, wenn die CPU das Verfahren vorher freigeschaltet bekommt.
 * Andernfalls gilt Bit 63 als reserviert und jeder Zugriff endet in
 * einem Seitenfehler. */
static bool enable_no_execute(void)
{
    uint32_t eax, ebx, ecx, edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(0x80000001), "c"(0));
    if (!(edx & (1u << 20)))
        return false;

    uint32_t low, high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(0xC0000080));
    low |= 1u << 11;                       /* EFER.NXE */
    __asm__ volatile("wrmsr" :: "c"(0xC0000080), "a"(low), "d"(high));
    return true;
}

bool vmm_no_execute(void)
{
    return no_execute_available;
}

void vmm_init(void)
{
    __asm__ volatile("mov %%cr3, %0" : "=r"(kernel_pml4));
    kernel_pml4 &= PTE_ADDR_MASK;

    no_execute_available = enable_no_execute();

    kprintf("Adressraum  : Kernel-Tabelle bei %p%s\n", (void *)kernel_pml4,
            no_execute_available ? ", Ausfuehrsperre aktiv" : "");
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

/* Laeuft bis zur Blatttabelle hinunter, ohne etwas anzulegen. Liefert
 * NULL, wenn unterwegs eine Ebene fehlt. Die Sperre muss gehalten
 * werden. */
static uint64_t *leaf_table(uint64_t root, uint64_t virt)
{
    uint64_t *pdpt = walk(root, virt, 4, false, 0);

    if (!pdpt)
        return NULL;

    uint64_t *pd = walk((uint64_t)pdpt - g_hhdm_offset, virt, 3, false, 0);

    if (!pd)
        return NULL;

    return walk((uint64_t)pd - g_hhdm_offset, virt, 2, false, 0);
}

/* Haengt eine Ebene der unteren Haelfte in einen zweiten Adressraum.
 *
 * Zwischentabellen werden verdoppelt - sie gehoeren dem jeweiligen
 * Adressraum. Die Seiten ganz unten dagegen werden geteilt: Beide
 * Eintraege zeigen auf denselben Rahmen, das Schreibrecht faellt weg,
 * und dafuer wird PTE_COW gesetzt. Wer als Erster hineinschreibt,
 * bekommt seine eigene Kopie.
 *
 * Nur lesbare Seiten - Programmcode etwa - brauchen die Markierung
 * nicht: An ihnen aendert sich ohnehin nichts. */
static bool fork_level(uint64_t dst_phys, uint64_t src_phys, int level)
{
    uint64_t *src = table_at(src_phys);
    uint64_t *dst = table_at(dst_phys);
    size_t limit = (level == 4) ? 256 : 512;

    for (size_t i = 0; i < limit; i++) {
        uint64_t entry = src[i];

        if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE))
            continue;

        if (level == 1) {
            if (entry & PTE_WRITE) {
                entry = (entry & ~PTE_WRITE) | PTE_COW;
                src[i] = entry;
            }
            pmm_share_page(entry & PTE_ADDR_MASK);
            dst[i] = entry;
            continue;
        }

        uint64_t table = pmm_alloc_page();

        if (!table)
            return false;

        memset(phys_to_virt(table), 0, PAGE_SIZE);
        dst[i] = table | (entry & ~PTE_ADDR_MASK);

        if (!fork_level(table, entry & PTE_ADDR_MASK, level - 1))
            return false;
    }
    return true;
}

bool vmm_fork(struct address_space *child, struct address_space *parent)
{
    if (!vmm_create(child))
        return false;

    uint64_t flags = spin_lock_irq(&vmm_lock);
    bool ok = fork_level(child->pml4_phys, parent->pml4_phys, 4);

    spin_unlock_irq(&vmm_lock, flags);

    if (!ok) {
        vmm_destroy(child);
        return false;
    }

    child->heap_break = parent->heap_break;

    /* Dem Elternteil wurde gerade das Schreibrecht entzogen. Der
     * Zwischenspeicher der Adressuebersetzung weiss davon nichts -
     * also einmal neu laden. Der Adressraum laeuft nur auf diesem
     * Kern, ein Neuladen von CR3 raeumt ihn dort vollstaendig aus. */
    vmm_switch(parent);
    return true;
}

bool vmm_cow_fault(struct address_space *space, uint64_t virt)
{
    uint64_t root = space ? space->pml4_phys : kernel_pml4;
    uint64_t page = ALIGN_DOWN(virt, PAGE_SIZE);

    uint64_t flags = spin_lock_irq(&vmm_lock);
    uint64_t *pt = leaf_table(root, page);

    if (!pt) {
        spin_unlock_irq(&vmm_lock, flags);
        return false;
    }

    uint64_t *entry = &pt[index_of(page, 1)];

    if (!(*entry & PTE_PRESENT) || !(*entry & PTE_COW)) {
        spin_unlock_irq(&vmm_lock, flags);
        return false;
    }

    uint64_t old = *entry & PTE_ADDR_MASK;

    if (pmm_shared(old)) {
        uint64_t fresh = pmm_alloc_page();

        if (!fresh) {
            spin_unlock_irq(&vmm_lock, flags);
            return false;
        }

        memcpy(phys_to_virt(fresh), phys_to_virt(old), PAGE_SIZE);
        pmm_free_page(old);           /* zaehlt nur herunter */
        *entry = fresh | ((*entry & ~PTE_ADDR_MASK & ~PTE_COW) | PTE_WRITE);
    } else {
        /* Der letzte Besitzer braucht keine Kopie - er bekommt sein
         * Schreibrecht einfach zurueck. */
        *entry = (*entry & ~PTE_COW) | PTE_WRITE;
    }

    spin_unlock_irq(&vmm_lock, flags);
    __asm__ volatile("invlpg (%0)" :: "r"(page) : "memory");
    return true;
}

bool vmm_map(struct address_space *space, uint64_t virt, uint64_t phys,
             uint64_t flags)
{
    uint64_t root = space ? space->pml4_phys : kernel_pml4;

    /* Ohne freigeschaltete Ausfuehrsperre gilt das oberste Bit als
     * reserviert - es zu setzen wuerde jeden Zugriff scheitern lassen. */
    if (!no_execute_available)
        flags &= ~PTE_NX;

    uint64_t __flags = spin_lock_irq(&vmm_lock);

    /* Eine Zwischentabelle darf die Ausfuehrsperre nicht tragen - sie
     * wuerde sonst fuer alles darunter gelten. */
    uint64_t table_flags = flags & ~PTE_NX;

    uint64_t *pdpt = walk(root, virt, 4, true, table_flags);
    if (!pdpt) { spin_unlock_irq(&vmm_lock, __flags); return false; }

    uint64_t *pd = walk((uint64_t)pdpt - g_hhdm_offset, virt, 3, true,
                        table_flags);
    if (!pd) { spin_unlock_irq(&vmm_lock, __flags); return false; }

    uint64_t *pt = walk((uint64_t)pd - g_hhdm_offset, virt, 2, true,
                        table_flags);
    if (!pt) { spin_unlock_irq(&vmm_lock, __flags); return false; }

    pt[index_of(virt, 1)] = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    spin_unlock_irq(&vmm_lock, __flags);
    return true;
}

/* Setzt die Rechte eines schon abgebildeten Bereichs neu. Gebraucht
 * wird das beim Laden eines Programms: Erst wird das Segment
 * beschreibbar angelegt und gefuellt, danach bekommt es die Rechte, die
 * in der Programmdatei stehen. */
bool vmm_protect_range(struct address_space *space, uint64_t virt,
                       size_t bytes, uint64_t flags)
{
    uint64_t root = space ? space->pml4_phys : kernel_pml4;
    uint64_t start = ALIGN_DOWN(virt, PAGE_SIZE);
    uint64_t end = ALIGN_UP(virt + bytes, PAGE_SIZE);

    if (!vmm_no_execute())
        flags &= ~PTE_NX;

    uint64_t __flags = spin_lock_irq(&vmm_lock);

    for (uint64_t page = start; page < end; page += PAGE_SIZE) {
        uint64_t *pdpt = walk(root, page, 4, false, 0);

        if (!pdpt)
            continue;

        uint64_t *pd = walk((uint64_t)pdpt - g_hhdm_offset, page, 3, false, 0);

        if (!pd)
            continue;

        uint64_t *pt = walk((uint64_t)pd - g_hhdm_offset, page, 2, false, 0);

        if (!pt)
            continue;

        uint64_t *entry = &pt[index_of(page, 1)];

        if (!(*entry & PTE_PRESENT))
            continue;
        *entry = (*entry & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    }

    spin_unlock_irq(&vmm_lock, __flags);

    /* Der Adressraum des Prozesses laeuft noch nicht - ein Umschalten
     * auf ihn laedt die Tabellen ohnehin neu. */
    return true;
}

void vmm_unmap(struct address_space *space, uint64_t virt)
{
    uint64_t root = space ? space->pml4_phys : kernel_pml4;

    uint64_t __flags = spin_lock_irq(&vmm_lock);

    uint64_t *pdpt = walk(root, virt, 4, false, 0);
    if (pdpt) {
        uint64_t *pd = walk((uint64_t)pdpt - g_hhdm_offset, virt, 3, false, 0);
        if (pd) {
            uint64_t *pt = walk((uint64_t)pd - g_hhdm_offset, virt, 2, false, 0);
            if (pt)
                pt[index_of(virt, 1)] = 0;
        }
    }

    spin_unlock_irq(&vmm_lock, __flags);
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
        /* Der Kernel schreibt an der Seitentabelle vorbei - also muss
         * er selbst dafuer sorgen, dass eine geteilte Seite vorher
         * verdoppelt wird. Sonst saehe das Geschwisterprogramm, was
         * hier hineingeschrieben wird. */
        vmm_cow_fault(space, dst);

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
