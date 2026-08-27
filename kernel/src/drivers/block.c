/* block.c - Registrierung und Zugriff auf Datentraeger.
 *
 * Die Treiber (AHCI, ATA) melden hier ihre Laufwerke an; das Dateisystem
 * kennt danach nur noch Sektornummern und muss nicht wissen, an welchem
 * Bus die Platte haengt.
 *
 * Dazwischen sitzt ein Puffer. FAT liest beim Verfolgen einer
 * Clusterkette immer wieder dieselben paar Sektoren der Zuordnungstabelle
 * und schreibt beim Anlegen einer Datei denselben Verzeichnissektor
 * mehrfach. Ohne Puffer geht jeder dieser Zugriffe einzeln auf die
 * Platte - das Kopieren des Kernels bei der Installation hat so eine
 * Minute gebraucht.
 *
 * Der Puffer haelt einzelne Sektoren und schreibt geaenderte erst
 * zurueck, wenn ihr Platz gebraucht wird oder jemand ausdruecklich
 * darum bittet. Verdraengt wird der am laengsten unbenutzte.
 */

#include "block.h"
#include "kstring.h"
#include "spinlock.h"
#include "thread.h"

static struct block_device *devices[BLOCK_MAX_DEVICES];
static size_t               device_count;

/* ------------------------------------------------------------------ */
/* Sektorpuffer                                                        */
/* ------------------------------------------------------------------ */

#define CACHE_SLOTS 256

struct cache_slot {
    struct block_device *dev;
    uint64_t             lba;
    uint64_t             used;      /* Zeitstempel des letzten Zugriffs */
    bool                 valid;
    bool                 dirty;
    uint8_t              data[BLOCK_SECTOR_SIZE];
};

static struct cache_slot cache[CACHE_SLOTS];
static uint64_t          cache_clock;
static struct spinlock   cache_lock = SPINLOCK_INIT("sektorpuffer");
static bool              cache_enabled = true;

static struct cache_slot *cache_find(struct block_device *dev, uint64_t lba)
{
    for (size_t i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].valid && cache[i].dev == dev && cache[i].lba == lba)
            return &cache[i];
    }
    return NULL;
}

/* Schreibt einen Platz zurueck, falls er geaendert wurde. */
static bool slot_flush(struct cache_slot *slot)
{
    if (!slot->valid || !slot->dirty)
        return true;
    if (!slot->dev->write)
        return false;
    if (!slot->dev->write(slot->dev, slot->lba, 1, slot->data))
        return false;

    slot->dirty = false;
    return true;
}

/* Sucht einen freien Platz, sonst den am laengsten unbenutzten. */
static struct cache_slot *cache_claim(void)
{
    struct cache_slot *oldest = &cache[0];

    for (size_t i = 0; i < CACHE_SLOTS; i++) {
        if (!cache[i].valid)
            return &cache[i];
        if (cache[i].used < oldest->used)
            oldest = &cache[i];
    }

    if (!slot_flush(oldest))
        return NULL;

    oldest->valid = false;
    return oldest;
}

bool block_flush(struct block_device *dev)
{
    bool ok = true;

    uint64_t __flags = spin_lock_irq(&cache_lock);
    for (size_t i = 0; i < CACHE_SLOTS; i++) {
        if (cache[i].valid && (!dev || cache[i].dev == dev)) {
            if (!slot_flush(&cache[i]))
                ok = false;
        }
    }
    spin_unlock_irq(&cache_lock, __flags);
    return ok;
}

/* Wirft alles weg, ohne zurueckzuschreiben. Noetig, wenn ein Traeger
 * unter dem Puffer weg neu beschrieben wurde. */
void block_cache_drop(struct block_device *dev)
{
    uint64_t __flags = spin_lock_irq(&cache_lock);
    for (size_t i = 0; i < CACHE_SLOTS; i++) {
        if (!dev || cache[i].dev == dev) {
            cache[i].valid = false;
            cache[i].dirty = false;
        }
    }
    spin_unlock_irq(&cache_lock, __flags);
}

void block_register(struct block_device *dev)
{
    if (device_count >= BLOCK_MAX_DEVICES || !dev)
        return;

    devices[device_count++] = dev;

    uint64_t mib = dev->sector_count * dev->sector_size / (1024 * 1024);
    kprintf("Datentraeger: %s - %s, %u MiB\n", dev->name, dev->model,
            (unsigned)mib);
}

