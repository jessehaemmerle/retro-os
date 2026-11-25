/* vmxnet3.c - die Netzwerkkarte von VMware (auch in QEMU nachgebildet).
 *
 * Wie bei virtio: keine Register, die es nicht gibt, sondern
 * Warteschlangen im Speicher. Der Gast legt eine Beschreibung von 720
 * Byte an, in der steht, wo seine Ringe liegen und wie gross sie sind,
 * nennt dem Geraet ihre Adresse und startet es mit einem Befehl. Danach
 * genuegt ein Schreiben auf den "Klingelknopf", wenn ein neuer Eintrag
 * im Ring steht.
 *
 * Ob ein Ringeintrag gilt, entscheidet ein Generationsbit: Beide Seiten
 * zaehlen im Kreis, und bei jedem Umlauf kippt das Bit. Der Gast
 * schreibt es als Letztes in den Eintrag - erst dann sieht das Geraet
 * ihn.
 *
 * Eine Sende- und eine Empfangswarteschlange, ohne Unterbrechungen: Der
 * Kern schaut nach, wie bei den anderen Karten auch.
 */

#include "net.h"
#include "nic.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"
#include "vmxnet3hw.h"

#define TX_SIZE      64
#define RX0_SIZE     64
#define RX1_SIZE     32               /* wird nie gefuellt */
#define BUF_BYTES    2048

#define PAGES_FIXED  7

struct vmxnet3 {
    volatile uint8_t *bar0;
    volatile uint8_t *bar1;

    uint64_t   phys;
    size_t     pages;

    struct vmx_shared        *shared;
    struct vmx_tx_queue_desc *txq;
    struct vmx_rx_queue_desc *rxq;

    uint32_t  *tx_ring, *tx_comp, *rx_ring0, *rx_ring1, *rx_comp;
    uint8_t   *tx_buf, *rx_buf;
    uint64_t   tx_buf_phys, rx_buf_phys;

    struct vmx_ring tx, tx_c, rx0, rx_c;
    uint32_t        tx_in_flight;
};

static struct vmxnet3 dev;

static uint32_t rd1(uint32_t reg) { return *(volatile uint32_t *)(dev.bar1 + reg); }
static void     wr1(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(dev.bar1 + reg) = v; }
static void     wr0(uint32_t reg, uint32_t v) { *(volatile uint32_t *)(dev.bar0 + reg) = v; }

static uint32_t command(uint32_t cmd)
{
    wr1(VMX_REG_CMD, cmd);
    return rd1(VMX_REG_CMD);
}

static void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* Legt einen Empfangspuffer in den Ring (ohne Klingeln). */
static void rx_post(void)
{
    uint32_t at = dev.rx0.next;
    uint32_t d[4];

    vmx_rx_desc(d, dev.rx_buf_phys + (uint64_t)at * BUF_BYTES, BUF_BYTES,
                dev.rx0.gen);

    uint32_t *slot = dev.rx_ring0 + (size_t)at * 4;

    slot[0] = d[0];
    slot[1] = d[1];
    slot[3] = d[3];
    barrier();
    slot[2] = d[2];                      /* mit dem Generationsbit zuletzt */
    barrier();

    vmx_ring_advance(&dev.rx0);
}

/* Holt erledigte Sendeauftraege ab. */
static void tx_reap(void)
{
    for (;;) {
        const uint32_t *c = dev.tx_comp + (size_t)dev.tx_c.next * 4;

        if (vmx_comp_gen(c) != dev.tx_c.gen)
            break;
        if (dev.tx_in_flight)
            dev.tx_in_flight--;
        vmx_ring_advance(&dev.tx_c);
    }
}

static bool vmxnet3_send(struct nic *nic, const void *frame, uint16_t length)
{
    UNUSED(nic);

    if (length == 0 || length > BUF_BYTES)
        return false;

    tx_reap();
    for (int guard = 0; guard < 1000000 && dev.tx_in_flight >= TX_SIZE - 1; guard++) {
        tx_reap();
        __asm__ volatile("pause");
    }
    if (dev.tx_in_flight >= TX_SIZE - 1)
        return false;

    uint32_t at = dev.tx.next;
    uint8_t *buf = dev.tx_buf + (size_t)at * BUF_BYTES;
    uint16_t padded = length;

    memcpy(buf, frame, length);
    if (padded < 60) {
        memset(buf + length, 0, (size_t)(60 - length));
        padded = 60;
    }

    uint32_t d[4];

    vmx_tx_desc(d, dev.tx_buf_phys + (uint64_t)at * BUF_BYTES, padded,
                dev.tx.gen);

    uint32_t *slot = dev.tx_ring + (size_t)at * 4;

    slot[0] = d[0];
    slot[1] = d[1];
    slot[3] = d[3];
    barrier();
    slot[2] = d[2];
    barrier();

    vmx_ring_advance(&dev.tx);
    dev.tx_in_flight++;
    wr0(VMX_REG_TXPROD, dev.tx.next);

    g_netif.tx_packets++;
    g_netif.tx_bytes += length;
    return true;
}

