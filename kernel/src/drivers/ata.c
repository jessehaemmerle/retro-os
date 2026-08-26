/* ata.c - Treiber fuer IDE-Festplatten im PIO-Modus.
 *
 * Der alte Weg: keine Speicherstrukturen, kein DMA - jedes Wort wandert
 * einzeln durch einen I/O-Port. Langsam, aber er funktioniert auf jedem
 * Rechner mit IDE-Controller und braucht keinerlei Einrichtung. Wird nur
 * genutzt, wenn kein AHCI-Controller gefunden wurde.
 */

#include "block.h"
#include "arch.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"

#define ATA_PRIMARY_IO    0x1F0
#define ATA_PRIMARY_CTRL  0x3F6
#define ATA_SECONDARY_IO  0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_REG_DATA      0
#define ATA_REG_ERROR     1
#define ATA_REG_SECCOUNT  2
#define ATA_REG_LBA0      3
#define ATA_REG_LBA1      4
#define ATA_REG_LBA2      5
#define ATA_REG_DRIVE     6
#define ATA_REG_STATUS    7
#define ATA_REG_COMMAND   7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_MAX_DRIVES 2

struct ata_drive {
    struct block_device block;
    uint16_t io_base;
    uint16_t ctrl_base;
    bool     slave;
    bool     lba48;
};

static struct ata_drive drives[ATA_MAX_DRIVES];
static size_t           drive_count;

static void ata_delay(uint16_t ctrl)
{
    /* Viermal den Status lesen entspricht rund 400 ns Wartezeit. */
    for (int i = 0; i < 4; i++)
        (void)inb(ctrl);
}

static bool ata_wait(uint16_t io, uint8_t mask, uint8_t want, uint32_t ms)
{
    uint64_t deadline = timer_ms() + ms;

    for (;;) {
        uint8_t status = inb(io + ATA_REG_STATUS);

        if (status == 0xFF)
            return false;                       /* kein Laufwerk am Bus */
        if (status & ATA_SR_ERR)
            return false;
        if ((status & mask) == want)
            return true;
        if (timer_ms() > deadline)
            return false;
    }
}

