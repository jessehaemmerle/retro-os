/* block.h - einheitlicher Zugriff auf Datentraeger. */
#ifndef BLOCK_H
#define BLOCK_H

#include "retro.h"

#define BLOCK_SECTOR_SIZE 512
#define BLOCK_MAX_DEVICES 8

struct block_device;

typedef bool (*block_rw_fn)(struct block_device *dev, uint64_t lba,
                            uint32_t count, void *buffer);

struct block_device {
    char        name[16];       /* z.B. "sata0" oder "ide0"  */
    char        model[48];      /* Modellbezeichnung des Laufwerks */
    uint64_t    sector_count;
    uint32_t    sector_size;

    block_rw_fn read;
    block_rw_fn write;
    void       *driver_data;
};

void block_register(struct block_device *dev);
size_t block_device_count(void);
struct block_device *block_device_at(size_t index);
struct block_device *block_primary(void);

bool block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
bool block_write(struct block_device *dev, uint64_t lba, uint32_t count,
                 const void *buf);

/* Sucht auf allen Bussen nach Laufwerken. */
void storage_init(void);

void ahci_init(void);
void ata_init(void);
void nvme_init(void);

#endif /* BLOCK_H */
