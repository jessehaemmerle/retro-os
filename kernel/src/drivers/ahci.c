/* ahci.c - Treiber fuer SATA-Controller im AHCI-Modus.
 *
 * AHCI arbeitet nicht mit Ports, sondern mit Speicherstrukturen: eine
 * Befehlsliste, eine Empfangsflaeche fuer Antwortrahmen und je Befehl eine
 * Tabelle mit den Zieladressen der Daten. Der Controller holt sich alles
 * selbst per DMA - der Kernel setzt nur ein Bit und wartet.
 *
 * Uebertragen wird ueber einen festen Zwischenpuffer. Das kostet eine
 * Kopie, erspart aber die Frage, ob ein beliebiger Aufrufer-Puffer
 * zusammenhaengend im physischen Speicher liegt.
 */

#include "block.h"
#include "arch.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"

#define AHCI_MAX_PORTS   32
#define AHCI_MAX_DRIVES  2
#define BOUNCE_SECTORS   64
#define CMD_TIMEOUT_MS   3000

/* --- Registersatz des Controllers --- */
struct hba_port {
    volatile uint32_t clb, clbu, fb, fbu;
    volatile uint32_t is, ie, cmd, reserved0;
    volatile uint32_t tfd, sig, ssts, sctl, serr, sact, ci, sntf;
    volatile uint32_t fbs;
    volatile uint32_t reserved1[11];
    volatile uint32_t vendor[4];
} PACKED;

struct hba_mem {
    volatile uint32_t cap, ghc, is, pi, vs;
    volatile uint32_t ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
    volatile uint8_t  reserved[0xA0 - 0x2C];
    volatile uint8_t  vendor[0x100 - 0xA0];
    struct hba_port   ports[AHCI_MAX_PORTS];
} PACKED;

struct hba_cmd_header {
    uint8_t  flags_low;      /* cfl (Bit 0-4), a, w, p */
    uint8_t  flags_high;     /* r, b, c, pmp */
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t reserved[4];
} PACKED;

struct hba_prdt_entry {
    uint32_t dba, dbau;
    uint32_t reserved;
    uint32_t dbc_flags;      /* Bit 0-21 Bytezahl-1, Bit 31 Interrupt */
} PACKED;

struct hba_cmd_table {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    struct hba_prdt_entry prdt[8];
} PACKED;

struct fis_reg_h2d {
    uint8_t  fis_type;       /* 0x27 */
    uint8_t  pmport_flags;   /* Bit 7 = Befehl statt Kontrolle */
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0, lba1, lba2, device;
    uint8_t  lba3, lba4, lba5, featureh;
    uint8_t  countl, counth, icc, control;
    uint8_t  reserved[4];
} PACKED;

/* --- Zustand je Laufwerk --- */
struct ahci_drive {
    struct block_device  block;
    struct hba_port     *port;

    struct hba_cmd_header *cmd_list;   /* virtuell */
    struct hba_cmd_table  *cmd_table;
    uint64_t               cmd_list_phys;
    uint64_t               fis_phys;
    uint64_t               cmd_table_phys;

    uint8_t  *bounce;
    uint64_t  bounce_phys;
};

static struct ahci_drive drives[AHCI_MAX_DRIVES];
static size_t            drive_count;

/* --- Portsteuerung --- */

static void port_stop(struct hba_port *port)
{
    port->cmd &= ~(1u << 0);    /* ST  */
    port->cmd &= ~(1u << 4);    /* FRE */

    for (int i = 0; i < 1000; i++) {
        if (!(port->cmd & ((1u << 14) | (1u << 15))))   /* FR, CR */
            break;
        io_wait();
    }
}

static void port_start(struct hba_port *port)
{
    while (port->cmd & (1u << 15))   /* auf CR = 0 warten */
        io_wait();

    port->cmd |= (1u << 4);    /* FRE */
    port->cmd |= (1u << 0);    /* ST  */
}

/* Wartet, bis der Controller den Befehl abgearbeitet hat. */
static bool wait_command(struct hba_port *port)
{
    uint64_t deadline = timer_ms() + CMD_TIMEOUT_MS;

    while (port->ci & 1) {
        if (port->is & (1u << 30)) {       /* Task File Error */
            kprintf("AHCI        : Fehler beim Zugriff (TFES)\n");
            return false;
        }
        if (timer_ms() > deadline) {
            kprintf("AHCI        : Zeitueberschreitung\n");
            return false;
        }
    }
    return !(port->is & (1u << 30));
}

