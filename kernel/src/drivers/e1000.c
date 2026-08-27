/* e1000.c - Treiber fuer Intel-Netzwerkkarten der Reihe 8254x/8257x.
 *
 * Die Karte arbeitet mit zwei Ringpuffern aus Deskriptoren: einer fuer
 * ankommende, einer fuer abgehende Rahmen. Jeder Deskriptor nennt eine
 * physische Adresse und eine Laenge; die Karte holt und legt die Daten
 * selbst per DMA ab. Kernel und Karte zeigen mit je einem Zeiger (Kopf und
 * Schwanz) auf den Ring und teilen sich so mit, wie weit sie sind.
 *
 * Abgefragt wird im Betrieb, nicht ueber Interrupts: die Hauptschleife der
 * Oberflaeche schaut ohnehin tausendmal je Sekunde vorbei.
 *
 * Derselbe Deskriptoraufbau traegt auch die neueren Karten der Reihe
 * 8257x und I21x. Sie brauchen nur ein paar Register mehr - das
 * erledigt e1000e.c, das diese Datei mitbenutzt.
 */

#include "net.h"
#include "nic.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"

#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EERD     0x0014
#define REG_ICR      0x00C0
#define REG_IMS      0x00D0
#define REG_IMC      0x00D8
#define REG_RCTL     0x0100
#define REG_TCTL     0x0400
#define REG_TIPG     0x0410
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_TDBAL    0x3800
#define REG_TDBAH    0x3804
#define REG_TDLEN    0x3808
#define REG_TDH      0x3810
#define REG_TDT      0x3818
#define REG_RXDCTL   0x2828
#define REG_TXDCTL   0x3828
#define REG_MTA      0x5200
#define REG_RAL0     0x5400
#define REG_RAH0     0x5404

#define CTRL_SLU     (1u << 6)     /* Verbindung selbst herstellen */
#define CTRL_ASDE    (1u << 5)

#define RCTL_EN      (1u << 1)
#define RCTL_SBP     (1u << 2)
#define RCTL_UPE     (1u << 3)
#define RCTL_MPE     (1u << 4)
#define RCTL_BAM     (1u << 15)
#define RCTL_SECRC   (1u << 26)
#define RCTL_BSIZE_2048 0

#define TCTL_EN      (1u << 1)
#define TCTL_PSP     (1u << 3)

#define TXD_CMD_EOP  (1u << 0)
#define TXD_CMD_IFCS (1u << 1)
#define TXD_CMD_RS   (1u << 3)
#define TXD_STAT_DD  (1u << 0)

#define RXD_STAT_DD  (1u << 0)
#define RXD_STAT_EOP (1u << 1)

#define RX_RING_SIZE 32
#define TX_RING_SIZE 16
#define RX_BUFFER    2048

struct rx_desc {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} PACKED;

struct tx_desc {
    uint64_t address;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} PACKED;

static volatile uint8_t *mmio;
static struct rx_desc   *rx_ring;
static struct tx_desc   *tx_ring;
static uint8_t          *rx_buffers;
static uint8_t          *tx_buffers;
static uint64_t          rx_ring_phys, tx_ring_phys;
static uint64_t          rx_buffers_phys, tx_buffers_phys;
static uint32_t          rx_cursor, tx_cursor;
static bool              present;
static char              model_name[48];
static bool              needs_queue_enable;

static bool e1000_send(struct nic *nic, const void *frame, uint16_t length);
static uint16_t e1000_receive(struct nic *nic, void *buffer,
                              uint16_t capacity);
static bool e1000_link_up(struct nic *nic);

static const struct nic_ops e1000_ops = {
    .send    = e1000_send,
    .receive = e1000_receive,
    .link_up = e1000_link_up,
};

static uint32_t reg_read(uint32_t offset)
{
    return *(volatile uint32_t *)(mmio + offset);
}

static void reg_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(mmio + offset) = value;
}

