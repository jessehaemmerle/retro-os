/* igb.c - Intel-Gigabitkarten der Reihe 82575 bis I350 und I210/I211.
 *
 * Von aussen sehen diese Karten aus wie die aelteren 8254x: PCI-Kennung
 * 0x8086, dieselben Namen im Handel, dieselben Buchsen. Innen sind sie
 * aber umgebaut worden, und zwar an genau den beiden Stellen, auf die
 * es einem Treiber ankommt:
 *
 *   - Die Ringe liegen nicht mehr bei 0x2800 und 0x3800, sondern in
 *     eigenen Bloecken ab 0xC000 und 0xE000. Dort ist Platz fuer bis zu
 *     sechzehn Warteschlangen; wir benutzen die erste.
 *   - Die Deskriptoren sind laenger geworden. Statt der alten Form mit
 *     Laenge und Zustand nebeneinander gibt es jetzt zwei Faelle: Was
 *     der Kern hineinschreibt, und was die Karte darueberschreibt.
 *     Beides teilt sich dieselben sechzehn Byte.
 *
 * Deshalb hat diese Reihe einen eigenen Treiber und benutzt nicht den
 * von e1000.c mit. Verbreitet ist sie sehr: Auf Hauptplatinen fuer
 * Server steckt fast immer eine I210 oder I350, und die kleinen
 * Netzwerkkarten zum Nachruesten sind es meistens auch.
 */

#include "net.h"
#include "nic.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"

/* --- Allgemeine Register, hier wie bei den aelteren Karten --- */
#define REG_CTRL       0x0000
#define REG_STATUS     0x0008
#define REG_CTRL_EXT   0x0018
#define REG_MDIC       0x0020
#define REG_IMC        0x0150
#define REG_EIMC       0x1528
#define REG_RCTL       0x0100
#define REG_TCTL       0x0400
#define REG_RXPBS      0x2404
#define REG_MTA        0x5200
#define REG_RAL0       0x5400
#define REG_RAH0       0x5404

/* --- Die erste Empfangs- und Sendewarteschlange --- */
#define REG_RDBAL      0xC000
#define REG_RDBAH      0xC004
#define REG_RDLEN      0xC008
#define REG_SRRCTL     0xC00C
#define REG_RDH        0xC010
#define REG_RDT        0xC018
#define REG_RXDCTL     0xC028

#define REG_TDBAL      0xE000
#define REG_TDBAH      0xE004
#define REG_TDLEN      0xE008
#define REG_TDH        0xE010
#define REG_TDT        0xE018
#define REG_TXDCTL     0xE028

#define CTRL_RST       (1u << 26)
#define CTRL_SLU       (1u << 6)

#define STATUS_LU      (1u << 1)
#define STATUS_SPEED   (3u << 6)

#define RCTL_EN        (1u << 1)
#define RCTL_BAM       (1u << 15)   /* Rundsendungen annehmen  */
#define RCTL_SECRC     (1u << 26)   /* Pruefbytes abschneiden  */

#define TCTL_EN        (1u << 1)
#define TCTL_PSP       (1u << 3)    /* kurze Rahmen auffuellen */

#define QUEUE_ENABLE   (1u << 25)

/* Die Warteschlange sagt der Karte, wie gross ein Puffer ist (in
 * Kilobyte) und welche Deskriptorform sie benutzen soll. */
#define SRRCTL_BSIZE_2K   2
#define SRRCTL_DESC_ADV   (1u << 25)   /* erweiterte Form, ein Puffer */
#define SRRCTL_DROP_EN    (1u << 31)

/* Beim Senden steht die Art des Deskriptors mit im Befehlswort. */
#define TXD_DTYP_DATA  (3u << 20)
#define TXD_CMD_EOP    (1u << 24)
#define TXD_CMD_IFCS   (1u << 25)
#define TXD_CMD_RS     (1u << 27)
#define TXD_CMD_DEXT   (1u << 29)
#define TXD_STAT_DD    (1u << 0)

#define RXD_STAT_DD    (1u << 0)
#define RXD_STAT_EOP   (1u << 1)

#define RING_SIZE      32
#define BUFFER_BYTES   2048

/* Der Kern legt zwei Adressen hinein, die Karte schreibt Zustand und
 * Laenge darueber - dieselben sechzehn Byte, zwei Sichtweisen. */
