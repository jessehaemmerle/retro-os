/* partition.c - MBR- und GPT-Partitionstabellen lesen.
 *
 * Beide Verfahren beginnen im ersten Sektor. Der MBR traegt dort vier
 * Eintraege; die GPT stellt an dieselbe Stelle einen Schutzeintrag mit
 * der Kennung 0xEE und legt ihre eigentliche Tabelle in den Sektor
 * dahinter. Wir sehen also zuerst nach der GPT und fallen sonst auf den
 * MBR zurueck.
 */

#include "partition.h"
#include "crypto.h"
#include "kstring.h"
#include "mm.h"

static uint16_t rd16(const uint8_t *p, size_t at)
{
    return (uint16_t)(p[at] | (p[at + 1] << 8));
}

static uint32_t rd32(const uint8_t *p, size_t at)
{
    return (uint32_t)p[at] | ((uint32_t)p[at + 1] << 8) |
           ((uint32_t)p[at + 2] << 16) | ((uint32_t)p[at + 3] << 24);
}

static uint64_t rd64(const uint8_t *p, size_t at)
{
    return (uint64_t)rd32(p, at) | ((uint64_t)rd32(p, at + 4) << 32);
}

/* Diese Kennungen im MBR stehen fuer ein FAT-Dateisystem. */
static bool mbr_type_is_fat(uint8_t type)
{
    switch (type) {
    case 0x01:  /* FAT12                 */
    case 0x04:  /* FAT16 unter 32 MiB    */
    case 0x06:  /* FAT16                 */
    case 0x0B:  /* FAT32 mit CHS         */
    case 0x0C:  /* FAT32 mit LBA         */
    case 0x0E:  /* FAT16 mit LBA         */
    case 0x1B:  /* versteckt, FAT32      */
    case 0x1C:
    case 0x1E:
    case 0xEF:  /* EFI-Systempartition   */
        return true;
    default:
        return false;
    }
}

