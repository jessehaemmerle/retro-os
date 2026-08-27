/* setup.c - RetroOS auf eine Festplatte bringen.
 *
 * Was dabei entsteht, ist der uebliche Aufbau eines heutigen Rechners:
 *
 *   Sektor 0        Schutzeintrag, damit alte Werkzeuge stillhalten
 *   Sektor 1-33     die GUID-Tabelle
 *   Sektor 34-...   der zweite Teil des Bootloaders fuer BIOS-Rechner
 *   Sektor 2048     EFI-Abschnitt: Bootloader, Kernel, Konfiguration
 *   danach          Ablage: alles, was der Benutzer anlegt
 *
 * Ein UEFI-Rechner braucht nur den EFI-Abschnitt: Er sucht dort von
 * selbst nach \EFI\BOOT\BOOTX64.EFI. Ein BIOS-Rechner liest stattdessen
 * den ersten Sektor; deshalb wird dort zusaetzlich Limines Startcode
 * abgelegt, der seinen zweiten Teil in der Luecke dahinter findet.
 * Beide Wege fuehren zu derselben Datei, und der Benutzer merkt nichts
 * davon, welchen sein Rechner genommen hat.
 */

#include "setup.h"
#include "boot.h"
#include "fat.h"
#include "kstring.h"
#include "partition.h"
#include "vfs.h"

/* Limines Startcode: erste 512 Byte in den Startsektor, der Rest in die
 * Luecke dahinter. Die Bytes stecken fest im Kernelabbild. */
extern const uint8_t _binary_limine_bios_hdd_bin_start[];
extern const uint8_t _binary_limine_bios_hdd_bin_end[];

/* Der zweite Teil beginnt gleich hinter der Tabelle. */
#define STAGE2_LBA  (2 + GPT_TABLE_SECTORS)

static const char LIMINE_CONF[] =
    "# Von RetroOS bei der Installation angelegt.\n"
    "timeout: 3\n"
    "verbose: no\n"
    "\n"
    "/RetroOS\n"
    "    protocol: limine\n"
    "    path: boot():/boot/retroos.elf\n"
    "    module_path: boot():/EFI/BOOT/BOOTX64.EFI\n"
    "    module_path: boot():/boot/limine-bios.sys\n";

/* ------------------------------------------------------------------ */
/* Voraussetzungen                                                     */
/* ------------------------------------------------------------------ */

static const void *source_kernel(size_t *size)
{
    return boot_self_image(size);
}

static const void *source_efi(size_t *size)
{
    return boot_module("BOOTX64.EFI", size);
}

static const void *source_bios(size_t *size)
{
    return boot_module("limine-bios.sys", size);
}

bool setup_sources_ready(void)
{
    size_t a = 0, b = 0, c = 0;

    return source_kernel(&a) && a > 0 &&
           source_efi(&b) && b > 0 &&
           source_bios(&c) && c > 0;
}