/* Die MAC-Adresse steht nach dem Einschalten bereits im Adressfilter. */
static bool read_mac(void)
{
    uint32_t low  = reg_read(REG_RAL0);
    uint32_t high = reg_read(REG_RAH0);

    if (low == 0 && (high & 0xFFFF) == 0)
        return false;

    g_netif.mac.b[0] = (uint8_t)(low);
    g_netif.mac.b[1] = (uint8_t)(low >> 8);
    g_netif.mac.b[2] = (uint8_t)(low >> 16);
    g_netif.mac.b[3] = (uint8_t)(low >> 24);
    g_netif.mac.b[4] = (uint8_t)(high);
    g_netif.mac.b[5] = (uint8_t)(high >> 8);
    return true;
}

/* Aus dem Zustandsregister laesst sich ablesen, mit welchem Tempo die
 * Verbindung ausgehandelt wurde. */
static uint32_t link_speed(void)
{
    uint32_t status = reg_read(REG_STATUS);

    if (!(status & (1u << 1)))
        return 0;
    switch ((status >> 6) & 3) {
    case 0:  return 10;
    case 1:  return 100;
    default: return 1000;
    }
}

static bool setup_rings(void)
{
    /* Ein zusammenhaengender Block fuer beide Ringe und alle Puffer. */
    size_t ring_bytes = ALIGN_UP(RX_RING_SIZE * sizeof(struct rx_desc) +
                                 TX_RING_SIZE * sizeof(struct tx_desc), PAGE_SIZE);
    size_t rx_bytes   = (size_t)RX_RING_SIZE * RX_BUFFER;
    size_t tx_bytes   = (size_t)TX_RING_SIZE * RX_BUFFER;

    uint64_t phys = pmm_alloc_pages((ring_bytes + rx_bytes + tx_bytes) / PAGE_SIZE);
    if (!phys)
        return false;

    uint8_t *base = phys_to_virt(phys);
    memset(base, 0, ring_bytes + rx_bytes + tx_bytes);

    rx_ring      = (struct rx_desc *)base;
    rx_ring_phys = phys;
    tx_ring      = (struct tx_desc *)(base + RX_RING_SIZE * sizeof(struct rx_desc));
    tx_ring_phys = phys + RX_RING_SIZE * sizeof(struct rx_desc);

    rx_buffers      = base + ring_bytes;
    rx_buffers_phys = phys + ring_bytes;
    tx_buffers      = rx_buffers + rx_bytes;
    tx_buffers_phys = rx_buffers_phys + rx_bytes;

    for (int i = 0; i < RX_RING_SIZE; i++) {
        rx_ring[i].address = rx_buffers_phys + (uint64_t)i * RX_BUFFER;
        rx_ring[i].status  = 0;
    }
    for (int i = 0; i < TX_RING_SIZE; i++) {
        tx_ring[i].address = tx_buffers_phys + (uint64_t)i * RX_BUFFER;
        tx_ring[i].status  = TXD_STAT_DD;
    }
    return true;
}

/* Die Karten, die dieser Treiber unmittelbar kennt. Die uebrigen
 * Intel-Ethernetkarten uebernimmt e1000e.c. */
static const struct { uint16_t id; const char *name; } known[] = {
    { 0x100E, "Intel 82540EM" },
    { 0x100F, "Intel 82545EM" },
    { 0x1010, "Intel 82546EB" },
    { 0x1019, "Intel 82547EI" },
    { 0x1026, "Intel 82545GM" },
    { 0x107C, "Intel 82541PI" },
};

static bool e1000_probe(const struct pci_device *pci)
{
    if (pci->vendor_id != 0x8086)
        return false;
    for (size_t i = 0; i < ARRAY_LEN(known); i++)
        if (known[i].id == pci->device_id)
            return true;
    return false;
}

/* Nimmt die Karte in Betrieb. wide sagt, ob es eine der neueren ist,
 * die ihre Warteschlangen ausdruecklich freigeschaltet haben will. */