size_t block_device_count(void) { return device_count; }

struct block_device *block_device_at(size_t index)
{
    return index < device_count ? devices[index] : NULL;
}

struct block_device *block_primary(void)
{
    return device_count ? devices[0] : NULL;
}

/* Ein einzelner Sektor ueber den Puffer. */
static bool cached_read(struct block_device *dev, uint64_t lba, void *buf)
{
    struct cache_slot *slot = cache_find(dev, lba);

    if (!slot) {
        slot = cache_claim();
        if (!slot)
            return dev->read(dev, lba, 1, buf);

        if (!dev->read(dev, lba, 1, slot->data))
            return false;

        slot->dev = dev;
        slot->lba = lba;
        slot->valid = true;
        slot->dirty = false;
    }

    slot->used = ++cache_clock;
    memcpy(buf, slot->data, BLOCK_SECTOR_SIZE);
    return true;
}

static bool cached_write(struct block_device *dev, uint64_t lba,
                         const void *buf)
{
    struct cache_slot *slot = cache_find(dev, lba);

    if (!slot) {
        slot = cache_claim();
        if (!slot)
            return dev->write(dev, lba, 1, (void *)buf);

        slot->dev = dev;
        slot->lba = lba;
        slot->valid = true;
    }

    memcpy(slot->data, buf, BLOCK_SECTOR_SIZE);
    slot->used = ++cache_clock;
    slot->dirty = true;
    return true;
}

bool block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev || !dev->read || count == 0)
        return false;
    if (lba + count > dev->sector_count)
        return false;

    /* Grosse Zugriffe gehen am Puffer vorbei - sie wuerden ihn nur
     * leerraeumen, und der Treiber holt sie ohnehin in einem Stueck.
     * Geaenderte Sektoren darin muessen aber vorher heraus. */
    if (!cache_enabled || dev->sector_size != BLOCK_SECTOR_SIZE || count > 8) {
        uint64_t __flags = spin_lock_irq(&cache_lock);
        for (uint32_t i = 0; i < count; i++) {
            struct cache_slot *slot = cache_find(dev, lba + i);

            if (slot && !slot_flush(slot)) {
                spin_unlock_irq(&cache_lock, __flags);
                return false;
            }
        }
        spin_unlock_irq(&cache_lock, __flags);
        return dev->read(dev, lba, count, buf);
    }

    uint8_t *out = buf;
    bool ok = true;

    uint64_t __flags = spin_lock_irq(&cache_lock);
    for (uint32_t i = 0; i < count && ok; i++)
        ok = cached_read(dev, lba + i, out + (size_t)i * BLOCK_SECTOR_SIZE);
    spin_unlock_irq(&cache_lock, __flags);
    return ok;
}

bool block_write(struct block_device *dev, uint64_t lba, uint32_t count,
                 const void *buf)
{
    if (!dev || !dev->write || count == 0)
        return false;
    if (lba + count > dev->sector_count)
        return false;

    if (!cache_enabled || dev->sector_size != BLOCK_SECTOR_SIZE || count > 8) {
        uint64_t __flags = spin_lock_irq(&cache_lock);
        for (uint32_t i = 0; i < count; i++) {
            struct cache_slot *slot = cache_find(dev, lba + i);

            if (slot) {
                slot->valid = false;   /* der Treiber ueberschreibt ihn */
                slot->dirty = false;
            }
        }
        spin_unlock_irq(&cache_lock, __flags);
        return dev->write(dev, lba, count, (void *)buf);
    }

    const uint8_t *in = buf;
    bool ok = true;

    uint64_t __flags = spin_lock_irq(&cache_lock);
    for (uint32_t i = 0; i < count && ok; i++)
        ok = cached_write(dev, lba + i, in + (size_t)i * BLOCK_SECTOR_SIZE);
    spin_unlock_irq(&cache_lock, __flags);
    return ok;
}

void storage_init(void)
{
    /* Erst die schnellen Wege, dann die alten. Datentraeger am USB-Bus
     * kommen spaeter dazu, wenn der Controller aufgezaehlt hat. */
    nvme_init();
    ahci_init();
    ata_init();
}