bool setup_is_boot_disk(struct block_device *dev)
{
    if (!dev)
        return false;

    uint8_t sector[512];
    uint8_t guid[16];

    /* Traegt der Traeger eine GUID-Tabelle, entscheidet deren Kennung. */
    if (boot_volume_gpt_guid(guid) && block_read(dev, 1, 1, sector) &&
        memcmp(sector, "EFI PART", 8) == 0) {
        if (memcmp(&sector[56], guid, 16) == 0)
            return true;
    }

    uint32_t mbr_id = boot_volume_mbr_id();

    if (mbr_id && block_read(dev, 0, 1, sector)) {
        uint32_t on_disk = (uint32_t)sector[440] |
                           ((uint32_t)sector[441] << 8) |
                           ((uint32_t)sector[442] << 16) |
                           ((uint32_t)sector[443] << 24);

        if (on_disk == mbr_id)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Einteilung                                                          */
/* ------------------------------------------------------------------ */

bool setup_plan_for(struct block_device *dev, struct setup_plan *out,
                    char *why, size_t size)
{
    if (!dev || !out) {
        if (why) strlcpy(why, "Kein Datentraeger angegeben.", size);
        return false;
    }
    if (!dev->write) {
        if (why) strlcpy(why, "Der Datentraeger laesst sich nicht "
                              "beschreiben.", size);
        return false;
    }
    if (setup_is_boot_disk(dev)) {
        if (why) strlcpy(why, "Von diesem Datentraeger laeuft RetroOS "
                              "gerade.", size);
        return false;
    }

    uint64_t usable = dev->sector_count;

    if (usable <= GPT_FIRST_USABLE + GPT_TAIL_SECTORS) {
        if (why) strlcpy(why, "Der Datentraeger ist zu klein.", size);
        return false;
    }

    uint64_t room = usable - GPT_FIRST_USABLE - GPT_TAIL_SECTORS;
    uint64_t esp = SETUP_ESP_SECTORS;

    /* Auf kleinen Platten faellt der EFI-Abschnitt kleiner aus - aber
     * nie unter das, was FAT32 noch traegt. */
    if (room < esp + SETUP_MIN_FAT)
        esp = SETUP_MIN_FAT;

    if (room < esp + SETUP_MIN_FAT) {
        if (why)
            ksnprintf(why, size,
                      "Der Datentraeger fasst nur %u MiB; noetig sind %u MiB.",
                      (unsigned)(usable / 2048),
                      (unsigned)((GPT_FIRST_USABLE + 2 * SETUP_MIN_FAT +
                                  GPT_TAIL_SECTORS) / 2048 + 1));
        return false;
    }

    /* Der zweite Abschnitt faengt an einer Megabyte-Grenze an. */
    uint64_t data_start = ALIGN_UP(GPT_FIRST_USABLE + esp, 2048);
    uint64_t last = usable - GPT_TAIL_SECTORS - 1;

    if (data_start > last || last - data_start + 1 < SETUP_MIN_FAT) {
        if (why) strlcpy(why, "Fuer die Ablage bleibt zu wenig Platz.", size);
        return false;
    }

    out->dev        = dev;
    out->esp_start  = GPT_FIRST_USABLE;
    out->esp_count  = data_start - GPT_FIRST_USABLE;
    out->data_start = data_start;
    out->data_count = last - data_start + 1;
    return true;
}

/* ------------------------------------------------------------------ */
/* Dateien in den EFI-Abschnitt legen                                  */
/* ------------------------------------------------------------------ */

static bool make_dir(struct fat_volume *vol, uint32_t parent,
                     const char *name, uint32_t *cluster)
{
    struct fat_dirent entry;

    if (!fat_create(vol, parent, name, true, &entry))
        return false;
    *cluster = entry.first_cluster;
    return true;
}

static bool put_file(struct fat_volume *vol, uint32_t dir, const char *name,
                     const void *data, size_t size)
{
    struct fat_dirent entry;
    uint32_t first = 0;

    if (!fat_create(vol, dir, name, false, &entry))
        return false;
    return fat_write_file(vol, &entry.ref, &first, data, (uint32_t)size);
}

/* ------------------------------------------------------------------ */
/* Startsektor fuer BIOS-Rechner                                       */
/* ------------------------------------------------------------------ */

static void wr16(uint8_t *p, size_t at, uint16_t value)
{
    p[at] = (uint8_t)value;
    p[at + 1] = (uint8_t)(value >> 8);
}

static void wr64(uint8_t *p, size_t at, uint64_t value)
{
    for (int i = 0; i < 8; i++)
        p[at + i] = (uint8_t)(value >> (8 * i));
}

/* Schreibt beliebig viele Bytes ab einem Sektor; der letzte Sektor wird
 * mit Nullen aufgefuellt. */
static bool write_bytes(struct block_device *dev, uint64_t lba,
                        const uint8_t *data, size_t bytes)
{
    uint8_t sector[512];

    for (uint64_t at = 0; bytes > 0; at++) {
        size_t take = MIN(bytes, (size_t)512);

        memset(sector, 0, sizeof(sector));
        memcpy(sector, data, take);
        if (!block_write(dev, lba + at, 1, sector))
            return false;
        data += take;
        bytes -= take;
    }
    return true;
}

/* Limines Startcode besteht aus zwei Stuecken: den 512 Byte, die das
 * BIOS laedt, und einem Rest, den jene 512 Byte nachladen. Wo der Rest
 * liegt, steht fest im Startsektor - an Versatz 0x1a4. Wir legen ihn in
 * die Luecke zwischen Tabelle und erstem Abschnitt; dort ist Platz und
 * niemand sonst raeumt darin herum. */
static bool install_bios_boot(struct block_device *dev)
{
    const uint8_t *image = _binary_limine_bios_hdd_bin_start;
    size_t total = (size_t)(_binary_limine_bios_hdd_bin_end - image);

    if (total <= 512)
        return false;

    size_t stage2 = total - 512;
    size_t sectors = (stage2 + 511) / 512;
    uint16_t size_a = (uint16_t)((sectors / 2 + (sectors % 2)) * 512);
    uint16_t size_b = (uint16_t)((sectors / 2) * 512);
    uint64_t loc_a = (uint64_t)STAGE2_LBA * 512;
    uint64_t loc_b = loc_a + size_a;

    /* Alles muss vor dem ersten Abschnitt bleiben. */
    if (loc_b + size_b > (uint64_t)GPT_FIRST_USABLE * 512)
        return false;

    if (!write_bytes(dev, loc_a / 512, image + 512, size_a))
        return false;
    if (!write_bytes(dev, loc_b / 512, image + 512 + size_a, stage2 - size_a))
        return false;

    /* Der Startsektor traegt schon den Schutzeintrag der Tabelle. Der
     * Startcode darf ihn nicht mitnehmen, also wird er danach wieder
     * eingesetzt - genau wie es Limines eigenes Werkzeug tut. */
    uint8_t current[512];
    uint8_t sector[512];

    if (!block_read(dev, 0, 1, current))
        return false;

    memcpy(sector, image, 512);
    memcpy(&sector[218], &current[218], 6);     /* Zeitstempel        */
    memcpy(&sector[440], &current[440], 70);    /* Kennung und Tabelle */

    wr16(sector, 0x1a4 + 0, size_a);
    wr16(sector, 0x1a4 + 2, size_b);
    wr64(sector, 0x1a4 + 4, loc_a);
    wr64(sector, 0x1a4 + 12, loc_b);

    return block_write(dev, 0, 1, sector);
}

/* ------------------------------------------------------------------ */
/* Der Ablauf                                                          */
/* ------------------------------------------------------------------ */

static void say(setup_report_fn report, void *user, int percent,
                const char *text)
{
    if (report)
        report(user, percent, text);
}

static bool fail(char *error, size_t size, const char *text)
{
    if (error)
        strlcpy(error, text, size);
    return false;
}

bool setup_run(const struct setup_plan *plan, setup_report_fn report,
               void *user, char *error, size_t size)
{
    if (!plan || !plan->dev)
        return fail(error, size, "Kein Ziel angegeben.");

    size_t kernel_size = 0, efi_size = 0, bios_size = 0;
    const void *kernel = source_kernel(&kernel_size);
    const void *efi    = source_efi(&efi_size);
    const void *bios   = source_bios(&bios_size);

    if (!kernel || !efi || !bios)
        return fail(error, size,
                    "Der Bootloader hat die noetigen Dateien nicht "
                    "mitgebracht.");
    if (setup_is_boot_disk(plan->dev))
        return fail(error, size,
                    "Von diesem Datentraeger laeuft RetroOS gerade.");

    /* Ist die Zielplatte gerade eingehaengt, muss der Baum sie zuerst
     * vergessen - gleich zeigen alle gemerkten Cluster ins Leere. */
    fs_detach_disk();

    say(report, user, 5, "Partitionstabelle wird angelegt ...");

    struct partition_plan parts[2] = {
        { plan->esp_start,  plan->esp_count,  true,  "EFI"     },
        { plan->data_start, plan->data_count, false, "RetroOS" },
    };

    if (!gpt_write(plan->dev, parts, 2))
        return fail(error, size, "Die Partitionstabelle liess sich nicht "
                                 "schreiben.");

    say(report, user, 15, "EFI-Abschnitt wird formatiert ...");
    if (!fat_format_at(plan->dev, plan->esp_start, plan->esp_count, "EFI"))
        return fail(error, size, "Der EFI-Abschnitt liess sich nicht "
                                 "formatieren.");

    say(report, user, 30, "Ablage wird formatiert ...");
    if (!fat_format_at(plan->dev, plan->data_start, plan->data_count,
                       "RETROOS"))
        return fail(error, size, "Die Ablage liess sich nicht formatieren.");

    say(report, user, 40, "Bootloader wird kopiert ...");

    struct fat_volume esp_vol;

    if (!fat_mount_at(plan->dev, plan->esp_start, &esp_vol))
        return fail(error, size, "Der frische EFI-Abschnitt laesst sich "
                                 "nicht lesen.");

    uint32_t root = esp_vol.root_cluster;
    uint32_t dir_efi = 0, dir_boot_efi = 0, dir_boot = 0;

    if (!make_dir(&esp_vol, root, "EFI", &dir_efi) ||
        !make_dir(&esp_vol, dir_efi, "BOOT", &dir_boot_efi) ||
        !make_dir(&esp_vol, root, "boot", &dir_boot))
        return fail(error, size, "Die Verzeichnisse liessen sich nicht "
                                 "anlegen.");

    if (!put_file(&esp_vol, dir_boot_efi, "BOOTX64.EFI", efi, efi_size))
        return fail(error, size, "BOOTX64.EFI liess sich nicht schreiben.");

    say(report, user, 55, "Kernel wird kopiert ...");
    if (!put_file(&esp_vol, dir_boot, "retroos.elf", kernel, kernel_size))
        return fail(error, size, "Der Kernel liess sich nicht schreiben.");

    say(report, user, 80, "Konfiguration wird geschrieben ...");
    if (!put_file(&esp_vol, dir_boot, "limine-bios.sys", bios, bios_size))
        return fail(error, size, "limine-bios.sys liess sich nicht "
                                 "schreiben.");
    if (!put_file(&esp_vol, dir_boot, "limine.conf", LIMINE_CONF,
                  sizeof(LIMINE_CONF) - 1))
        return fail(error, size, "limine.conf liess sich nicht schreiben.");

    say(report, user, 90, "Startsektor wird gesetzt ...");
    if (!install_bios_boot(plan->dev))
        return fail(error, size, "Der Startsektor liess sich nicht "
                                 "schreiben. Ueber UEFI startet das System "
                                 "trotzdem.");

    say(report, user, 100, "Fertig.");
    kprintf("Installation: %s eingerichtet - EFI ab Sektor %llu, "
            "Ablage ab Sektor %llu\n", plan->dev->name,
            (unsigned long long)plan->esp_start,
            (unsigned long long)plan->data_start);

    /* Die Ablage gleich einhaengen, damit sie sofort benutzbar ist. */
    fs_mount_disk();
    return true;
}