static uint16_t vmxnet3_receive(struct nic *nic, void *buffer, uint16_t capacity)
{
    UNUSED(nic);

    const uint32_t *c = dev.rx_comp + (size_t)dev.rx_c.next * 4;

    if (vmx_comp_gen(c) != dev.rx_c.gen)
        return 0;                        /* nichts da */

    barrier();

    struct vmx_rx_comp info;
    uint32_t words[4] = { c[0], c[1], c[2], c[3] };

    vmx_rx_comp_decode(words, &info);
    vmx_ring_advance(&dev.rx_c);

    uint16_t copied = 0;

    /* Nur Kopfpuffer aus Ring 0, ein Stueck je Rahmen - die Puffer
     * sind groesser als jeder Rahmen. */
    if (info.queue_id == 0 && info.index < RX0_SIZE && info.sop && info.eop &&
        !info.error && info.length > 0 && info.length <= capacity) {
        memcpy(buffer, dev.rx_buf + (size_t)info.index * BUF_BYTES, info.length);
        copied = (uint16_t)info.length;
        g_netif.rx_packets++;
        g_netif.rx_bytes += info.length;
    } else {
        g_netif.rx_dropped++;
    }

    /* Einen frischen Puffer hinlegen und dem Geraet Bescheid sagen. */
    if (info.queue_id == 0) {
        rx_post();
        wr0(VMX_REG_RXPROD, dev.rx0.next);
    }
    return copied;
}

static bool vmxnet3_link_up(struct nic *nic)
{
    UNUSED(nic);
    return vmx_link_up(command(VMX_CMD_GET_LINK));
}

static const struct nic_ops vmxnet3_ops = {
    .send    = vmxnet3_send,
    .receive = vmxnet3_receive,
    .link_up = vmxnet3_link_up,
};

static bool vmxnet3_probe(const struct pci_device *pci)
{
    return pci->vendor_id == 0x15AD && pci->device_id == 0x07B0;
}

