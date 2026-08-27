/* setup.h - RetroOS auf eine Festplatte bringen.
 *
 * Bis hierher lief RetroOS immer nur von dem Datentraeger, von dem es
 * gestartet wurde. Das Installationsprogramm macht daraus ein System,
 * das auf einer Platte liegt und dort auch bleibt: Es legt eine
 * GUID-Tabelle mit zwei Abschnitten an, formatiert beide mit FAT32,
 * schreibt Kernel und Bootloader in den EFI-Abschnitt und setzt fuer
 * aeltere Rechner zusaetzlich den Startsektor.
 *
 * Der zweite Abschnitt ist die Ablage. Er wird beim naechsten Start
 * unter /Festplatte eingehaengt - dort bleiben Dateien liegen.
 */
#ifndef SETUP_H
#define SETUP_H

#include "retro.h"
#include "block.h"

/* Der EFI-Abschnitt fasst Kernel und Bootloader; vierundsechzig
 * Megabyte sind reichlich und heute die uebliche Groesse. */
#define SETUP_ESP_SECTORS  131072

/* FAT32 verlangt mindestens 65525 Cluster. Mit einem Sektor je Cluster
 * sind das gut zweiunddreissig Megabyte; mit etwas Luft davor ist
 * fuenfunddreissig eine sichere Untergrenze. */
#define SETUP_MIN_FAT      71680

struct setup_plan {
    struct block_device *dev;
    uint64_t esp_start,  esp_count;
    uint64_t data_start, data_count;
};

/* Hat der Bootloader Kernel und Bootloaderdateien mitgebracht? Ohne sie
 * laesst sich nichts installieren - dann laeuft RetroOS aus einer
 * Quelle, die das nicht vorgesehen hat. */
bool setup_sources_ready(void);

/* Laeuft das System gerade von diesem Traeger? Dann darf er nicht
 * ueberschrieben werden. */
bool setup_is_boot_disk(struct block_device *dev);

/* Teilt den Traeger ein. Passt nichts, steht der Grund in why. */
bool setup_plan_for(struct block_device *dev, struct setup_plan *out,
                    char *why, size_t size);

typedef void (*setup_report_fn)(void *user, int percent, const char *text);

/* Fuehrt die Installation aus. Alles, was auf dem Traeger stand, ist
 * danach verloren. */
bool setup_run(const struct setup_plan *plan, setup_report_fn report,
               void *user, char *error, size_t size);

#endif /* SETUP_H */
