/* nvme.c - Treiber fuer NVM-Express-Datentraeger.
 *
 * Eine NVMe-SSD haengt unmittelbar am PCIe-Bus; es gibt keinen
 * Zwischencontroller wie bei SATA. Die Verstaendigung laeuft ueber
 * Warteschlangenpaare im Arbeitsspeicher: Der Kern legt einen Befehl in
 * die Sende-Warteschlange und stupst eine Tuerklingel an; das Geraet
 * arbeitet ihn ab und legt das Ergebnis in die Empfangs-Warteschlange.
 *
 * RetroOS benutzt zwei Paare - eines fuer die Verwaltung, eines fuer die
 * Nutzdaten - und wartet auf das Ergebnis, statt sich unterbrechen zu
 * lassen: Der Dateizugriff im Kern ist ohnehin gleichlaufend, und so
 * bleibt der Weg kurz und ohne Verriegelung.
 */

#include "block.h"
#include "arch.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"

/* --- Register des Controllers --- */
#define NVME_CAP      0x00      /* Faehigkeiten, 64 Bit   */
#define NVME_VS       0x08      /* Fassung                */
#define NVME_INTMS    0x0C
#define NVME_INTMC    0x10
#define NVME_CC       0x14      /* Steuerung              */
#define NVME_CSTS     0x1C      /* Zustand                */
#define NVME_AQA      0x24      /* Groesse der Verwaltung */
#define NVME_ASQ      0x28
#define NVME_ACQ      0x30

#define CC_ENABLE     (1u << 0)
#define CSTS_READY    (1u << 0)
#define CSTS_FATAL    (1u << 1)

/* --- Befehle --- */
#define ADMIN_CREATE_SQ   0x01
#define ADMIN_CREATE_CQ   0x05
#define ADMIN_IDENTIFY    0x06
#define ADMIN_SET_FEATURE 0x09

#define IO_WRITE          0x01
#define IO_READ           0x02
#define IO_FLUSH          0x00

#define QUEUE_SLOTS   32        /* Eintraege je Warteschlange */
#define NVME_MAX      2

/* Ein Befehl ist immer 64 Byte lang. */
struct nvme_command {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} PACKED;

/* Ein Ergebnis ist 16 Byte lang. */
struct nvme_completion {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;
} PACKED;

struct nvme_queue {
    struct nvme_command    *sq;
    struct nvme_completion *cq;
    uint64_t                sq_phys, cq_phys;

    volatile uint32_t      *sq_doorbell;
    volatile uint32_t      *cq_doorbell;

    uint16_t                sq_tail;
    uint16_t                cq_head;
    uint8_t                 phase;      /* erwartetes Phasenbit */
    uint16_t                next_id;
};

struct nvme_controller {
    volatile uint8_t   *regs;
    uint32_t            doorbell_stride;

    struct nvme_queue   admin;
    struct nvme_queue   io;

    uint32_t            namespace_id;
    uint64_t            sector_count;
    uint32_t            sector_size;
    uint32_t            max_transfer;   /* Sektoren je Befehl */

    /* Zwischenpuffer, damit auch unausgerichtete Ziele bedient werden. */
    uint8_t            *bounce;
    uint64_t            bounce_phys;
    uint32_t            bounce_sectors;

    struct block_device block;
    char                model[48];
};

static struct nvme_controller controllers[NVME_MAX];
static size_t                 controller_count;

/* ------------------------------------------------------------------ */
/* Register                                                            */
/* ------------------------------------------------------------------ */

static uint32_t reg_read32(struct nvme_controller *c, uint32_t offset)
{
    return *(volatile uint32_t *)(c->regs + offset);
}

static void reg_write32(struct nvme_controller *c, uint32_t offset,
                        uint32_t value)
{
    *(volatile uint32_t *)(c->regs + offset) = value;
}

static uint64_t reg_read64(struct nvme_controller *c, uint32_t offset)
{
    /* Der Bereich ist als 64 Bit ansprechbar, aber zwei 32-Bit-Zugriffe
     * gehen auf jedem Chipsatz. */
    return (uint64_t)reg_read32(c, offset) |
           ((uint64_t)reg_read32(c, offset + 4) << 32);
}

static void reg_write64(struct nvme_controller *c, uint32_t offset,
                        uint64_t value)
{
    reg_write32(c, offset, (uint32_t)value);
    reg_write32(c, offset + 4, (uint32_t)(value >> 32));
}

