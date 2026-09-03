/* vbe.c - Moduswechsel ueber die Bochs-Schnittstelle. */

#include "vbe.h"
#include "io.h"
#include "log.h"
#include "pci.h"

#define DISPI_INDEX  0x01CE
#define DISPI_DATA   0x01CF

#define DISPI_ID          0
#define DISPI_XRES        1
#define DISPI_YRES        2
#define DISPI_BPP         3
#define DISPI_ENABLE      4
#define DISPI_VIRT_WIDTH  6
#define DISPI_VIRT_HEIGHT 7
#define DISPI_X_OFFSET    8
#define DISPI_Y_OFFSET    9

#define ENABLE_DISABLED 0x00
#define ENABLE_ENABLED  0x01
#define ENABLE_LFB      0x40

static bool     available;
static uint64_t vram;
static uint64_t lfb;

static void write_reg(uint16_t index, uint16_t value)
{
    outw(DISPI_INDEX, index);
    outw(DISPI_DATA, value);
}

static uint16_t read_reg(uint16_t index)
{
    outw(DISPI_INDEX, index);
    return inw(DISPI_DATA);
}

bool vbe_available(void)      { return available; }
uint64_t vbe_vram_bytes(void) { return vram; }
uint64_t vbe_framebuffer(void){ return lfb; }

bool vbe_init(void)
{
    available = false;
    vram = 0;
    lfb = 0;

    /* Die Kennung liegt zwischen 0xB0C0 und 0xB0C5; alles andere ist
     * keine Karte, die das hier versteht. Ohne Geraet liest man an
     * einem toten Port lauter Einsen, und auch das faellt damit auf. */
    uint16_t id = read_reg(DISPI_ID);

    if (id < 0xB0C0 || id > 0xB0C5)
        return false;

    /* Der lineare Speicher steht in BAR 0 der Grafikkarte, und seine
     * Groesse sagt, welche Modi ueberhaupt hineinpassen. Gesucht wird
     * ueber die Klasse, denn die Kennungen unterscheiden sich je nach
     * Emulator: QEMU meldet 1234:1111, VirtualBox 80EE:BEEF, Bochs
     * wieder etwas anderes. */
    const struct pci_device *gpu = pci_find_class(0x03, 0x00);

    if (!gpu)
        gpu = pci_find_class(0x03, 0x80);

    if (gpu && gpu->bar[0] && !gpu->bar_is_io[0]) {
        lfb = gpu->bar[0];
        vram = gpu->bar_size[0];
    }

    available = true;
    log_info("grafik", "Bochs-Schnittstelle 0x%04x, %u MiB Grafikspeicher",
             id, (unsigned)(vram / (1024 * 1024)));
    return true;
}

bool vbe_set_mode(uint32_t width, uint32_t height, uint64_t *pitch_bytes)
{
    if (!available || width == 0 || height == 0)
        return false;

    /* Abschalten, einstellen, wieder einschalten - in dieser
     * Reihenfolge, sonst uebernimmt die Karte die halbe Aenderung. */
    write_reg(DISPI_ENABLE, ENABLE_DISABLED);
    write_reg(DISPI_XRES, (uint16_t)width);
    write_reg(DISPI_YRES, (uint16_t)height);
    write_reg(DISPI_BPP, 32);
    write_reg(DISPI_VIRT_WIDTH, (uint16_t)width);
    write_reg(DISPI_X_OFFSET, 0);
    write_reg(DISPI_Y_OFFSET, 0);
    write_reg(DISPI_ENABLE, ENABLE_ENABLED | ENABLE_LFB);

    /* Nachsehen, was daraus geworden ist: Eine Karte, die den Modus
     * nicht kann, setzt einen anderen und meldet ihn hier. Wer das
     * nicht prueft, zeichnet danach an der falschen Stelle. */
    uint32_t got_w = read_reg(DISPI_XRES);
    uint32_t got_h = read_reg(DISPI_YRES);
    uint32_t got_bpp = read_reg(DISPI_BPP);

    if (got_w != width || got_h != height || got_bpp != 32) {
        log_warn("grafik", "%ux%u abgelehnt, Karte meldet %ux%ux%u",
                 (unsigned)width, (unsigned)height,
                 (unsigned)got_w, (unsigned)got_h, (unsigned)got_bpp);
        return false;
    }

    /* Die virtuelle Breite ist die Zeilenlaenge in Punkten; manche
     * Karten runden sie auf. */
    uint32_t virt = read_reg(DISPI_VIRT_WIDTH);

    if (virt < width)
        virt = width;
    if (pitch_bytes)
        *pitch_bytes = (uint64_t)virt * 4u;
    return true;
}