/* Baut einen ATA-Befehl auf und fuehrt ihn aus. */
static bool run_ata(struct ahci_drive *d, uint8_t command, uint64_t lba,
                    uint16_t sectors, uint32_t bytes, bool write)
{
    struct hba_port *port = d->port;

    port->is = (uint32_t)-1;   /* alte Meldungen loeschen */

    struct hba_cmd_header *hdr = &d->cmd_list[0];

    memset(hdr, 0, sizeof(*hdr));
    hdr->flags_low = (uint8_t)(sizeof(struct fis_reg_h2d) / 4);
    if (write)
        hdr->flags_low |= (1 << 6);        /* Schreibrichtung */
    hdr->prdtl = 1;
    hdr->ctba  = (uint32_t)(d->cmd_table_phys & 0xFFFFFFFF);
    hdr->ctbau = (uint32_t)(d->cmd_table_phys >> 32);

    memset(d->cmd_table, 0, sizeof(struct hba_cmd_table));
    d->cmd_table->prdt[0].dba       = (uint32_t)(d->bounce_phys & 0xFFFFFFFF);
    d->cmd_table->prdt[0].dbau      = (uint32_t)(d->bounce_phys >> 32);
    d->cmd_table->prdt[0].dbc_flags = (bytes - 1) & 0x3FFFFF;

    struct fis_reg_h2d *fis = (struct fis_reg_h2d *)d->cmd_table->cfis;

    fis->fis_type     = 0x27;
    fis->pmport_flags = 0x80;
    fis->command      = command;
    fis->lba0   = (uint8_t)(lba);
    fis->lba1   = (uint8_t)(lba >> 8);
    fis->lba2   = (uint8_t)(lba >> 16);
    fis->device = 0x40;                    /* LBA-Modus */
    fis->lba3   = (uint8_t)(lba >> 24);
    fis->lba4   = (uint8_t)(lba >> 32);
    fis->lba5   = (uint8_t)(lba >> 40);
    fis->countl = (uint8_t)(sectors);
    fis->counth = (uint8_t)(sectors >> 8);

    /* Warten, bis das Laufwerk nicht mehr beschaeftigt ist. */
    uint64_t deadline = timer_ms() + CMD_TIMEOUT_MS;
    while (port->tfd & 0x88) {             /* BSY | DRQ */
        if (timer_ms() > deadline) {
            kprintf("AHCI        : Laufwerk antwortet nicht\n");
            return false;
        }
    }

    port->ci = 1;
    return wait_command(port);
}

static bool ahci_read(struct block_device *bdev, uint64_t lba, uint32_t count,
                      void *buffer)
{
    struct ahci_drive *d = bdev->driver_data;
    uint8_t *out = buffer;

    while (count > 0) {
        uint32_t chunk = MIN(count, (uint32_t)BOUNCE_SECTORS);

        if (!run_ata(d, 0x25 /* READ DMA EXT */, lba, (uint16_t)chunk,
                     chunk * BLOCK_SECTOR_SIZE, false))
            return false;

        memcpy(out, d->bounce, chunk * BLOCK_SECTOR_SIZE);
        out   += chunk * BLOCK_SECTOR_SIZE;
        lba   += chunk;
        count -= chunk;
    }
    return true;
}

static bool ahci_write(struct block_device *bdev, uint64_t lba, uint32_t count,
                       void *buffer)
{
    struct ahci_drive *d = bdev->driver_data;
    const uint8_t *in = buffer;

    while (count > 0) {
        uint32_t chunk = MIN(count, (uint32_t)BOUNCE_SECTORS);

        memcpy(d->bounce, in, chunk * BLOCK_SECTOR_SIZE);

        if (!run_ata(d, 0x35 /* WRITE DMA EXT */, lba, (uint16_t)chunk,
                     chunk * BLOCK_SECTOR_SIZE, true))
            return false;

        /* Puffer des Laufwerks leeren, damit Daten wirklich liegen. */
        if (!run_ata(d, 0xEA /* FLUSH CACHE EXT */, 0, 0, 512, false))
            return false;

        in    += chunk * BLOCK_SECTOR_SIZE;
        lba   += chunk;
        count -= chunk;
    }
    return true;
}