struct rx_desc {
    union {
        struct {
            uint64_t packet_addr;
            uint64_t header_addr;
        } read;
        struct {
            uint32_t info;
            uint32_t hash;
            uint32_t status_error;
            uint16_t length;
            uint16_t vlan;
        } wb;
    };
} PACKED;

struct tx_desc {
    uint64_t address;
    uint32_t command;        /* Art, Befehle und Laenge   */
    uint32_t status;         /* Nutzlaenge und Rueckmeldung */
} PACKED;

struct igb {
    volatile uint8_t *mmio;
    struct rx_desc   *rx_ring;
    struct tx_desc   *tx_ring;
    uint8_t          *rx_buffers, *tx_buffers;
    uint64_t          rx_ring_phys, tx_ring_phys;
    uint64_t          rx_buffers_phys, tx_buffers_phys;
    uint64_t          block_phys;
    size_t            block_pages;
    uint32_t          rx_cursor, tx_cursor;
};

static struct igb device;

static uint32_t reg_read(uint32_t offset)
{
    return *(volatile uint32_t *)(device.mmio + offset);
}

static void reg_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(device.mmio + offset) = value;
}

/* Ein kurzer Lesezugriff zwingt die Schreibvorgaenge ueber den Bus. */
static void reg_flush(void)
{
    (void)reg_read(REG_STATUS);
}

/* ------------------------------------------------------------------ */
/* Betrieb                                                             */
/* ------------------------------------------------------------------ */

static bool igb_send(struct nic *nic, const void *frame, uint16_t length)
{
    UNUSED(nic);

    if (length == 0 || length > BUFFER_BYTES)
        return false;

    uint32_t slot = device.tx_cursor;
    struct tx_desc *desc = &device.tx_ring[slot];

    /* Warten, bis die Karte den Platz zurueckgegeben hat. */
    for (int guard = 0; guard < 1000000; guard++) {
        if (desc->status & TXD_STAT_DD)
            break;
        io_wait();
    }
    if (!(desc->status & TXD_STAT_DD))
        return false;

    memcpy(device.tx_buffers + (size_t)slot * BUFFER_BYTES, frame, length);

    desc->address = device.tx_buffers_phys + (uint64_t)slot * BUFFER_BYTES;
    desc->command = (uint32_t)length | TXD_DTYP_DATA | TXD_CMD_DEXT |
                    TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    /* Die Nutzlaenge steht ab Bit vierzehn - sie zaehlt, was die Karte
     * insgesamt auf die Leitung legen soll. */
    desc->status = (uint32_t)length << 14;

    device.tx_cursor = (slot + 1) % RING_SIZE;

    __asm__ volatile("sfence" ::: "memory");
    reg_write(REG_TDT, device.tx_cursor);

    g_netif.tx_packets++;
    g_netif.tx_bytes += length;
    return true;
}

static uint16_t igb_receive(struct nic *nic, void *buffer, uint16_t capacity)
{
    UNUSED(nic);

    struct rx_desc *desc = &device.rx_ring[device.rx_cursor];

    if (!(desc->wb.status_error & RXD_STAT_DD))
        return 0;

    uint16_t length = desc->wb.length;
    uint16_t copied = 0;

    if ((desc->wb.status_error & RXD_STAT_EOP) && length > 0 &&
        length <= capacity) {
        memcpy(buffer,
               device.rx_buffers + (size_t)device.rx_cursor * BUFFER_BYTES,
               length);
        copied = length;
        g_netif.rx_packets++;
        g_netif.rx_bytes += length;
    } else {
        g_netif.rx_dropped++;
    }

    /* Den Deskriptor wieder auf die Lesesicht zuruecksetzen. */
    desc->read.packet_addr =
        device.rx_buffers_phys + (uint64_t)device.rx_cursor * BUFFER_BYTES;
    desc->read.header_addr = 0;

    uint32_t last = device.rx_cursor;

    device.rx_cursor = (device.rx_cursor + 1) % RING_SIZE;

    __asm__ volatile("sfence" ::: "memory");
    reg_write(REG_RDT, last);
    return copied;
}

static bool igb_link_up(struct nic *nic)
{
    UNUSED(nic);
    return (reg_read(REG_STATUS) & STATUS_LU) != 0;
}