/* Die Tuerklingeln liegen ab 0x1000, ihr Abstand steht in CAP. */
static volatile uint32_t *doorbell(struct nvme_controller *c, uint16_t queue,
                                   bool completion)
{
    uint32_t offset = 0x1000 +
                      ((uint32_t)queue * 2 + (completion ? 1 : 0)) *
                      c->doorbell_stride;

    return (volatile uint32_t *)(c->regs + offset);
}

/* ------------------------------------------------------------------ */
/* Warteschlangen                                                      */
/* ------------------------------------------------------------------ */

static bool queue_create(struct nvme_controller *c, struct nvme_queue *q,
                         uint16_t id)
{
    uint64_t sq_phys = pmm_alloc_page();
    uint64_t cq_phys = pmm_alloc_page();

    if (!sq_phys || !cq_phys)
        return false;

    q->sq = phys_to_virt(sq_phys);
    q->cq = phys_to_virt(cq_phys);
    q->sq_phys = sq_phys;
    q->cq_phys = cq_phys;
    q->phase = 1;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->next_id = 0;

    memset(q->sq, 0, PAGE_SIZE);
    memset(q->cq, 0, PAGE_SIZE);

    q->sq_doorbell = doorbell(c, id, false);
    q->cq_doorbell = doorbell(c, id, true);
    return true;
}

/* Legt einen Befehl ab und wartet auf das Ergebnis. */
static bool submit(struct nvme_controller *c, struct nvme_queue *q,
                   struct nvme_command *cmd, uint32_t *result)
{
    UNUSED(c);

    uint16_t id = q->next_id++;

    cmd->command_id = id;
    q->sq[q->sq_tail] = *cmd;

    q->sq_tail = (uint16_t)((q->sq_tail + 1) % QUEUE_SLOTS);
    *q->sq_doorbell = q->sq_tail;

    /* Auf das Ergebnis warten. Das Phasenbit kippt bei jedem Umlauf,
     * daran erkennt man einen frischen Eintrag. */
    uint64_t deadline = timer_ms() + 5000;

    for (;;) {
        volatile struct nvme_completion *entry = &q->cq[q->cq_head];

        /* Das unterste Bit des Zustands ist das Phasenbit. Es kippt bei
         * jedem Umlauf der Warteschlange; stimmt es mit dem erwarteten
         * ueberein, ist der Eintrag frisch. */
        if ((entry->status & 1) == q->phase) {
            uint16_t status = (uint16_t)(entry->status >> 1);

            if (result)
                *result = entry->result;

            q->cq_head = (uint16_t)((q->cq_head + 1) % QUEUE_SLOTS);
            if (q->cq_head == 0)
                q->phase ^= 1;
            *q->cq_doorbell = q->cq_head;

            return status == 0;
        }
        if (timer_ms() > deadline)
            return false;
        if (reg_read32(c, NVME_CSTS) & CSTS_FATAL)
            return false;
        __asm__ volatile("pause");
    }
}

/* ------------------------------------------------------------------ */
/* Lesen und Schreiben                                                 */
/* ------------------------------------------------------------------ */

/* Ein Befehl beschreibt seinen Puffer mit hoechstens zwei Zeigern.
 * Reicht das nicht, kommt eine Liste weiterer Zeiger dazu - hier
 * genuegt uns der Zwischenpuffer, der immer zusammenhaengend liegt. */
static bool transfer(struct nvme_controller *c, uint64_t lba, uint32_t count,
                     bool write)
{
    struct nvme_command cmd;
    uint64_t bytes = (uint64_t)count * c->sector_size;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = write ? IO_WRITE : IO_READ;
    cmd.nsid = c->namespace_id;
    cmd.prp1 = c->bounce_phys;
    if (bytes > PAGE_SIZE)
        cmd.prp2 = c->bounce_phys + PAGE_SIZE;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;              /* null bedeutet ein Sektor */

    return submit(c, &c->io, &cmd, NULL);
}

static bool nvme_read(struct block_device *dev, uint64_t lba, uint32_t count,
                      void *buffer)
{
    struct nvme_controller *c = dev->driver_data;
    uint8_t *out = buffer;

    while (count > 0) {
        uint32_t chunk = MIN(count, c->bounce_sectors);

        if (!transfer(c, lba, chunk, false))
            return false;
        memcpy(out, c->bounce, (size_t)chunk * c->sector_size);

        out += (size_t)chunk * c->sector_size;
        lba += chunk;
        count -= chunk;
    }
    return true;
}

