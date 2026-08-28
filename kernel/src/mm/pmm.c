/* pmm.c - Verwaltung der physischen Seitenrahmen.
 *
 * Eine Bitmap mit einem Bit je 4-KiB-Seite. Das ist der einfachste
 * Allokator, der noch vernuenftig arbeitet: konstanter Speicherbedarf
 * (32 KiB pro GiB RAM) und keine Fragmentierungsprobleme.
 *
 * Daneben steht ein Zaehler je Seite. Er wird gebraucht, seit ein
 * Prozess sich abspalten kann: Eltern und Kind teilen sich dann
 * dieselben Seiten, bis eine Seite beschrieben wird. Erst wenn der
 * letzte Besitzer sie loslaesst, wird sie wirklich frei.
 */

#include "mm.h"
#include "spinlock.h"
#include "boot.h"
#include "kstring.h"
#include "thread.h"

static uint8_t  *bitmap;
static uint64_t  bitmap_pages;    /* Anzahl verwalteter Seiten */
static uint64_t  highest_addr;
static uint64_t  used_pages;
static uint64_t  usable_pages;
static uint64_t  last_index;      /* Suchbeschleunigung */
static uint16_t *refs;            /* Besitzer je Seite, 0 = unbenutzt */
static uint64_t  shared_pages;    /* wie oft Seiten mehrfach vergeben sind */

static inline void bit_set(uint64_t i)   { bitmap[i / 8] |=  (uint8_t)(1 << (i % 8)); }
static inline void bit_clear(uint64_t i) { bitmap[i / 8] &= (uint8_t)~(1 << (i % 8)); }
static inline bool bit_test(uint64_t i)  { return bitmap[i / 8] & (1 << (i % 8)); }

void pmm_init(void)
{
    const struct boot_info *bi = boot_info();

    /* 1. Hoechste physische Adresse bestimmen. */
    for (uint32_t i = 0; i < bi->memmap_count; i++) {
        const struct boot_mem_entry *e = &bi->memmap[i];
        uint64_t top = e->base + e->length;

        if (e->type == BOOT_MEM_USABLE && top > highest_addr)
            highest_addr = top;
    }

    bitmap_pages = highest_addr / PAGE_SIZE;
    uint64_t bitmap_size = ALIGN_UP(bitmap_pages / 8, PAGE_SIZE);

    /* 2. Platz fuer die Bitmap in einem nutzbaren Bereich finden. */
    for (uint32_t i = 0; i < bi->memmap_count; i++) {
        const struct boot_mem_entry *e = &bi->memmap[i];

        if (e->type == BOOT_MEM_USABLE && e->length >= bitmap_size) {
            bitmap = phys_to_virt(e->base);
            break;
        }
    }

    if (!bitmap)
        panic("Kein Platz fuer die Speicher-Bitmap gefunden");

    /* 3. Erst alles als belegt markieren ... */
    memset(bitmap, 0xFF, bitmap_size);
    used_pages = bitmap_pages;

    /* 4. ... dann die nutzbaren Bereiche freigeben. */
    for (uint32_t i = 0; i < bi->memmap_count; i++) {
        const struct boot_mem_entry *e = &bi->memmap[i];

        if (e->type != BOOT_MEM_USABLE)
            continue;

        uint64_t start = ALIGN_UP(e->base, PAGE_SIZE);
        uint64_t end   = ALIGN_DOWN(e->base + e->length, PAGE_SIZE);

        for (uint64_t p = start; p < end; p += PAGE_SIZE) {
            uint64_t idx = p / PAGE_SIZE;
            if (idx < bitmap_pages && bit_test(idx)) {
                bit_clear(idx);
                used_pages--;
                usable_pages++;
            }
        }
    }

    /* 5. Die Bitmap selbst gehoert nicht zum freien Speicher. */
    uint64_t bm_phys = (uint64_t)bitmap - g_hhdm_offset;
    for (uint64_t p = 0; p < bitmap_size; p += PAGE_SIZE) {
        uint64_t idx = (bm_phys + p) / PAGE_SIZE;
        if (idx < bitmap_pages && !bit_test(idx)) {
            bit_set(idx);
            used_pages++;
        }
    }

    /* 6. Die erste Seite bleibt tabu - so faellt ein NULL-Zugriff auf. */
    if (!bit_test(0)) {
        bit_set(0);
        used_pages++;
    }

    /* 7. Platz fuer die Besitzerzaehler - zwei Byte je Seite. Ab jetzt
     *    ist der Allokator schon benutzbar, also holt er ihn sich
     *    selbst. */
    uint64_t refs_bytes = ALIGN_UP(bitmap_pages * sizeof(uint16_t), PAGE_SIZE);
    uint64_t refs_phys = pmm_alloc_pages(refs_bytes / PAGE_SIZE);

    if (!refs_phys)
        panic("Kein Platz fuer die Besitzerzaehler");

    refs = phys_to_virt(refs_phys);
    memset(refs, 0, refs_bytes);

    /* Die Seiten des Zaehlerfeldes selbst gehoeren sich einmal. */
    for (uint64_t p = 0; p < refs_bytes; p += PAGE_SIZE)
        refs[(refs_phys + p) / PAGE_SIZE] = 1;

    kprintf("PMM         : %u MiB verwaltet, %u MiB frei\n",
            (unsigned)(usable_pages * PAGE_SIZE / (1024 * 1024)),
            (unsigned)(pmm_free_bytes() / (1024 * 1024)));
}