static const struct nic_ops igb_ops = {
    .send    = igb_send,
    .receive = igb_receive,
    .link_up = igb_link_up,
};

/* ------------------------------------------------------------------ */
/* Erkennen                                                            */
/* ------------------------------------------------------------------ */

static const struct { uint16_t id; const char *name; } named[] = {
    { 0x10A7, "Intel 82575EB" },
    { 0x10A9, "Intel 82575EB" },
    { 0x10D6, "Intel 82575GB" },
    { 0x10C9, "Intel 82576" },
    { 0x10E6, "Intel 82576" },
    { 0x10E7, "Intel 82576" },
    { 0x10E8, "Intel 82576" },
    { 0x1526, "Intel 82576" },
    { 0x150A, "Intel 82576NS" },
    { 0x1518, "Intel 82576NS" },
    { 0x150D, "Intel 82576" },
    { 0x1516, "Intel 82580" },
    { 0x150E, "Intel 82580" },
    { 0x150F, "Intel 82580" },
    { 0x1510, "Intel 82580" },
    { 0x1511, "Intel 82580" },
    { 0x1527, "Intel 82580" },
    { 0x1521, "Intel I350" },
    { 0x1522, "Intel I350" },
    { 0x1523, "Intel I350" },
    { 0x1524, "Intel I350" },
    { 0x1546, "Intel I350" },
    { 0x1F40, "Intel I354" },
    { 0x1F41, "Intel I354" },
    { 0x1F45, "Intel I354" },
    { 0x1533, "Intel I210" },
    { 0x1536, "Intel I210" },
    { 0x1537, "Intel I210" },
    { 0x1538, "Intel I210" },
    { 0x157B, "Intel I210" },
    { 0x157C, "Intel I210" },
    { 0x1539, "Intel I211" },
};

bool igb_owns(uint16_t device_id)
{
    for (size_t i = 0; i < ARRAY_LEN(named); i++)
        if (named[i].id == device_id)
            return true;
    return false;
}

static bool igb_probe(const struct pci_device *pci)
{
    return pci->vendor_id == 0x8086 && igb_owns(pci->device_id);
}

/* ------------------------------------------------------------------ */
/* Einrichten                                                          */
/* ------------------------------------------------------------------ */

static bool read_mac(void)
{
    uint32_t low  = reg_read(REG_RAL0);
    uint32_t high = reg_read(REG_RAH0);

    if (low == 0 && (high & 0xFFFF) == 0)
        return false;

    g_netif.mac.b[0] = (uint8_t)low;
    g_netif.mac.b[1] = (uint8_t)(low >> 8);
    g_netif.mac.b[2] = (uint8_t)(low >> 16);
    g_netif.mac.b[3] = (uint8_t)(low >> 24);
    g_netif.mac.b[4] = (uint8_t)high;
    g_netif.mac.b[5] = (uint8_t)(high >> 8);
    return true;
}

static uint32_t link_speed(void)
{
    switch ((reg_read(REG_STATUS) & STATUS_SPEED) >> 6) {
    case 0:  return 10;
    case 1:  return 100;
    default: return 1000;
    }
}

