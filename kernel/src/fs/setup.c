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

/* Neben einem anderen System liegt alles in EFI\\RETROOS; dort sucht
 * Limine auch seine Konfiguration und findet den Kernel daneben. */
static const char LIMINE_CONF_BESIDE[] =
    "# Von RetroOS bei der Installation angelegt.\n"
    "timeout: 3\n"
    "verbose: no\n"
    "\n"
    "/RetroOS\n"
    "    protocol: limine\n"
    "    path: boot():/EFI/RETROOS/retroos.elf\n";

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
    out->mode       = SETUP_WHOLE_DISK;
    out->esp_start  = GPT_FIRST_USABLE;
    out->esp_count  = data_start - GPT_FIRST_USABLE;
    out->data_start = data_start;
    out->data_count = last - data_start + 1;
    return true;
}

bool setup_plan_beside(struct block_device *dev, struct setup_plan *out,
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
    if (!gpt_present(dev)) {
        if (why) strlcpy(why, "Ohne GUID-Tabelle gibt es nichts, wobei man "
                              "sich einnisten koennte.", size);
        return false;
    }

    uint64_t esp_start = 0, esp_count = 0;

    if (!gpt_find_esp(dev, &esp_start, &esp_count)) {
        if (why) strlcpy(why, "Es gibt keine EFI-Systempartition.", size);
        return false;
    }

    uint64_t gap_start = 0, gap_count = 0;

    if (!gpt_largest_gap(dev, &gap_start, &gap_count) ||
        gap_count < SETUP_MIN_FAT) {
        if (why)
            ksnprintf(why, size,
                      "Zu wenig freier Platz: %u MiB, noetig sind %u MiB.",
                      (unsigned)(gap_count / 2048),
                      (unsigned)(SETUP_MIN_FAT / 2048 + 1));
        return false;
    }

    out->dev        = dev;
    out->mode       = SETUP_BESIDE;
    out->esp_start  = esp_start;
    out->esp_count  = esp_count;
    out->data_start = gap_start;
    out->data_count = gap_count;
    out->fallback_free = false;
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

/* Sucht ein Verzeichnis; legt es an, wenn es fehlt. */
struct find_state {
    const char *name;
    bool        found;
    bool        is_dir;
    uint32_t    cluster;
    struct fat_entry_ref ref;
};

static void find_cb(void *user, const struct fat_dirent *entry)
{
    struct find_state *st = user;

    if (st->found || strcasecmp(entry->name, st->name) != 0)
        return;

    st->found = true;
    st->is_dir = entry->is_dir;
    st->cluster = entry->first_cluster;
    st->ref = entry->ref;
}

static bool open_dir(struct fat_volume *vol, uint32_t parent,
                     const char *name, uint32_t *cluster)
{
    struct find_state st = { .name = name, .found = false };

    fat_list_dir(vol, parent, find_cb, &st);

    if (st.found) {
        if (!st.is_dir)
            return false;
        *cluster = st.cluster;
        return true;
    }
    return make_dir(vol, parent, name, cluster);
}

/* Ist der Name schon vergeben? */
static bool name_taken(struct fat_volume *vol, uint32_t dir, const char *name)
{
    struct find_state st = { .name = name, .found = false };

    fat_list_dir(vol, dir, find_cb, &st);
    return st.found;
}

/* Legt eine Datei an oder ueberschreibt eine vorhandene. */
static bool replace_file(struct fat_volume *vol, uint32_t dir,
                         const char *name, const void *data, size_t size)
{
    struct find_state st = { .name = name, .found = false };

    fat_list_dir(vol, dir, find_cb, &st);

    if (st.found) {
        if (st.is_dir)
            return false;

        uint32_t first = st.cluster;

        return fat_write_file(vol, &st.ref, &first, data, (uint32_t)size);
    }

    struct fat_dirent entry;
    uint32_t first = 0;

    if (!fat_create(vol, dir, name, false, &entry))
        return false;
    return fat_write_file(vol, &entry.ref, &first, data, (uint32_t)size);
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

/* Die Dateien in einen schon vorhandenen EFI-Abschnitt legen, ohne dem
 * zu nahe zu treten, was dort steht. RetroOS bekommt ein eigenes
 * Verzeichnis; den ueblichen Startpfad belegt es nur, wenn er frei ist.
 * Sonst muss der Benutzer im Startmenue der Firmware auswaehlen - das
 * ist unbequem, aber besser, als ein fremdes System unstartbar zu
 * machen. */
static bool populate_shared_esp(struct setup_plan *plan,
                                struct fat_volume *esp,
                                const void *kernel, size_t kernel_size,
                                const void *efi, size_t efi_size,
                                const void *bios, size_t bios_size,
                                char *error, size_t size)
{
    UNUSED(bios);
    UNUSED(bios_size);

    uint32_t root = esp->root_cluster;
    uint32_t dir_efi = 0, dir_own = 0;

    if (!open_dir(esp, root, "EFI", &dir_efi))
        return fail(error, size, "Im EFI-Abschnitt fehlt der Ordner EFI.");
    if (!open_dir(esp, dir_efi, "RETROOS", &dir_own))
        return fail(error, size, "Der Ordner EFI\\RETROOS liess sich nicht "
                                 "anlegen.");

    if (!replace_file(esp, dir_own, "BOOTX64.EFI", efi, efi_size))
        return fail(error, size, "BOOTX64.EFI liess sich nicht schreiben.");
    if (!replace_file(esp, dir_own, "retroos.elf", kernel, kernel_size))
        return fail(error, size, "Der Kernel liess sich nicht schreiben.");
    if (!replace_file(esp, dir_own, "limine.conf", LIMINE_CONF_BESIDE,
                      sizeof(LIMINE_CONF_BESIDE) - 1))
        return fail(error, size, "limine.conf liess sich nicht schreiben.");

    /* Den ueblichen Startpfad nur belegen, wenn dort nichts steht. */
    uint32_t dir_boot = 0;

    plan->fallback_free = false;
    if (open_dir(esp, dir_efi, "BOOT", &dir_boot) &&
        (!name_taken(esp, dir_boot, "BOOTX64.EFI") ||
         name_taken(esp, dir_boot, "limine.conf"))) {
        /* Frei - oder es steht schon unser eigener Bootloader dort,
         * weil RetroOS hier bereits einmal installiert wurde. */
        if (replace_file(esp, dir_boot, "BOOTX64.EFI", efi, efi_size) &&
            replace_file(esp, dir_boot, "limine.conf", LIMINE_CONF_BESIDE,
                         sizeof(LIMINE_CONF_BESIDE) - 1))
            plan->fallback_free = true;
    }
    return true;
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
    block_cache_drop(plan->dev);

    /* Der Weg daneben laesst die Tabelle und alles Vorhandene stehen -
     * er traegt nur einen Abschnitt nach und legt die Dateien in den
     * EFI-Abschnitt, der schon da ist. */
    if (plan->mode == SETUP_BESIDE) {
        struct setup_plan work = *plan;

        say(report, user, 10, "Abschnitt wird eingetragen ...");

        struct partition_plan added = {
            work.data_start, work.data_count, false, "RetroOS"
        };

        if (!gpt_add_partition(work.dev, &added))
            return fail(error, size,
                        "Der Abschnitt liess sich nicht eintragen.");

        say(report, user, 30, "Ablage wird formatiert ...");
        if (!fat_format_at(work.dev, work.data_start, work.data_count,
                           "RETROOS"))
            return fail(error, size, "Die Ablage liess sich nicht "
                                     "formatieren.");

        say(report, user, 50, "Bootloader und Kernel werden kopiert ...");

        struct fat_volume esp_vol;

        if (!fat_mount_at(work.dev, work.esp_start, &esp_vol))
            return fail(error, size,
                        "Der EFI-Abschnitt laesst sich nicht lesen.");

        if (!populate_shared_esp(&work, &esp_vol, kernel, kernel_size,
                                 efi, efi_size, bios, bios_size,
                                 error, size))
            return false;

        say(report, user, 90, "Wird abgeschlossen ...");
        if (!block_flush(work.dev))
            return fail(error, size,
                        "Die Platte hat die Daten nicht angenommen.");

        say(report, user, 100, "Fertig.");
        kprintf("Installation: %s daneben eingerichtet - Ablage ab Sektor "
                "%llu, Start %s\n", work.dev->name,
                (unsigned long long)work.data_start,
                work.fallback_free ? "ueber den ueblichen Pfad"
                                   : "nur ueber das Startmenue");

        /* Das Ergebnis gehoert zurueck an den Aufrufer. */
        ((struct setup_plan *)plan)->fallback_free = work.fallback_free;

        fs_mount_disk();
        return true;
    }

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

    /* Alles, was noch im Puffer steht, muss jetzt auf die Platte -
     * gleich soll der Rechner davon starten. */
    if (!block_flush(plan->dev))
        return fail(error, size, "Die Platte hat die Daten nicht angenommen.");

    say(report, user, 100, "Fertig.");
    kprintf("Installation: %s eingerichtet - EFI ab Sektor %llu, "
            "Ablage ab Sektor %llu\n", plan->dev->name,
            (unsigned long long)plan->esp_start,
            (unsigned long long)plan->data_start);

    /* Die Ablage gleich einhaengen, damit sie sofort benutzbar ist. */
    fs_mount_disk();
    return true;
}
