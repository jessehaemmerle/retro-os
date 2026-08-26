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
static volatile struct limine_kernel_address_request kaddr_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

uint64_t g_hhdm_offset;

static struct boot_info info;

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
