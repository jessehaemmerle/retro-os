/* boot.c - Schnittstelle zum Limine-Bootloader.
 *
 * Limine uebergibt RetroOS bereits im Long Mode mit aktivem Paging und
 * einem linearen Framebuffer. Hier werden die Antworten des Bootloaders
 * eingesammelt und in eine bootloader-unabhaengige Struktur uebersetzt,
 * damit der restliche Kernel nichts von Limine wissen muss.
 */

#include "boot.h"
#include "kstring.h"
#include "limine.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(2);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_address_request kaddr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

/* Der Bootloader haelt die geladene Programmdatei im Speicher fest -
 * also den Kernel selbst, Byte fuer Byte, so wie er auf dem Datentraeger
 * steht. Das Installationsprogramm schreibt genau diese Bytes auf die
 * Festplatte; es muss den Kernel also nicht ein zweites Mal mitfuehren. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_file_request self_request = {
    .id = LIMINE_KERNEL_FILE_REQUEST,
    .revision = 0,
};

/* Dasselbe fuer die Beigaben aus limine.conf: der Bootloader fuer UEFI
 * und sein Gegenstueck fuer BIOS. */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

uint64_t g_hhdm_offset;

static struct boot_info info;

/* Vergleicht nur den letzten Teil eines Pfades - der Bootloader nennt
 * ihn je nach Datentraeger unterschiedlich vollstaendig. */
static bool path_ends_with(const char *path, const char *name)
{
    if (!path || !name)
        return false;

    size_t plen = strlen(path);
    size_t nlen = strlen(name);

    if (nlen > plen)
        return false;
    if (plen > nlen && path[plen - nlen - 1] != '/' &&
        path[plen - nlen - 1] != '\\')
        return false;
    return strcasecmp(path + plen - nlen, name) == 0;
}

const void *boot_self_image(size_t *size)
{
    if (!self_request.response || !self_request.response->kernel_file)
        return NULL;

    struct limine_file *file = self_request.response->kernel_file;

    if (size)
        *size = (size_t)file->size;
    return file->address;
}

/* Von welchem Datentraeger wurde gestartet? Der Bootloader nennt die
 * Kennung aus der GUID-Tabelle bzw. die des MBR. Das
 * Installationsprogramm vergleicht damit, um sich nicht selbst den Boden
 * unter den Fuessen wegzuziehen. */
bool boot_volume_gpt_guid(uint8_t out[16])
{
    if (!self_request.response || !self_request.response->kernel_file)
        return false;

    struct limine_file *file = self_request.response->kernel_file;
    const uint8_t *guid = (const uint8_t *)&file->gpt_disk_uuid;
    bool empty = true;

    for (int i = 0; i < 16; i++)
        if (guid[i]) {
            empty = false;
            break;
        }
    if (empty)
        return false;

    memcpy(out, guid, 16);
    return true;
}

uint32_t boot_volume_mbr_id(void)
{
    if (!self_request.response || !self_request.response->kernel_file)
        return 0;
    return self_request.response->kernel_file->mbr_disk_id;
}

const void *boot_module(const char *name, size_t *size)
{
    if (!module_request.response)
        return NULL;

    for (uint64_t i = 0; i < module_request.response->module_count; i++) {
        struct limine_file *file = module_request.response->modules[i];

        if (!file || !path_ends_with(file->path, name))
            continue;
        if (size)
            *size = (size_t)file->size;
        return file->address;
    }
    return NULL;
}

static uint32_t translate_type(uint64_t limine_type)
{
    switch (limine_type) {
    case LIMINE_MEMMAP_USABLE:
        return BOOT_MEM_USABLE;
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
        return BOOT_MEM_RECLAIMABLE;
    default:
        return BOOT_MEM_RESERVED;
    }
}

bool boot_revision_ok(void)
{
    return LIMINE_BASE_REVISION_SUPPORTED;
}

void boot_collect(void)
{
    memset(&info, 0, sizeof(info));

    if (hhdm_request.response) {
        info.hhdm_offset = hhdm_request.response->offset;
        g_hhdm_offset    = info.hhdm_offset;
    }

    if (kaddr_request.response) {
        info.kernel_phys = kaddr_request.response->physical_base;
        info.kernel_virt = kaddr_request.response->virtual_base;
    }

    if (fb_request.response && fb_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = fb_request.response->framebuffers[0];

        info.fb_addr   = (uint64_t)fb->address;
        info.fb_width  = fb->width;
        info.fb_height = fb->height;
        info.fb_pitch  = fb->pitch;
        info.fb_bpp    = fb->bpp;
    }

    if (rsdp_request.response)
        info.rsdp = (uint64_t)rsdp_request.response->address;

    if (memmap_request.response) {
        uint64_t count = memmap_request.response->entry_count;

        for (uint64_t i = 0; i < count && info.memmap_count < BOOT_MAX_MEMMAP; i++) {
            struct limine_memmap_entry *e = memmap_request.response->entries[i];
            struct boot_mem_entry *dst = &info.memmap[info.memmap_count++];

            dst->base   = e->base;
            dst->length = e->length;
            dst->type   = translate_type(e->type);

            info.total_memory += e->length;
            if (dst->type == BOOT_MEM_USABLE)
                info.usable_memory += e->length;
        }
    }
}

const struct boot_info *boot_info(void)
{
    return &info;
}