static bool nvme_write(struct block_device *dev, uint64_t lba, uint32_t count,
                       void *buffer)
{
    struct nvme_controller *c = dev->driver_data;
    const uint8_t *in = buffer;

    while (count > 0) {
        uint32_t chunk = MIN(count, c->bounce_sectors);

        memcpy(c->bounce, in, (size_t)chunk * c->sector_size);
        if (!transfer(c, lba, chunk, true))
            return false;

        in += (size_t)chunk * c->sector_size;
        lba += chunk;
        count -= chunk;
    }

    /* Der Controller darf zwischenspeichern - also anschliessend
     * ausdruecklich auf den Datentraeger schreiben lassen. */
    struct nvme_command flush;

    memset(&flush, 0, sizeof(flush));
    flush.opcode = IO_FLUSH;
    flush.nsid = c->namespace_id;
    submit(c, &c->io, &flush, NULL);
    return true;
}

/* ------------------------------------------------------------------ */
/* Einrichten                                                          */
/* ------------------------------------------------------------------ */

static bool wait_ready(struct nvme_controller *c, bool ready)
{
    uint64_t deadline = timer_ms() + 5000;

    for (;;) {
        uint32_t status = reg_read32(c, NVME_CSTS);

        if (status & CSTS_FATAL)
            return false;
        if (((status & CSTS_READY) != 0) == ready)
            return true;
        if (timer_ms() > deadline)
            return false;
        __asm__ volatile("pause");
    }
}

static bool identify(struct nvme_controller *c, uint32_t nsid, uint8_t cns,
                     void *out)
{
    struct nvme_command cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = ADMIN_IDENTIFY;
    cmd.nsid = nsid;
    cmd.prp1 = c->bounce_phys;
    cmd.cdw10 = cns;

    if (!submit(c, &c->admin, &cmd, NULL))
        return false;
    memcpy(out, c->bounce, 4096);
    return true;
}

static bool create_io_queues(struct nvme_controller *c)
{
    struct nvme_command cmd;
    uint32_t result = 0;

    /* Erst sagen, wie viele Paare wir wollen. */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = ADMIN_SET_FEATURE;
    cmd.cdw10 = 0x07;                    /* Anzahl der Warteschlangen */
    cmd.cdw11 = 0;                       /* je eines, null bedeutet eins */
    if (!submit(c, &c->admin, &cmd, &result))
        return false;

    if (!queue_create(c, &c->io, 1))
        return false;

    /* Die Empfangsseite muss vor der Sendeseite bestehen. */
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = ADMIN_CREATE_CQ;
    cmd.prp1 = c->io.cq_phys;
    cmd.cdw10 = (uint32_t)((QUEUE_SLOTS - 1) << 16) | 1;
    cmd.cdw11 = 1;                       /* zusammenhaengend, ohne IRQ */
    if (!submit(c, &c->admin, &cmd, NULL))
        return false;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = ADMIN_CREATE_SQ;
    cmd.prp1 = c->io.sq_phys;
    cmd.cdw10 = (uint32_t)((QUEUE_SLOTS - 1) << 16) | 1;
    cmd.cdw11 = (1u << 16) | 1;          /* gehoert zu Empfangsschlange 1 */
    return submit(c, &c->admin, &cmd, NULL);
}

/* Kopiert einen Text aus der Identify-Antwort und schneidet Leerzeichen ab. */
static void trim_field(const uint8_t *field, size_t length, char *out,
                       size_t size)
{
    size_t at = 0;

    for (size_t i = 0; i < length && at + 1 < size; i++)
        out[at++] = (char)field[i];
    out[at] = '\0';
    while (at > 0 && (out[at - 1] == ' ' || out[at - 1] == '\0'))
        out[--at] = '\0';
}