static bool vmxnet3_attach(const struct pci_device *pci, struct nic *nic)
{
    memset(&dev, 0, sizeof(dev));

    if (pci->bar_is_io[0] || pci->bar_is_io[1] || !pci->bar[0] || !pci->bar[1])
        return false;

    pci_enable_bus_master(pci);
    dev.bar0 = phys_to_virt(pci->bar[0]);
    dev.bar1 = phys_to_virt(pci->bar[1]);

    /* Bauform 1 und UPT-Version 1 waehlen - wenn das Geraet sie kennt. */
    if (!(rd1(VMX_REG_VRRS) & 1u) || !(rd1(VMX_REG_UVRS) & 1u)) {
        kprintf("Netzwerk    : vmxnet3 kennt Bauform 1 nicht\n");
        return false;
    }
    wr1(VMX_REG_VRRS, 1);
    wr1(VMX_REG_UVRS, 1);

    (void)command(VMX_CMD_RESET);

    /* MAC-Adresse: zwei Register. */
    uint32_t low = rd1(VMX_REG_MACL);
    uint32_t high = rd1(VMX_REG_MACH);

    g_netif.mac.b[0] = (uint8_t)low;
    g_netif.mac.b[1] = (uint8_t)(low >> 8);
    g_netif.mac.b[2] = (uint8_t)(low >> 16);
    g_netif.mac.b[3] = (uint8_t)(low >> 24);
    g_netif.mac.b[4] = (uint8_t)high;
    g_netif.mac.b[5] = (uint8_t)(high >> 8);
    if (low == 0 && (high & 0xFFFF) == 0)
        return false;

    /* Speicher in einem Stueck: sieben feste Seiten fuer Beschreibung,
     * Warteschlangenbeschreibung und die fuenf Ringe, danach die
     * Puffer. */
    dev.pages = PAGES_FIXED + 2 * (TX_SIZE * BUF_BYTES / PAGE_SIZE);
    dev.phys = pmm_alloc_pages(dev.pages);
    if (!dev.phys)
        return false;

    uint8_t *base = phys_to_virt(dev.phys);

    memset(base, 0, dev.pages * PAGE_SIZE);

    dev.shared   = (struct vmx_shared *)(base + 0 * PAGE_SIZE);
    dev.txq      = (struct vmx_tx_queue_desc *)(base + 1 * PAGE_SIZE);
    dev.rxq      = (struct vmx_rx_queue_desc *)((uint8_t *)dev.txq +
                                                sizeof(struct vmx_tx_queue_desc));
    dev.tx_ring  = (uint32_t *)(base + 2 * PAGE_SIZE);
    dev.tx_comp  = (uint32_t *)(base + 3 * PAGE_SIZE);
    dev.rx_ring0 = (uint32_t *)(base + 4 * PAGE_SIZE);
    dev.rx_ring1 = (uint32_t *)(base + 5 * PAGE_SIZE);
    dev.rx_comp  = (uint32_t *)(base + 6 * PAGE_SIZE);
    dev.tx_buf   = base + PAGES_FIXED * PAGE_SIZE;
    dev.rx_buf   = dev.tx_buf + (size_t)TX_SIZE * BUF_BYTES;
    dev.tx_buf_phys = dev.phys + PAGES_FIXED * PAGE_SIZE;
    dev.rx_buf_phys = dev.tx_buf_phys + (uint64_t)TX_SIZE * BUF_BYTES;

    vmx_ring_init(&dev.tx, TX_SIZE);
    vmx_ring_init(&dev.tx_c, TX_SIZE);
    vmx_ring_init(&dev.rx0, RX0_SIZE);
    vmx_ring_init(&dev.rx_c, RX0_SIZE + RX1_SIZE);

    /* --- Warteschlangenbeschreibung --- */
    dev.txq->tx_ring_pa = dev.phys + 2 * PAGE_SIZE;
    dev.txq->comp_ring_pa = dev.phys + 3 * PAGE_SIZE;
    dev.txq->tx_ring_size = TX_SIZE;
    dev.txq->comp_ring_size = TX_SIZE;
    dev.txq->tx_threshold = 1;
    dev.txq->intr_idx = 0;

    dev.rxq->rx_ring_pa[0] = dev.phys + 4 * PAGE_SIZE;
    dev.rxq->rx_ring_pa[1] = dev.phys + 5 * PAGE_SIZE;
    dev.rxq->comp_ring_pa = dev.phys + 6 * PAGE_SIZE;
    dev.rxq->rx_ring_size[0] = RX0_SIZE;
    dev.rxq->rx_ring_size[1] = RX1_SIZE;
    dev.rxq->comp_ring_size = RX0_SIZE + RX1_SIZE;
    dev.rxq->intr_idx = 0;
    dev.rxq->update_rx_prod = 1;

    /* --- Hauptbeschreibung --- */
    struct vmx_shared *s = dev.shared;

    s->magic = VMX_MAGIC;
    s->dev_read.misc.info.version = 0x01040A00u;
    s->dev_read.misc.info.gos = vmx_gos_word();
    s->dev_read.misc.info.rev_supported = 1;
    s->dev_read.misc.info.upt_supported = 1;
    s->dev_read.misc.queue_desc_pa = dev.phys + 1 * PAGE_SIZE;
    s->dev_read.misc.queue_desc_len =
        sizeof(struct vmx_tx_queue_desc) + sizeof(struct vmx_rx_queue_desc);
    s->dev_read.misc.mtu = 1500;
    s->dev_read.misc.max_rx_sg = 1;
    s->dev_read.misc.num_tx_queues = 1;
    s->dev_read.misc.num_rx_queues = 1;

    s->dev_read.intr.num_intrs = 1;
    s->dev_read.intr.event_intr_idx = 0;
    s->dev_read.intr.intr_ctrl = VMX_IC_DISABLE_ALL;

    s->dev_read.rx_filter.rx_mode =
        VMX_RXM_UCAST | VMX_RXM_BCAST | VMX_RXM_ALL_MULTI;

    wr1(VMX_REG_DSAL, (uint32_t)dev.phys);
    wr1(VMX_REG_DSAH, (uint32_t)(dev.phys >> 32));

    if (command(VMX_CMD_ACTIVATE) != 0) {
        kprintf("Netzwerk    : vmxnet3 laesst sich nicht aktivieren\n");
        pmm_free_pages(dev.phys, dev.pages);
        return false;
    }

    wr0(VMX_REG_IMR, 1);                 /* Unterbrechung 0 sperren */

    /* Empfangspuffer hinlegen - einen Platz laesst man frei, sonst
     * waeren voller und leerer Ring nicht zu unterscheiden. */
    for (uint32_t i = 0; i < RX0_SIZE - 1; i++)
        rx_post();
    wr0(VMX_REG_RXPROD, dev.rx0.next);

    (void)command(VMX_CMD_UPDATE_RXMODE);

    uint32_t link = command(VMX_CMD_GET_LINK);

    strlcpy(nic->model, "VMware vmxnet3", sizeof(nic->model));
    nic->ops = &vmxnet3_ops;
    nic->state = &dev;
    nic->speed_mbit = vmx_link_speed(link);

    char text[24];

    mac_format(&g_netif.mac, text, sizeof(text));
    kprintf("Netzwerk    : VMware vmxnet3, %s\n", text);
    return true;
}

const struct nic_driver vmxnet3_driver = {
    .family = "VMware vmxnet3",
    .probe  = vmxnet3_probe,
    .attach = vmxnet3_attach,
};
