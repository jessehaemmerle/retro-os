/* fat.h - FAT32-Dateisystem auf einem Blockgeraet.
 *
 * FAT wurde gewaehlt, weil es einfach genug ist, um es vollstaendig selbst
 * zu schreiben, und weil jeder andere Rechner die Datentraeger lesen kann.
 * Lange Dateinamen (VFAT) werden gelesen und geschrieben.
 */
#ifndef FAT_H
#define FAT_H

#include "block.h"

#define FAT_NAME_MAX 63

struct fat_volume {
    struct block_device *dev;

    uint64_t partition_lba;      /* Beginn der Partition               */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;

    uint64_t fat_lba;            /* erster Sektor der ersten FAT       */
    uint64_t data_lba;           /* Sektor von Cluster 2               */
    uint32_t cluster_count;
    uint32_t cluster_bytes;

    char     label[12];
    bool     mounted;
};

/* Beschreibt, wo ein Eintrag im Verzeichnis liegt - noetig zum Aendern. */
struct fat_entry_ref {
    uint32_t dir_cluster;
    uint32_t index;              /* laufende Nummer des kurzen Eintrags */
    uint8_t  lfn_slots;          /* wieviele Namensteile davor liegen   */
};

struct fat_dirent {
    char     name[FAT_NAME_MAX + 1];
    bool     is_dir;
    bool     read_only;
    uint32_t first_cluster;
    uint32_t size;

    uint8_t  day, month;
    uint16_t year;
    uint8_t  hour, minute;

    struct fat_entry_ref ref;
};

typedef void (*fat_dir_cb)(void *user, const struct fat_dirent *entry);

bool fat_mount(struct block_device *dev, struct fat_volume *vol);
bool fat_format(struct block_device *dev, const char *label);

bool fat_list_dir(struct fat_volume *vol, uint32_t dir_cluster,
                  fat_dir_cb cb, void *user);

bool fat_read_file(struct fat_volume *vol, uint32_t first_cluster,
                   uint32_t size, void *buffer);

/* Schreibt eine Datei vollstaendig neu; passt Clusterkette und Verzeichnis-
 * eintrag an. first_cluster wird aktualisiert. */
bool fat_write_file(struct fat_volume *vol, const struct fat_entry_ref *ref,
                    uint32_t *first_cluster, const void *data, uint32_t size);

bool fat_create(struct fat_volume *vol, uint32_t dir_cluster, const char *name,
                bool is_dir, struct fat_dirent *out);

bool fat_delete(struct fat_volume *vol, const struct fat_entry_ref *ref,
                uint32_t first_cluster, bool is_dir);

bool fat_dir_is_empty(struct fat_volume *vol, uint32_t dir_cluster);

/* Benennt um, indem der alte Eintrag entfernt und ein neuer angelegt wird;
 * die Clusterkette bleibt dabei erhalten. */
bool fat_rename(struct fat_volume *vol, struct fat_dirent *entry,
                const char *new_name);

uint64_t fat_total_bytes(struct fat_volume *vol);
uint64_t fat_free_bytes(struct fat_volume *vol);

#endif /* FAT_H */