static uint64_t find_run(size_t count, uint64_t from, uint64_t to)
{
    uint64_t run = 0;

    for (uint64_t i = from; i < to; i++) {
        if (bit_test(i)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            uint64_t start = i - count + 1;

            for (uint64_t p = start; p <= i; p++) {
                bit_set(p);
                if (refs)
                    refs[p] = 1;
            }

            used_pages += count;
            last_index = i + 1;
            return start * PAGE_SIZE;
        }
    }
    return 0;
}

/* Mit mehreren Kernen darf nur einer zugleich in der Bitkarte
 * herumraeumen. */
static struct spinlock pmm_lock = SPINLOCK_INIT("seitenverwaltung");

uint64_t pmm_alloc_pages(size_t count)
{
    if (count == 0 || !bitmap)
        return 0;

    uint64_t flags = spin_lock_irq(&pmm_lock);
    uint64_t addr = find_run(count, last_index, bitmap_pages);

    if (!addr) {
        last_index = 0;
        addr = find_run(count, 0, bitmap_pages);
    }

    spin_unlock_irq(&pmm_lock, flags);
    return addr;
}

uint64_t pmm_alloc_page(void)
{
    return pmm_alloc_pages(1);
}

/* Gibt eine Seite frei - aber nur, wenn sie niemandem sonst mehr
 * gehoert. Sonst wird nur der Zaehler kleiner. */
void pmm_free_pages(uint64_t phys, size_t count)
{
    uint64_t idx = phys / PAGE_SIZE;

    uint64_t flags = spin_lock_irq(&pmm_lock);

    for (size_t i = 0; i < count; i++) {
        uint64_t page = idx + i;

        if (page >= bitmap_pages || !bit_test(page))
            continue;

        if (refs && refs[page] > 1) {
            refs[page]--;
            shared_pages--;
            continue;
        }

        if (refs)
            refs[page] = 0;
        bit_clear(page);
        used_pages--;
    }
    spin_unlock_irq(&pmm_lock, flags);
}

void pmm_free_page(uint64_t phys)
{
    pmm_free_pages(phys, 1);
}

void pmm_share_page(uint64_t phys)
{
    uint64_t page = phys / PAGE_SIZE;

    if (!refs || page >= bitmap_pages)
        return;

    uint64_t flags = spin_lock_irq(&pmm_lock);

    /* Mehr Besitzer, als der Zaehler fasst, kann es nicht geben - so
     * viele Prozesse gibt es nicht. Sicherheitshalber bleibt er dann
     * aber stehen, und die Seite wird nie mehr frei. */
    if (refs[page] < 0xFFFF) {
        refs[page]++;
        shared_pages++;
    }

    spin_unlock_irq(&pmm_lock, flags);
}

bool pmm_shared(uint64_t phys)
{
    uint64_t page = phys / PAGE_SIZE;

    if (!refs || page >= bitmap_pages)
        return false;
    return refs[page] > 1;
}

uint64_t pmm_shared_bytes(void) { return shared_pages * PAGE_SIZE; }

uint64_t pmm_total_bytes(void) { return usable_pages * PAGE_SIZE; }
uint64_t pmm_used_bytes(void)
{
    uint64_t reserved = bitmap_pages - usable_pages;
    return (used_pages - reserved) * PAGE_SIZE;
}
uint64_t pmm_free_bytes(void) { return pmm_total_bytes() - pmm_used_bytes(); }