static void select_drive(struct ata_drive *d, uint64_t lba, uint8_t count,
                         bool lba48)
{
    uint16_t io = d->io_base;
    uint8_t  select = (uint8_t)(0xE0 | (d->slave ? 0x10 : 0x00));

    if (lba48) {
        outb(io + ATA_REG_DRIVE, select);
        outb(io + ATA_REG_SECCOUNT, 0);
        outb(io + ATA_REG_LBA0, (uint8_t)(lba >> 24));
        outb(io + ATA_REG_LBA1, (uint8_t)(lba >> 32));
        outb(io + ATA_REG_LBA2, (uint8_t)(lba >> 40));
        outb(io + ATA_REG_SECCOUNT, count);
        outb(io + ATA_REG_LBA0, (uint8_t)(lba));
        outb(io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        outb(io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    } else {
        outb(io + ATA_REG_DRIVE, (uint8_t)(select | ((lba >> 24) & 0x0F)));
        outb(io + ATA_REG_SECCOUNT, count);
        outb(io + ATA_REG_LBA0, (uint8_t)(lba));
        outb(io + ATA_REG_LBA1, (uint8_t)(lba >> 8));
        outb(io + ATA_REG_LBA2, (uint8_t)(lba >> 16));
    }
    ata_delay(d->ctrl_base);
}

static bool transfer(struct ata_drive *d, uint64_t lba, uint32_t count,
                     void *buffer, bool write)
{
    uint16_t  io = d->io_base;
    uint16_t *buf = buffer;

    while (count > 0) {
        uint32_t chunk = MIN(count, d->lba48 ? 65536u : 256u);
        uint8_t  encoded = (uint8_t)(chunk == 256 || chunk == 65536 ? 0 : chunk);

        if (!ata_wait(io, ATA_SR_BSY, 0, 3000))
            return false;

        select_drive(d, lba, encoded, d->lba48);

        uint8_t cmd;
        if (write)
            cmd = d->lba48 ? 0x34 : 0x30;      /* WRITE SECTORS (EXT) */
        else
            cmd = d->lba48 ? 0x24 : 0x20;      /* READ SECTORS (EXT)  */
        outb(io + ATA_REG_COMMAND, cmd);

        for (uint32_t s = 0; s < chunk; s++) {
            if (!ata_wait(io, ATA_SR_BSY | ATA_SR_DRQ, ATA_SR_DRQ, 3000))
                return false;

            if (write) {
                for (int i = 0; i < 256; i++)
                    outw(io + ATA_REG_DATA, *buf++);
                ata_delay(d->ctrl_base);
            } else {
                for (int i = 0; i < 256; i++)
                    *buf++ = inw(io + ATA_REG_DATA);
            }
        }

        if (write) {
            outb(io + ATA_REG_COMMAND, d->lba48 ? 0xEA : 0xE7);   /* FLUSH */
            if (!ata_wait(io, ATA_SR_BSY, 0, 3000))
                return false;
        }

        lba   += chunk;
        count -= chunk;
    }
    return true;
}

static bool ata_read(struct block_device *bdev, uint64_t lba, uint32_t count,
                     void *buffer)
{
    return transfer(bdev->driver_data, lba, count, buffer, false);
}

static bool ata_write(struct block_device *bdev, uint64_t lba, uint32_t count,
                      void *buffer)
{
    return transfer(bdev->driver_data, lba, count, buffer, true);
}

static void extract_string(const uint16_t *id, int word, int words, char *out)
{
    int n = 0;

    for (int i = 0; i < words; i++) {
        out[n++] = (char)(id[word + i] >> 8);
        out[n++] = (char)(id[word + i] & 0xFF);
    }
    out[n] = '\0';

    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0'))
        out[--n] = '\0';
}

static void identify(uint16_t io, uint16_t ctrl, bool slave, int index)
{
    if (drive_count >= ATA_MAX_DRIVES)
        return;

    outb(io + ATA_REG_DRIVE, (uint8_t)(slave ? 0xB0 : 0xA0));
    ata_delay(ctrl);
    outb(io + ATA_REG_SECCOUNT, 0);
    outb(io + ATA_REG_LBA0, 0);
    outb(io + ATA_REG_LBA1, 0);
    outb(io + ATA_REG_LBA2, 0);
    outb(io + ATA_REG_COMMAND, 0xEC);          /* IDENTIFY DEVICE */
    ata_delay(ctrl);

    if (inb(io + ATA_REG_STATUS) == 0)
        return;                                 /* kein Laufwerk */

    if (!ata_wait(io, ATA_SR_BSY, 0, 1000))
        return;

    /* Ein ATAPI-Geraet meldet sich hier mit einer Kennung in LBA1/LBA2. */
    if (inb(io + ATA_REG_LBA1) || inb(io + ATA_REG_LBA2))
        return;

    if (!ata_wait(io, ATA_SR_DRQ, ATA_SR_DRQ, 1000))
        return;

    uint16_t id[256] = { 0 };
    for (int i = 0; i < 256; i++)
        id[i] = inw(io + ATA_REG_DATA);

    struct ata_drive *d = &drives[drive_count];

    memset(d, 0, sizeof(*d));
    d->io_base   = io;
    d->ctrl_base = ctrl;
    d->slave     = slave;
    d->lba48     = (id[83] & (1 << 10)) != 0;

    uint64_t sectors;
    if (d->lba48) {
        sectors = (uint64_t)id[100]
                | ((uint64_t)id[101] << 16)
                | ((uint64_t)id[102] << 32)
                | ((uint64_t)id[103] << 48);
    } else {
        sectors = ((uint32_t)id[61] << 16) | id[60];
    }
    if (sectors == 0)
        return;

    d->block.sector_count = sectors;
    d->block.sector_size  = BLOCK_SECTOR_SIZE;
    d->block.read         = ata_read;
    d->block.write        = ata_write;
    d->block.driver_data  = d;
    ksnprintf(d->block.name, sizeof(d->block.name), "ide%d", index);
    extract_string(id, 27, 20, d->block.model);

    drive_count++;
    block_register(&d->block);
}

void ata_init(void)
{
    /* Nur einspringen, wenn nicht schon ein AHCI-Laufwerk gefunden wurde. */
    if (block_device_count() > 0)
        return;

    identify(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, false, 0);
    identify(ATA_PRIMARY_IO, ATA_PRIMARY_CTRL, true, 1);
    identify(ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, false, 2);
}