static bool setup(const struct pci_device *dev)
{
    if (controller_count >= NVME_MAX)
        return false;

    struct nvme_controller *c = &controllers[controller_count];

    memset(c, 0, sizeof(*c));

    if (dev->bar[0] == 0 || dev->bar_is_io[0])
        return false;

    c->regs = phys_to_virt(dev->bar[0]);
    pci_enable_bus_master(dev);

    uint64_t cap = reg_read64(c, NVME_CAP);

    c->doorbell_stride = 4u << ((cap >> 32) & 0xF);

    /* Der Controller muss aus sein, bevor man ihn einrichtet. */
    uint32_t cc = reg_read32(c, NVME_CC);

    if (cc & CC_ENABLE) {
        reg_write32(c, NVME_CC, cc & ~CC_ENABLE);
        if (!wait_ready(c, false))
            return false;
    }

    /* Ein Zwischenpuffer von 64 KiB - zwei Zeiger reichen dafuer nicht,
     * darum begrenzen wir die Uebertragung auf zwei Seiten je Befehl. */
    uint64_t bounce_phys = pmm_alloc_pages(2);

    if (!bounce_phys)
        return false;
    c->bounce = phys_to_virt(bounce_phys);
    c->bounce_phys = bounce_phys;

    if (!queue_create(c, &c->admin, 0)) {
        pmm_free_pages(bounce_phys, 2);
        return false;
    }

    reg_write32(c, NVME_AQA, ((QUEUE_SLOTS - 1) << 16) | (QUEUE_SLOTS - 1));
    reg_write64(c, NVME_ASQ, c->admin.sq_phys);
    reg_write64(c, NVME_ACQ, c->admin.cq_phys);

    /* Befehlsgroessen: 64 Byte senden, 16 Byte empfangen, Seiten zu 4 KiB. */
    cc = (6u << 16) | (4u << 20) | (0u << 7) | (0u << 11) | CC_ENABLE;
    reg_write32(c, NVME_CC, cc);

    if (!wait_ready(c, true))
        return false;

    /* Wir warten auf die Ergebnisse, also keine Unterbrechungen. */
    reg_write32(c, NVME_INTMS, 0xFFFFFFFFu);

    uint8_t *identity = c->bounce;

    if (!identify(c, 0, 1, identity))
        return false;

    char model[41], serial[21];

    trim_field(identity + 24, 40, model, sizeof(model));
    trim_field(identity + 4, 20, serial, sizeof(serial));

    /* Die groesste Uebertragung steht als Zweierpotenz der Seitengroesse. */
    uint8_t mdts = identity[77];

    c->max_transfer = mdts ? (1u << mdts) : 64;

    if (!identify(c, 1, 0, identity))
        return false;

    uint64_t sectors = 0;

    for (int i = 0; i < 8; i++)
        sectors |= (uint64_t)identity[i] << (i * 8);

    uint8_t format = identity[26] & 0xF;
    uint8_t lba_shift = identity[128 + format * 4 + 2];

    c->namespace_id = 1;
    c->sector_count = sectors;
    c->sector_size = lba_shift ? (1u << lba_shift) : 512;

    if (c->sector_size < 512 || c->sector_size > 4096 || sectors == 0)
        return false;

    c->bounce_sectors = (uint32_t)(2 * PAGE_SIZE / c->sector_size);
    if (c->bounce_sectors > c->max_transfer)
        c->bounce_sectors = c->max_transfer;
    if (c->bounce_sectors == 0)
        c->bounce_sectors = 1;

    if (!create_io_queues(c))
        return false;

    ksnprintf(c->model, sizeof(c->model), "%s", model[0] ? model : "NVMe");

    ksnprintf(c->block.name, sizeof(c->block.name), "nvme%u",
              (unsigned)controller_count);
    strlcpy(c->block.model, c->model, sizeof(c->block.model));
    c->block.sector_count = c->sector_count;
    c->block.sector_size = c->sector_size;
    c->block.read = nvme_read;
    c->block.write = nvme_write;
    c->block.driver_data = c;

    controller_count++;
    block_register(&c->block);
    return true;
}

void nvme_init(void)
{
    for (size_t i = 0; i < pci_device_count(); i++) {
        const struct pci_device *dev = pci_device_at(i);

        /* Klasse 1, Unterklasse 8, Schnittstelle 2 ist NVM Express. */
        if (!dev || dev->class_code != 0x01 || dev->subclass != 0x08)
            continue;
        if (dev->prog_if != 0x02)
            continue;

        if (!setup(dev))
            kprintf("NVMe        : Controller %02x:%02x.%u antwortet nicht\n",
                    dev->bus, dev->slot, dev->func);
    }
}