static bool igb_attach(const struct pci_device *pci, struct nic *nic)
{
    const char *name = "Intel-Gigabitkarte";

    for (size_t i = 0; i < ARRAY_LEN(named); i++)
        if (named[i].id == pci->device_id)
            name = named[i].name;

    memset(&device, 0, sizeof(device));

    if (!pci->bar[0] || pci->bar_is_io[0])
        return false;

    device.mmio = phys_to_virt(pci->bar[0]);
    pci_enable_bus_master(pci);
    pci_set_intx(pci, false);

    /* Alle Unterbrechungen abschalten - wir fragen die Ringe ab. */
    reg_write(REG_EIMC, 0xFFFFFFFF);
    reg_write(REG_IMC, 0xFFFFFFFF);
    reg_flush();

    if (!read_mac()) {
        kprintf("Netzwerk    : %s meldet keine Adresse\n", name);
        return false;
    }

    /* Mehrfachadressen: nichts durchlassen, was nicht an uns geht. */
    for (int i = 0; i < 128; i++)
        reg_write(REG_MTA + (uint32_t)i * 4, 0);

    /* Speicher fuer beide Ringe und die Puffer in einem Stueck. */
    size_t ring_bytes = ALIGN_UP(sizeof(struct rx_desc) * RING_SIZE, 128) +
                        ALIGN_UP(sizeof(struct tx_desc) * RING_SIZE, 128);
    size_t buffer_bytes = (size_t)RING_SIZE * BUFFER_BYTES * 2;
    size_t total = ALIGN_UP(ring_bytes + buffer_bytes, PAGE_SIZE);
    size_t pages = total / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);

    if (!phys)
        return false;

    memset(phys_to_virt(phys), 0, total);

    device.block_phys = phys;
    device.block_pages = pages;

    size_t rx_ring_bytes = ALIGN_UP(sizeof(struct rx_desc) * RING_SIZE, 128);

    device.rx_ring_phys = phys;
    device.tx_ring_phys = phys + rx_ring_bytes;
    device.rx_buffers_phys = phys + ring_bytes;
    device.tx_buffers_phys = device.rx_buffers_phys +
                             (uint64_t)RING_SIZE * BUFFER_BYTES;

    device.rx_ring    = phys_to_virt(device.rx_ring_phys);
    device.tx_ring    = phys_to_virt(device.tx_ring_phys);
    device.rx_buffers = phys_to_virt(device.rx_buffers_phys);
    device.tx_buffers = phys_to_virt(device.tx_buffers_phys);

    /* Verbindung selbst aushandeln. */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU);

    /* --- Empfang --- */
    reg_write(REG_RCTL, 0);            /* erst aus, dann einrichten */

    for (uint32_t i = 0; i < RING_SIZE; i++) {
        device.rx_ring[i].read.packet_addr =
            device.rx_buffers_phys + (uint64_t)i * BUFFER_BYTES;
        device.rx_ring[i].read.header_addr = 0;
    }

    reg_write(REG_RDBAL, (uint32_t)device.rx_ring_phys);
    reg_write(REG_RDBAH, (uint32_t)(device.rx_ring_phys >> 32));
    reg_write(REG_RDLEN, (uint32_t)(sizeof(struct rx_desc) * RING_SIZE));
    reg_write(REG_SRRCTL,
              SRRCTL_BSIZE_2K | SRRCTL_DESC_ADV | SRRCTL_DROP_EN);
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, 0);
    reg_write(REG_RXDCTL, reg_read(REG_RXDCTL) | QUEUE_ENABLE);

    /* Warten, bis die Warteschlange laeuft. */
    for (int guard = 0; guard < 100000; guard++) {
        if (reg_read(REG_RXDCTL) & QUEUE_ENABLE)
            break;
        io_wait();
    }

    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);

    /* Der Schwanz zeigt hinter den letzten Platz, den die Karte
     * benutzen darf - alle bis auf einen. */
    reg_write(REG_RDT, RING_SIZE - 1);

    /* --- Senden --- */
    reg_write(REG_TCTL, 0);

    for (uint32_t i = 0; i < RING_SIZE; i++)
        device.tx_ring[i].status = TXD_STAT_DD;   /* alle frei */

    reg_write(REG_TDBAL, (uint32_t)device.tx_ring_phys);
    reg_write(REG_TDBAH, (uint32_t)(device.tx_ring_phys >> 32));
    reg_write(REG_TDLEN, (uint32_t)(sizeof(struct tx_desc) * RING_SIZE));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TXDCTL, reg_read(REG_TXDCTL) | QUEUE_ENABLE);

    for (int guard = 0; guard < 100000; guard++) {
        if (reg_read(REG_TXDCTL) & QUEUE_ENABLE)
            break;
        io_wait();
    }

    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP);
    reg_flush();

    device.rx_cursor = 0;
    device.tx_cursor = 0;

    strlcpy(nic->model, name, sizeof(nic->model));
    nic->ops = &igb_ops;
    nic->state = &device;
    nic->speed_mbit = link_speed();

    char mac[24];

    mac_format(&g_netif.mac, mac, sizeof(mac));
    kprintf("Netzwerk    : %s, %s\n", name, mac);
    return true;
}

const struct nic_driver igb_driver = {
    .family = "Intel igb",
    .probe  = igb_probe,
    .attach = igb_attach,
};
