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

#endif /* PARTITION_H */