/* Die Kennungen der GPT sind 16-Byte-Werte. Wir brauchen nur zwei. */
static const uint8_t GUID_EFI[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
static const uint8_t GUID_MSDATA[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

/* Wandelt den Namen aus der GPT - dort in UTF-16 - in Latin-1. */
static void gpt_name(const uint8_t *entry, char *out, size_t size)
{
    size_t at = 0;

    for (size_t i = 0; i < 36 && at + 1 < size; i++) {
        uint16_t code = rd16(entry, 56 + i * 2);

        if (code == 0)
            break;
        out[at++] = code < 256 ? (char)code : '?';
    }
    out[at] = '\0';
}

static size_t scan_gpt(struct block_device *dev, struct partition *out,
                       size_t max)
{
    uint8_t header[512];

    if (!block_read(dev, 1, 1, header))
        return 0;
    if (memcmp(header, "EFI PART", 8) != 0)
        return 0;

    uint64_t table_lba = rd64(header, 72);
    uint32_t entries = rd32(header, 80);
    uint32_t entry_size = rd32(header, 84);

    if (entry_size < 128 || entry_size > 512 || entries == 0)
        return 0;
    if (entries > 256)
        entries = 256;

    size_t found = 0;
    uint8_t sector[512];
    uint32_t per_sector = 512 / entry_size;

    for (uint32_t i = 0; i < entries && found < max; i++) {
        uint32_t sector_index = i / per_sector;
        uint32_t offset = (i % per_sector) * entry_size;

        if (offset == 0 && !block_read(dev, table_lba + sector_index, 1,
                                       sector))
            break;

        const uint8_t *entry = sector + offset;
        bool empty = true;

        for (int k = 0; k < 16; k++)
            if (entry[k]) {
                empty = false;
                break;
            }
        if (empty)
            continue;

        uint64_t first = rd64(entry, 32);
        uint64_t last = rd64(entry, 40);

        if (last < first)
            continue;

        struct partition *p = &out[found++];

        memset(p, 0, sizeof(*p));
        p->start = first;
        p->count = last - first + 1;
        p->is_efi = memcmp(entry, GUID_EFI, 16) == 0;
        p->is_fat = p->is_efi || memcmp(entry, GUID_MSDATA, 16) == 0;
        gpt_name(entry, p->name, sizeof(p->name));
    }
    return found;
}

static size_t scan_mbr(struct block_device *dev, struct partition *out,
                       size_t max)
{
    uint8_t sector[512];

    if (!block_read(dev, 0, 1, sector))
        return 0;
    if (rd16(sector, 510) != 0xAA55)
        return 0;

    size_t found = 0;

    for (int i = 0; i < 4 && found < max; i++) {
        const uint8_t *entry = &sector[446 + i * 16];
        uint8_t type = entry[4];
        uint32_t start = rd32(entry, 8);
        uint32_t count = rd32(entry, 12);

        if (type == 0 || count == 0)
            continue;
        if (type == 0xEE)          /* Schutzeintrag einer GPT */
            continue;

        struct partition *p = &out[found++];

        memset(p, 0, sizeof(*p));
        p->start = start;
        p->count = count;
        p->mbr_type = type;
        p->is_fat = mbr_type_is_fat(type);
        p->is_efi = type == 0xEF;
        ksnprintf(p->name, sizeof(p->name), "Partition %d", i + 1);

        /* Erweiterte Partitionen tragen ihre Eintraege in einer Kette. */
        if (type == 0x05 || type == 0x0F || type == 0x85) {
            uint64_t base = start;
            uint64_t next = start;

            found--;                /* der Behaelter selbst zaehlt nicht */
            for (int guard = 0; guard < 16 && found < max; guard++) {
                uint8_t link[512];

                if (!block_read(dev, next, 1, link))
                    break;
                if (rd16(link, 510) != 0xAA55)
                    break;

                uint8_t inner_type = link[446 + 4];
                uint32_t inner_start = rd32(&link[446], 8);
                uint32_t inner_count = rd32(&link[446], 12);

                if (inner_type && inner_count) {
                    struct partition *q = &out[found++];

                    memset(q, 0, sizeof(*q));
                    q->start = next + inner_start;
                    q->count = inner_count;
                    q->mbr_type = inner_type;
                    q->is_fat = mbr_type_is_fat(inner_type);
                    ksnprintf(q->name, sizeof(q->name), "Logisch %d",
                              guard + 1);
                }

                uint32_t chain = rd32(&link[446 + 16], 8);

                if (!chain)
                    break;
                next = base + chain;
            }
        }
    }
    return found;
}

size_t partition_scan(struct block_device *dev, struct partition *out,
                      size_t max, enum partition_scheme *scheme)
{
    if (scheme)
        *scheme = SCHEME_NONE;
    if (!dev || !out || max == 0)
        return 0;

    size_t found = scan_gpt(dev, out, max);

    if (found > 0) {
        if (scheme)
            *scheme = SCHEME_GPT;
        return found;
    }

    found = scan_mbr(dev, out, max);
    if (found > 0 && scheme)
        *scheme = SCHEME_MBR;
    return found;
}

const char *partition_scheme_name(enum partition_scheme scheme)
{
    switch (scheme) {
    case SCHEME_GPT: return "GPT";
    case SCHEME_MBR: return "MBR";
    default:         return "ohne Tabelle";
    }
}

/* ------------------------------------------------------------------ */
/* Eine neue GUID-Tabelle schreiben                                    */
/* ------------------------------------------------------------------ */

static void wr16(uint8_t *p, size_t at, uint16_t value)
{
    p[at] = (uint8_t)value;
    p[at + 1] = (uint8_t)(value >> 8);
}

static void wr32(uint8_t *p, size_t at, uint32_t value)
{
    for (int i = 0; i < 4; i++)
        p[at + i] = (uint8_t)(value >> (8 * i));
}

static void wr64(uint8_t *p, size_t at, uint64_t value)
{
    for (int i = 0; i < 8; i++)
        p[at + i] = (uint8_t)(value >> (8 * i));
}

/* Eine Kennung nach Art 4: alles gewuerfelt, bis auf die sechs Bit, die
 * die Bauart des Wertes verraten. */
static void make_guid(uint8_t out[16])
{
    crypto_random(out, 16);
    out[7] = (uint8_t)((out[7] & 0x0F) | 0x40);
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);
}

/* Der Name steht in der Tabelle als UTF-16. Fuer unsere Zwecke genuegt
 * es, jedes Zeichen auf zwei Byte zu verbreitern. */
static void put_name(uint8_t *entry, const char *name)
{
    for (size_t i = 0; i < 36; i++) {
        uint16_t code = (name && name[i]) ? (uint8_t)name[i] : 0;

        wr16(entry, 56 + i * 2, code);
        if (!code)
            break;
    }
}

/* Der Schutzeintrag im ersten Sektor. Ein Rechner, der nur MBR kennt,
 * sieht dort eine einzige Partition, die den ganzen Traeger belegt, und
 * laesst die Finger davon. */
static bool write_protective_mbr(struct block_device *dev)
{
    uint8_t sector[512];
    uint64_t span = dev->sector_count - 1;

    memset(sector, 0, sizeof(sector));

    uint8_t *entry = &sector[446];

    entry[0] = 0x00;                 /* nicht startfaehig markiert */
    entry[1] = 0x00;                 /* Kopf                       */
    entry[2] = 0x02;                 /* Sektor 2, Zylinder 0       */
    entry[3] = 0x00;
    entry[4] = 0xEE;                 /* "hier liegt eine GPT"      */
    entry[5] = 0xFF;                 /* Ende: unbekannt            */
    entry[6] = 0xFF;
    entry[7] = 0xFF;
    wr32(entry, 8, 1);
    wr32(entry, 12, (uint32_t)MIN(span, 0xFFFFFFFFull));

    wr16(sector, 510, 0xAA55);
    return block_write(dev, 0, 1, sector);
}

/* Kopf der Tabelle. Die Pruefsumme deckt nur die ersten 92 Byte ab und
 * wird ueber sich selbst als Null gerechnet. */
static void build_header(uint8_t *sector, uint64_t my_lba, uint64_t other_lba,
                         uint64_t entries_lba, uint64_t first_usable,
                         uint64_t last_usable, const uint8_t disk_guid[16],
                         uint32_t entries_crc)
{
    memset(sector, 0, 512);
    memcpy(sector, "EFI PART", 8);
    wr32(sector, 8, 0x00010000);         /* Fassung 1.0            */
    wr32(sector, 12, 92);                /* Laenge des Kopfes      */
    wr32(sector, 16, 0);                 /* Pruefsumme, spaeter    */
    wr32(sector, 20, 0);
    wr64(sector, 24, my_lba);
    wr64(sector, 32, other_lba);
    wr64(sector, 40, first_usable);
    wr64(sector, 48, last_usable);
    memcpy(&sector[56], disk_guid, 16);
    wr64(sector, 72, entries_lba);
    wr32(sector, 80, GPT_ENTRY_COUNT);
    wr32(sector, 84, GPT_ENTRY_BYTES);
    wr32(sector, 88, entries_crc);

    wr32(sector, 16, crc32(sector, 92));
}

bool gpt_write(struct block_device *dev, const struct partition_plan *parts,
               size_t count)
{
    if (!dev || count == 0 || count > GPT_ENTRY_COUNT)
        return false;
    if (dev->sector_count < GPT_FIRST_USABLE + GPT_TAIL_SECTORS + 1)
        return false;

    size_t table_bytes = (size_t)GPT_ENTRY_COUNT * GPT_ENTRY_BYTES;
    uint8_t *table = kmalloc(table_bytes);

    if (!table)
        return false;

    memset(table, 0, table_bytes);

    uint64_t last_lba = dev->sector_count - 1;
    uint64_t first_usable = GPT_FIRST_USABLE;
    uint64_t last_usable = last_lba - GPT_TAIL_SECTORS;

    for (size_t i = 0; i < count; i++) {
        const struct partition_plan *plan = &parts[i];
        uint64_t end = plan->start + plan->count - 1;

        if (plan->count == 0 || plan->start < first_usable ||
            end > last_usable) {
            kfree(table);
            return false;
        }

        uint8_t *entry = &table[i * GPT_ENTRY_BYTES];

        memcpy(entry, plan->efi ? GUID_EFI : GUID_MSDATA, 16);
        make_guid(&entry[16]);
        wr64(entry, 32, plan->start);
        wr64(entry, 40, end);
        wr64(entry, 48, 0);              /* keine besonderen Merkmale */
        put_name(entry, plan->name);
    }

    uint32_t entries_crc = crc32(table, table_bytes);
    uint8_t  disk_guid[16];

    make_guid(disk_guid);

    uint64_t primary_entries = 2;
    uint64_t backup_entries = last_lba - GPT_TABLE_SECTORS;

    uint8_t header[512];
    bool ok = true;

    /* Erst die Eintraege, dann die Koepfe: Waere es umgekehrt und der
     * Schreibvorgang braeche ab, verwiese ein gueltiger Kopf auf eine
     * Tabelle, die noch gar nicht dasteht. */
    for (uint32_t s = 0; s < GPT_TABLE_SECTORS && ok; s++) {
        ok = block_write(dev, primary_entries + s, 1,
                         table + (size_t)s * 512) &&
             block_write(dev, backup_entries + s, 1, table + (size_t)s * 512);
    }

    if (ok) {
        build_header(header, 1, last_lba, primary_entries, first_usable,
                     last_usable, disk_guid, entries_crc);
        ok = block_write(dev, 1, 1, header);
    }
    if (ok) {
        build_header(header, last_lba, 1, backup_entries, first_usable,
                     last_usable, disk_guid, entries_crc);
        ok = block_write(dev, last_lba, 1, header);
    }
    if (ok)
        ok = write_protective_mbr(dev);

    kfree(table);
    return ok;
}
