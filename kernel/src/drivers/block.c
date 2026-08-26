/* block.c - Registrierung und Zugriff auf Datentraeger.
 *
 * Die Treiber (AHCI, ATA) melden hier ihre Laufwerke an; das Dateisystem
 * kennt danach nur noch Sektornummern und muss nicht wissen, an welchem
 * Bus die Platte haengt.
 */

#include "block.h"
#include "kstring.h"

static struct block_device *devices[BLOCK_MAX_DEVICES];
static size_t               device_count;

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

bool block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev || !dev->read || count == 0)
        return false;
    if (lba + count > dev->sector_count)
        return false;

    return dev->read(dev, lba, count, buf);
}

bool block_write(struct block_device *dev, uint64_t lba, uint32_t count,
                 const void *buf)
{
    if (!dev || !dev->write || count == 0)
        return false;
    if (lba + count > dev->sector_count)
        return false;

    return dev->write(dev, lba, count, (void *)buf);
}

void storage_init(void)
{
    /* Erst die schnellen Wege, dann die alten. */
    nvme_init();
    ahci_init();
    ata_init();

    if (device_count == 0)
        kprintf("Datentraeger: keiner gefunden\n");
}