/* Modellname aus der IDENTIFY-Antwort: 16-Bit-Woerter, byteweise vertauscht. */
static void extract_string(const uint16_t *id, int word, int words, char *out)
{
    int n = 0;

    for (int i = 0; i < words; i++) {
        out[n++] = (char)(id[word + i] >> 8);
        out[n++] = (char)(id[word + i] & 0xFF);
    }
    out[n] = '\0';

    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0'))
        out[--n] = '\0';
}

static bool setup_port(struct hba_port *port, int index)
{
    if (drive_count >= AHCI_MAX_DRIVES)
        return false;

    /* Nur angeschlossene SATA-Platten beachten. */
    uint32_t ssts = port->ssts;
    if ((ssts & 0x0F) != 3 || ((ssts >> 8) & 0x0F) != 1)
        return false;
    if (port->sig != 0x00000101)      /* keine ATA-Platte (z.B. ATAPI) */
        return false;

    struct ahci_drive *d = &drives[drive_count];

    memset(d, 0, sizeof(*d));
    d->port = port;

    /* Eine Seite fasst Befehlsliste, Empfangsflaeche und Befehlstabelle. */
    uint64_t phys = pmm_alloc_page();
    if (!phys)
        return false;

    uint8_t *page = phys_to_virt(phys);
    memset(page, 0, PAGE_SIZE);

    d->cmd_list       = (struct hba_cmd_header *)page;
    d->cmd_list_phys  = phys;
    d->fis_phys       = phys + 0x400;
    d->cmd_table      = (struct hba_cmd_table *)(page + 0x500);
    d->cmd_table_phys = phys + 0x500;

    uint64_t bounce_phys = pmm_alloc_pages(BOUNCE_SECTORS * BLOCK_SECTOR_SIZE / PAGE_SIZE);
    if (!bounce_phys) {
        pmm_free_page(phys);
        return false;
    }
    d->bounce      = phys_to_virt(bounce_phys);
    d->bounce_phys = bounce_phys;

    port_stop(port);
    port->clb  = (uint32_t)(d->cmd_list_phys & 0xFFFFFFFF);
    port->clbu = (uint32_t)(d->cmd_list_phys >> 32);
    port->fb   = (uint32_t)(d->fis_phys & 0xFFFFFFFF);
    port->fbu  = (uint32_t)(d->fis_phys >> 32);
    port->serr = (uint32_t)-1;
    port->is   = (uint32_t)-1;
    port_start(port);

    if (!run_ata(d, 0xEC /* IDENTIFY DEVICE */, 0, 0, 512, false)) {
        pmm_free_page(phys);
        return false;
    }

    const uint16_t *id = (const uint16_t *)d->bounce;

    /* Wortweise zusammensetzen - der Puffer ist nicht auf 64 Bit ausgerichtet. */
    uint64_t sectors = (uint64_t)id[100]
                     | ((uint64_t)id[101] << 16)
                     | ((uint64_t)id[102] << 32)
                     | ((uint64_t)id[103] << 48);

    if (sectors == 0)
        sectors = ((uint64_t)id[61] << 16) | id[60];  /* 28-Bit-Rueckfall */

    d->block.sector_count = sectors;
    d->block.sector_size  = BLOCK_SECTOR_SIZE;
    d->block.read         = ahci_read;
    d->block.write        = ahci_write;
    d->block.driver_data  = d;
    ksnprintf(d->block.name, sizeof(d->block.name), "sata%d", index);
    extract_string(id, 27, 20, d->block.model);

    drive_count++;
    block_register(&d->block);
    return true;
}

void ahci_init(void)
{
    const struct pci_device *dev = pci_find_class(0x01, 0x06);

    if (!dev)
        return;

    pci_enable_bus_master(dev);

    struct hba_mem *hba = phys_to_virt(dev->bar[5]);

    /* AHCI-Betrieb einschalten. */
    hba->ghc |= (1u << 31);

    uint32_t implemented = hba->pi;
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (implemented & (1u << i))
            setup_port(&hba->ports[i], i);
    }
}
