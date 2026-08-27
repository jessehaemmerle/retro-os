/* partition.h - Partitionstabellen lesen: MBR und GPT.
 *
 * Ein moderner Datentraeger traegt seine Aufteilung in einer GUID-Tabelle,
 * ein aelterer im Startsektor. RetroOS liest beides und liefert die
 * gefundenen Abschnitte in einer einheitlichen Liste.
 */
#ifndef PARTITION_H
#define PARTITION_H

#include "retro.h"
#include "block.h"

#define PARTITION_MAX 16

enum partition_scheme {
    SCHEME_NONE,        /* keine Tabelle - der ganze Traeger */
    SCHEME_MBR,
    SCHEME_GPT,
};

struct partition {
    uint64_t start;             /* erster Sektor            */
    uint64_t count;             /* Anzahl Sektoren          */
    uint8_t  mbr_type;          /* Kennung im MBR, sonst 0  */
    bool     is_fat;            /* sieht nach FAT aus       */
    bool     is_efi;            /* EFI-Systempartition      */
    char     name[40];          /* Name aus der GPT         */
};

/* Liest die Tabelle des Traegers. Gibt die Anzahl der Eintraege zurueck
 * und traegt das erkannte Verfahren in *scheme ein. */
size_t partition_scan(struct block_device *dev, struct partition *out,
                      size_t max, enum partition_scheme *scheme);

const char *partition_scheme_name(enum partition_scheme scheme);

/* --- Eine neue Tabelle anlegen --- */

#define GPT_ENTRY_COUNT     128
#define GPT_ENTRY_BYTES     128
#define GPT_TABLE_SECTORS   32       /* 128 Eintraege zu je 128 Byte */

/* Der erste Abschnitt beginnt bei einem Megabyte. Das ist heute ueblich
 * (SSDs schreiben blockweise) und laesst zugleich Platz fuer den zweiten
 * Teil des Bootloaders, den ein BIOS-Rechner dort erwartet. */
#define GPT_FIRST_USABLE    2048
#define GPT_TAIL_SECTORS    33       /* Sicherungstabelle am Ende     */

struct partition_plan {
    uint64_t    start;               /* erster Sektor              */
    uint64_t    count;               /* Anzahl Sektoren            */
    bool        efi;                 /* EFI-Systemabschnitt?       */
    const char *name;                /* hoechstens 35 Zeichen      */
};

/* Legt eine GUID-Tabelle mit den angegebenen Abschnitten an: Schutz-MBR
 * im ersten Sektor, Tabelle vorn, Sicherung hinten. Was vorher auf dem
 * Traeger stand, ist danach nicht mehr auffindbar. */
bool gpt_write(struct block_device *dev, const struct partition_plan *parts,
               size_t count);

/* --- Eine vorhandene Tabelle ergaenzen --- */

/* Traegt der Datentraeger bereits eine gueltige GUID-Tabelle? */
bool gpt_present(struct block_device *dev);

/* Sucht die groesste zusammenhaengende Luecke zwischen den Abschnitten.
 * Gibt false zurueck, wenn keine da ist. */
bool gpt_largest_gap(struct block_device *dev, uint64_t *start,
                     uint64_t *count);

/* Sucht die EFI-Systempartition. */
bool gpt_find_esp(struct block_device *dev, uint64_t *start, uint64_t *count);

/* Traegt einen weiteren Abschnitt in die vorhandene Tabelle ein und
 * berichtigt die Pruefsummen beider Koepfe. Die uebrigen Eintraege
 * bleiben, wie sie sind. */
bool gpt_add_partition(struct block_device *dev,
                       const struct partition_plan *plan);

#endif /* PARTITION_H */