bool e1000_bring_up(const struct pci_device *dev, struct nic *nic,
                    bool queue_enable, const char *name)
{
    strlcpy(model_name, name, sizeof(model_name));
    needs_queue_enable = queue_enable;

    if (dev->bar[0] == 0 || dev->bar_is_io[0])
        return false;

    pci_enable_bus_master(dev);
    mmio = phys_to_virt(dev->bar[0]);

    reg_write(REG_IMC, 0xFFFFFFFF);        /* alle Interrupts abschalten */
    reg_read(REG_ICR);

    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    if (!read_mac()) {
        kprintf("Netzwerk    : keine MAC-Adresse gefunden\n");
        return false;
    }

    for (int i = 0; i < 128; i++)          /* Multicast-Tabelle leeren */
        reg_write(REG_MTA + (uint32_t)i * 4, 0);

    if (!setup_rings())
        return false;

    reg_write(REG_RDBAL, (uint32_t)(rx_ring_phys & 0xFFFFFFFF));
    reg_write(REG_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    reg_write(REG_RDLEN, RX_RING_SIZE * (uint32_t)sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RX_RING_SIZE - 1);
    if (needs_queue_enable)
        reg_write(REG_RXDCTL, reg_read(REG_RXDCTL) | (1u << 25));
    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);

    reg_write(REG_TDBAL, (uint32_t)(tx_ring_phys & 0xFFFFFFFF));
    reg_write(REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    reg_write(REG_TDLEN, TX_RING_SIZE * (uint32_t)sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    reg_write(REG_TIPG, 0x0060200A);
    if (needs_queue_enable)
        reg_write(REG_TXDCTL, reg_read(REG_TXDCTL) | (1u << 25));
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP | (0x0F << 4) | (0x40 << 12));

    rx_cursor = 0;
    tx_cursor = 0;
    present   = true;

    strlcpy(nic->model, model_name, sizeof(nic->model));
    nic->ops = &e1000_ops;
    nic->speed_mbit = link_speed();

    char mac[24];

    mac_format(&g_netif.mac, mac, sizeof(mac));
    kprintf("Netzwerk    : %s, %s\n", model_name, mac);
    return true;
}

static bool e1000_attach(const struct pci_device *pci, struct nic *nic)
{
    const char *name = "Intel-Netzwerkkarte";

    for (size_t i = 0; i < ARRAY_LEN(known); i++)
        if (known[i].id == pci->device_id)
            name = known[i].name;

    /* Die alten 8254x kennen das Freigabebit der Warteschlangen nicht. */
    return e1000_bring_up(pci, nic, false, name);
}

static bool e1000_send(struct nic *nic, const void *frame, uint16_t length)
{
    UNUSED(nic);
    if (!present || length == 0 || length > RX_BUFFER)
        return false;

    struct tx_desc *desc = &tx_ring[tx_cursor];

    /* Warten, bis die Karte den Platz freigegeben hat. */
    for (int guard = 0; guard < 1000000 && !(desc->status & TXD_STAT_DD); guard++)
        io_wait();

    if (!(desc->status & TXD_STAT_DD))
        return false;

    memcpy(tx_buffers + (size_t)tx_cursor * RX_BUFFER, frame, length);

    desc->length = length;
    desc->cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    desc->status = 0;

    tx_cursor = (tx_cursor + 1) % TX_RING_SIZE;
    reg_write(REG_TDT, tx_cursor);

    g_netif.tx_packets++;
    g_netif.tx_bytes += length;
    return true;
}

static uint16_t e1000_receive(struct nic *nic, void *buffer,
                              uint16_t capacity)
{
    UNUSED(nic);
    if (!present)
        return 0;

    struct rx_desc *desc = &rx_ring[rx_cursor];

    if (!(desc->status & RXD_STAT_DD))
        return 0;

    uint16_t length = desc->length;
    uint16_t copied = 0;

    if (length > 0 && length <= capacity) {
        memcpy(buffer, rx_buffers + (size_t)rx_cursor * RX_BUFFER, length);
        copied = length;
        g_netif.rx_packets++;
        g_netif.rx_bytes += length;
    } else {
        g_netif.rx_dropped++;
    }

    desc->status = 0;
    reg_write(REG_RDT, rx_cursor);
    rx_cursor = (rx_cursor + 1) % RX_RING_SIZE;

    return copied;
}

static bool e1000_link_up(struct nic *nic)
{
    UNUSED(nic);
    return present && (reg_read(REG_STATUS) & (1u << 1)) != 0;
}

const struct nic_driver e1000_driver = {
    .family = "Intel 8254x",
    .probe  = e1000_probe,
    .attach = e1000_attach,
};
