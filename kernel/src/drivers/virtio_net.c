/* virtio_net.c - die Netzwerkkarte der virtuellen Maschinen.
 *
 * virtio ist kein nachgebauter Chip, sondern eine Abmachung zwischen
 * Gast und Wirt: Statt Register zu kitzeln, die es in Wahrheit gar
 * nicht gibt, legt der Gast Ringe im Speicher an und sagt kurz
 * Bescheid, wenn etwas darin steht. Das spart der virtuellen Maschine
 * das muehsame Nachspielen echter Hardware und ist darum in KVM,
 * Proxmox, QEMU und den grossen Rechenzentren der uebliche Weg.
 *
 * Ein Ring besteht aus drei Teilen:
 *
 *   Deskriptoren   je sechzehn Byte: Adresse, Laenge, Merkmale, Nachfolger
 *   avail          was der Gast dem Wirt hinlegt
 *   used           was der Wirt zurueckgibt, mit der geschriebenen Laenge
 *
 * Beide Seiten fuehren nur einen Zaehler, der immer weiterlaeuft; der
 * Rest um die Ringgroesse geteilt sagt, wo der Eintrag steht.
 *
 * Es gibt zwei Bauformen. Die alte legt alles auf Ein-/Ausgabeadressen,
 * die neue (virtio 1.0) auf Speicherbereiche, die ueber besondere
 * PCI-Faehigkeiten angekuendigt werden. Dieser Treiber kann beide -
 * er nimmt die neue, wenn die Karte sie anbietet.
 */

#include "net.h"
#include "nic.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"

/* --- Zustandsbits, die der Gast der Karte meldet --- */
#define ST_ACKNOWLEDGE  0x01
#define ST_DRIVER       0x02
#define ST_DRIVER_OK    0x04
#define ST_FEATURES_OK  0x08
#define ST_FAILED       0x80

/* --- Merkmale, die wir annehmen --- */
#define F_NET_MAC       (1ull << 5)
#define F_NET_STATUS    (1ull << 16)
#define F_ANY_LAYOUT    (1ull << 27)
#define F_VERSION_1     (1ull << 32)

/* --- Deskriptor-Merkmale --- */
#define DESC_NEXT       1
#define DESC_WRITE      2

/* --- Faehigkeiten der neuen Bauform --- */
#define VIRTIO_CAP      0x09     /* herstellereigene PCI-Faehigkeit */
#define CFG_COMMON      1
#define CFG_NOTIFY      2
#define CFG_ISR         3
#define CFG_DEVICE      4

/* Wie viele Plaetze wir tatsaechlich benutzen. Der Ring der Karte ist
 * meist groesser; niemand zwingt uns, ihn auszureizen. */
#define RX_SLOTS        32
#define TX_SLOTS        16
#define BUFFER_BYTES    2048
#define HEADER_STRIDE   16       /* der Kopf ist zehn oder zwoelf Byte */

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

struct virtqueue {
    uint16_t             size;        /* Plaetze im Ring der Karte   */
    struct vring_desc   *desc;
    struct vring_avail  *avail;
    struct vring_used   *used;
    uint64_t             phys;        /* Anfang des ganzen Blocks    */
    size_t               pages;
    uint16_t             last_used;
    uint16_t             notify_off;
};

struct virtio_net {
    const struct pci_device *pci;

    bool      modern;
    uint16_t  io;                     /* alte Bauform                */

    volatile uint8_t *common;         /* neue Bauform                */
    volatile uint8_t *notify;
    volatile uint8_t *isr;
    volatile uint8_t *config;
    uint32_t  notify_multiplier;

    uint64_t  features;
    size_t    header_bytes;

    struct virtqueue rx;
    struct virtqueue tx;

    uint8_t  *rx_headers, *rx_data;
    uint8_t  *tx_headers, *tx_data;
    uint64_t  rx_headers_phys, rx_data_phys;
    uint64_t  tx_headers_phys, tx_data_phys;
    uint64_t  buffer_phys;
    size_t    buffer_pages;

    bool      tx_busy[TX_SLOTS];
    uint16_t  tx_next;
};

static struct virtio_net device;

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* Vor dem Anstossen muessen die Schreibvorgaenge sichtbar sein - sonst
 * liest der Wirt einen halb gefuellten Ring. */
static inline void write_fence(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* Zugriff auf die Karte - beide Bauformen unter einem Dach            */
/* ------------------------------------------------------------------ */

static inline uint8_t m8(volatile uint8_t *p)   { return *p; }
static inline uint16_t m16(volatile uint8_t *p) { return *(volatile uint16_t *)p; }
static inline uint32_t m32(volatile uint8_t *p) { return *(volatile uint32_t *)p; }
static inline void wm8(volatile uint8_t *p, uint8_t v)   { *p = v; }
static inline void wm16(volatile uint8_t *p, uint16_t v) { *(volatile uint16_t *)p = v; }
static inline void wm32(volatile uint8_t *p, uint32_t v) { *(volatile uint32_t *)p = v; }

/* Versaetze im gemeinsamen Bereich der neuen Bauform. */
#define COMMON_DEV_FEATURE_SEL  0x00
#define COMMON_DEV_FEATURE      0x04
#define COMMON_DRV_FEATURE_SEL  0x08
#define COMMON_DRV_FEATURE      0x0C
#define COMMON_MSIX_CONFIG      0x10
#define COMMON_NUM_QUEUES       0x12
#define COMMON_STATUS           0x14
#define COMMON_GENERATION       0x15
#define COMMON_QUEUE_SELECT     0x16
#define COMMON_QUEUE_SIZE       0x18
#define COMMON_QUEUE_MSIX       0x1A
#define COMMON_QUEUE_ENABLE     0x1C
#define COMMON_QUEUE_NOTIFY_OFF 0x1E
#define COMMON_QUEUE_DESC       0x20
#define COMMON_QUEUE_AVAIL      0x28
#define COMMON_QUEUE_USED       0x30

/* Versaetze der alten Bauform, alle als Ein-/Ausgabeadresse. */
#define LEGACY_DEV_FEATURES     0x00
#define LEGACY_DRV_FEATURES     0x04
#define LEGACY_QUEUE_PFN        0x08
#define LEGACY_QUEUE_SIZE       0x0C
#define LEGACY_QUEUE_SELECT     0x0E
#define LEGACY_QUEUE_NOTIFY     0x10
#define LEGACY_STATUS           0x12
#define LEGACY_ISR              0x13
#define LEGACY_CONFIG           0x14

static uint8_t status_get(void)
{
    if (device.modern)
        return m8(device.common + COMMON_STATUS);
    return inb((uint16_t)(device.io + LEGACY_STATUS));
}

static void status_add(uint8_t bits)
{
    uint8_t now = (uint8_t)(status_get() | bits);

    if (device.modern)
        wm8(device.common + COMMON_STATUS, now);
    else
        outb((uint16_t)(device.io + LEGACY_STATUS), now);
}

static void status_reset(void)
{
    if (device.modern)
        wm8(device.common + COMMON_STATUS, 0);
    else
        outb((uint16_t)(device.io + LEGACY_STATUS), 0);

    /* Die Karte bestaetigt den Ruecksetzer, indem sie null liest. */
    for (int guard = 0; guard < 100000 && status_get() != 0; guard++)
        io_wait();
}

static uint64_t features_offered(void)
{
    if (!device.modern)
        return inl((uint16_t)(device.io + LEGACY_DEV_FEATURES));

    wm32(device.common + COMMON_DEV_FEATURE_SEL, 0);
    uint64_t low = m32(device.common + COMMON_DEV_FEATURE);

    wm32(device.common + COMMON_DEV_FEATURE_SEL, 1);
    uint64_t high = m32(device.common + COMMON_DEV_FEATURE);

    return low | (high << 32);
}

static void features_accept(uint64_t bits)
{
    if (!device.modern) {
        outl((uint16_t)(device.io + LEGACY_DRV_FEATURES), (uint32_t)bits);
        return;
    }

    wm32(device.common + COMMON_DRV_FEATURE_SEL, 0);
    wm32(device.common + COMMON_DRV_FEATURE, (uint32_t)bits);
    wm32(device.common + COMMON_DRV_FEATURE_SEL, 1);
    wm32(device.common + COMMON_DRV_FEATURE, (uint32_t)(bits >> 32));
}

static uint8_t config_byte(size_t offset)
{
    if (device.modern)
        return m8(device.config + offset);
    return inb((uint16_t)(device.io + LEGACY_CONFIG + offset));
}

/* ------------------------------------------------------------------ */
/* Ringe anlegen                                                       */
/* ------------------------------------------------------------------ */

/* Deskriptoren und avail liegen zusammen, used faengt an der naechsten
 * Seitengrenze an - so verlangt es die alte Bauform, und der neuen ist
 * es recht. */
static size_t ring_bytes(uint16_t size, size_t *used_offset)
{
    size_t front = (size_t)16 * size + 6 + (size_t)2 * size;
    size_t back  = 6 + (size_t)8 * size;

    *used_offset = ALIGN_UP(front, PAGE_SIZE);
    return *used_offset + ALIGN_UP(back, PAGE_SIZE);
}

static uint16_t queue_size_of(uint16_t index)
{
    if (device.modern) {
        wm16(device.common + COMMON_QUEUE_SELECT, index);
        return m16(device.common + COMMON_QUEUE_SIZE);
    }
    outw((uint16_t)(device.io + LEGACY_QUEUE_SELECT), index);
    return inw((uint16_t)(device.io + LEGACY_QUEUE_SIZE));
}

static bool queue_setup(struct virtqueue *q, uint16_t index, uint16_t needed)
{
    uint16_t size = queue_size_of(index);

    if (size == 0 || size < needed)
        return false;

    /* Die neue Bauform laesst uns den Ring verkleinern; das spart
     * Speicher, weil wir ohnehin nur wenige Plaetze belegen. */
    if (device.modern && size > needed) {
        wm16(device.common + COMMON_QUEUE_SIZE, needed);
        size = m16(device.common + COMMON_QUEUE_SIZE);
        if (size < needed)
            return false;
    }

    size_t used_offset;
    size_t bytes = ring_bytes(size, &used_offset);
    size_t pages = bytes / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);

    if (!phys)
        return false;

    memset(phys_to_virt(phys), 0, bytes);

    q->size  = size;
    q->phys  = phys;
    q->pages = pages;
    q->desc  = phys_to_virt(phys);
    q->avail = (struct vring_avail *)((uint8_t *)q->desc + 16 * (size_t)size);
    q->used  = (struct vring_used *)((uint8_t *)q->desc + used_offset);
    q->last_used = 0;

    if (device.modern) {
        wm16(device.common + COMMON_QUEUE_SELECT, index);
        wm32(device.common + COMMON_QUEUE_DESC, (uint32_t)phys);
        wm32(device.common + COMMON_QUEUE_DESC + 4, (uint32_t)(phys >> 32));

        uint64_t avail_phys = phys + 16 * (uint64_t)size;

        wm32(device.common + COMMON_QUEUE_AVAIL, (uint32_t)avail_phys);
        wm32(device.common + COMMON_QUEUE_AVAIL + 4,
             (uint32_t)(avail_phys >> 32));

        uint64_t used_phys = phys + used_offset;

        wm32(device.common + COMMON_QUEUE_USED, (uint32_t)used_phys);
        wm32(device.common + COMMON_QUEUE_USED + 4,
             (uint32_t)(used_phys >> 32));

        q->notify_off = m16(device.common + COMMON_QUEUE_NOTIFY_OFF);
        wm16(device.common + COMMON_QUEUE_MSIX, 0xFFFF);   /* keine */
        wm16(device.common + COMMON_QUEUE_ENABLE, 1);
    } else {
        /* Die alte Bauform kennt nur eine Seitennummer - dafuer muss
         * der Ring unterhalb von 16 TiB liegen, was er immer tut. */
        outw((uint16_t)(device.io + LEGACY_QUEUE_SELECT), index);
        outl((uint16_t)(device.io + LEGACY_QUEUE_PFN),
             (uint32_t)(phys / PAGE_SIZE));
        q->notify_off = index;
    }
    return true;
}

static void queue_notify(struct virtqueue *q, uint16_t index)
{
    write_fence();

    if (device.modern)
        wm16(device.notify + (size_t)q->notify_off * device.notify_multiplier,
             index);
    else
        outw((uint16_t)(device.io + LEGACY_QUEUE_NOTIFY), index);
}

/* Legt eine Kette in den avail-Ring. */
static void queue_offer(struct virtqueue *q, uint16_t head)
{
    q->avail->ring[q->avail->idx % q->size] = head;
    write_fence();
    q->avail->idx++;
    barrier();
}

/* ------------------------------------------------------------------ */
/* Betrieb                                                             */
/* ------------------------------------------------------------------ */

/* Holt zurueckgegebene Sendeplaetze ab. */
static void tx_reclaim(void)
{
    barrier();

    while (device.tx.last_used != device.tx.used->idx) {
        struct vring_used_elem *elem =
            &device.tx.used->ring[device.tx.last_used % device.tx.size];
        uint16_t slot = (uint16_t)(elem->id / 2);

        if (slot < TX_SLOTS)
            device.tx_busy[slot] = false;
        device.tx.last_used++;
    }
}

static bool virtio_send(struct nic *nic, const void *frame, uint16_t length)
{
    UNUSED(nic);

    if (length == 0 || length > BUFFER_BYTES)
        return false;

    tx_reclaim();

    /* Auf einen freien Platz warten - der Wirt raeumt sie zuegig ab. */
    for (int guard = 0; guard < 1000000 && device.tx_busy[device.tx_next];
         guard++) {
        io_wait();
        tx_reclaim();
    }
    if (device.tx_busy[device.tx_next])
        return false;

    uint16_t slot = device.tx_next;
    uint16_t head = (uint16_t)(slot * 2);

    /* Der Kopf beschreibt Pruefsummen und Segmentierung - wir nutzen
     * beides nicht, also bleibt er leer. */
    memset(device.tx_headers + (size_t)slot * HEADER_STRIDE, 0,
           device.header_bytes);
    memcpy(device.tx_data + (size_t)slot * BUFFER_BYTES, frame, length);

    device.tx.desc[head].addr =
        device.tx_headers_phys + (uint64_t)slot * HEADER_STRIDE;
    device.tx.desc[head].len = (uint32_t)device.header_bytes;
    device.tx.desc[head].flags = DESC_NEXT;
    device.tx.desc[head].next = (uint16_t)(head + 1);

    device.tx.desc[head + 1].addr =
        device.tx_data_phys + (uint64_t)slot * BUFFER_BYTES;
    device.tx.desc[head + 1].len = length;
    device.tx.desc[head + 1].flags = 0;
    device.tx.desc[head + 1].next = 0;

    device.tx_busy[slot] = true;
    device.tx_next = (uint16_t)((slot + 1) % TX_SLOTS);

    queue_offer(&device.tx, head);
    queue_notify(&device.tx, 1);

    g_netif.tx_packets++;
    g_netif.tx_bytes += length;
    return true;
}

static uint16_t virtio_receive(struct nic *nic, void *buffer,
                               uint16_t capacity)
{
    UNUSED(nic);

    barrier();
    if (device.rx.last_used == device.rx.used->idx)
        return 0;

    struct vring_used_elem *elem =
        &device.rx.used->ring[device.rx.last_used % device.rx.size];
    uint16_t head = (uint16_t)elem->id;
    uint32_t written = elem->len;
    uint16_t slot = (uint16_t)(head / 2);
    uint16_t copied = 0;

    if (slot < RX_SLOTS && written > device.header_bytes) {
        uint32_t payload = written - (uint32_t)device.header_bytes;

        if (payload > BUFFER_BYTES)
            payload = BUFFER_BYTES;

        if (payload <= capacity) {
            memcpy(buffer, device.rx_data + (size_t)slot * BUFFER_BYTES,
                   payload);
            copied = (uint16_t)payload;
            g_netif.rx_packets++;
            g_netif.rx_bytes += payload;
        } else {
            g_netif.rx_dropped++;
        }
    } else {
        g_netif.rx_dropped++;
    }

    device.rx.last_used++;

    /* Den Platz sofort wieder hinlegen, sonst laeuft der Ring leer. */
    if (slot < RX_SLOTS) {
        queue_offer(&device.rx, head);
        queue_notify(&device.rx, 0);
    }
    return copied;
}

static bool virtio_link_up(struct nic *nic)
{
    UNUSED(nic);

    if (!(device.features & F_NET_STATUS))
        return true;             /* die Karte sagt nichts dazu */

    /* Der Zustand steht hinter der Adresse im Geraetebereich. */
    return (config_byte(6) & 1u) != 0;
}

static const struct nic_ops virtio_ops = {
    .send    = virtio_send,
    .receive = virtio_receive,
    .link_up = virtio_link_up,
};

/* ------------------------------------------------------------------ */
/* Einrichten                                                          */
/* ------------------------------------------------------------------ */

/* Die neue Bauform kuendigt ihre Bereiche ueber herstellereigene
 * PCI-Faehigkeiten an. Davon gibt es mehrere, deshalb reicht das
 * allgemeine pci_find_capability hier nicht - wir laufen die Liste
 * selbst ab und sortieren nach der Art des Bereichs. */
static bool map_modern(const struct pci_device *pci)
{
    uint16_t status = pci_read16(pci, 0x06);

    if (!(status & (1 << 4)))
        return false;

    uint8_t offset = (uint8_t)(pci_read32(pci, 0x34) & 0xFC);

    for (int guard = 0; guard < 48 && offset >= 0x40; guard++) {
        uint32_t head = pci_read32(pci, offset);
        uint8_t  id   = (uint8_t)(head & 0xFF);
        uint8_t  next = (uint8_t)((head >> 8) & 0xFC);

        if (id == VIRTIO_CAP) {
            uint8_t  type = (uint8_t)((head >> 24) & 0xFF);
            uint32_t word = pci_read32(pci, (uint8_t)(offset + 4));
            uint8_t  bar  = (uint8_t)(word & 0xFF);
            uint32_t area = pci_read32(pci, (uint8_t)(offset + 8));

            if (bar < 6 && pci->bar[bar] && !pci->bar_is_io[bar]) {
                volatile uint8_t *base =
                    (volatile uint8_t *)phys_to_virt(pci->bar[bar]) + area;

                switch (type) {
                case CFG_COMMON: device.common = base; break;
                case CFG_NOTIFY:
                    device.notify = base;
                    device.notify_multiplier =
                        pci_read32(pci, (uint8_t)(offset + 16));
                    break;
                case CFG_ISR:    device.isr = base;    break;
                case CFG_DEVICE: device.config = base; break;
                default: break;
                }
            }
        }

        if (!next)
            break;
        offset = next;
    }

    return device.common && device.notify && device.config;
}

static bool buffers_setup(void)
{
    size_t header_bytes = (size_t)(RX_SLOTS + TX_SLOTS) * HEADER_STRIDE;
    size_t data_bytes   = (size_t)(RX_SLOTS + TX_SLOTS) * BUFFER_BYTES;
    size_t total = ALIGN_UP(header_bytes + data_bytes, PAGE_SIZE);
    size_t pages = total / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);

    if (!phys)
        return false;

    memset(phys_to_virt(phys), 0, total);

    device.buffer_phys = phys;
    device.buffer_pages = pages;

    device.rx_headers_phys = phys;
    device.tx_headers_phys = phys + (uint64_t)RX_SLOTS * HEADER_STRIDE;
    device.rx_data_phys    = phys + header_bytes;
    device.tx_data_phys    = device.rx_data_phys +
                             (uint64_t)RX_SLOTS * BUFFER_BYTES;

    device.rx_headers = phys_to_virt(device.rx_headers_phys);
    device.tx_headers = phys_to_virt(device.tx_headers_phys);
    device.rx_data    = phys_to_virt(device.rx_data_phys);
    device.tx_data    = phys_to_virt(device.tx_data_phys);
    return true;
}

/* Alle Empfangsplaetze verketten und hinlegen. */
static void rx_prime(void)
{
    for (uint16_t slot = 0; slot < RX_SLOTS; slot++) {
        uint16_t head = (uint16_t)(slot * 2);

        device.rx.desc[head].addr =
            device.rx_headers_phys + (uint64_t)slot * HEADER_STRIDE;
        device.rx.desc[head].len = (uint32_t)device.header_bytes;
        device.rx.desc[head].flags = DESC_WRITE | DESC_NEXT;
        device.rx.desc[head].next = (uint16_t)(head + 1);

        device.rx.desc[head + 1].addr =
            device.rx_data_phys + (uint64_t)slot * BUFFER_BYTES;
        device.rx.desc[head + 1].len = BUFFER_BYTES;
        device.rx.desc[head + 1].flags = DESC_WRITE;
        device.rx.desc[head + 1].next = 0;

        queue_offer(&device.rx, head);
    }
}

static bool virtio_net_probe(const struct pci_device *pci)
{
    if (pci->vendor_id != 0x1AF4)
        return false;

    if (pci->device_id == 0x1041)        /* neue Kennung: 0x1040 + 1 */
        return true;

    /* Die alte Kennung ist fuer alle virtio-Geraete gleich; welche Art
     * es ist, steht in der Untersystemkennung. */
    if (pci->device_id == 0x1000)
        return (pci_read32(pci, 0x2C) >> 16) == 1;

    return false;
}

static bool virtio_net_attach(const struct pci_device *pci, struct nic *nic)
{
    memset(&device, 0, sizeof(device));
    device.pci = pci;

    device.modern = map_modern(pci);

    if (!device.modern) {
        for (int i = 0; i < 6; i++) {
            if (pci->bar_is_io[i] && pci->bar[i]) {
                device.io = (uint16_t)pci->bar[i];
                break;
            }
        }
        if (!device.io)
            return false;
    }

    pci_enable_bus_master(pci);
    pci_set_intx(pci, false);       /* wir fragen die Ringe selbst ab */

    /* Der vorgeschriebene Ablauf: zuruecksetzen, melden, Merkmale
     * aushandeln, Ringe anlegen, fertigmelden. */
    status_reset();
    status_add(ST_ACKNOWLEDGE);
    status_add(ST_DRIVER);

    uint64_t offered = features_offered();
    uint64_t wanted = offered &
                      (F_NET_MAC | F_NET_STATUS | F_ANY_LAYOUT | F_VERSION_1);

    if (device.modern && !(offered & F_VERSION_1)) {
        status_add(ST_FAILED);
        return false;
    }
    if (!device.modern)
        wanted &= 0xFFFFFFFFull;    /* die alte Bauform kennt nur 32 Bit */

    features_accept(wanted);
    device.features = wanted;

    if (device.modern) {
        status_add(ST_FEATURES_OK);
        if (!(status_get() & ST_FEATURES_OK)) {
            status_add(ST_FAILED);
            return false;
        }
    }

    /* Seit virtio 1.0 traegt jeder Rahmen zusaetzlich die Zahl der
     * benutzten Puffer im Kopf - zwei Byte mehr. */
    device.header_bytes = (wanted & F_VERSION_1) ? 12 : 10;

    if (!buffers_setup()) {
        status_add(ST_FAILED);
        return false;
    }

    if (!queue_setup(&device.rx, 0, RX_SLOTS * 2) ||
        !queue_setup(&device.tx, 1, TX_SLOTS * 2)) {
        status_add(ST_FAILED);
        return false;
    }

    /* Die Karte soll uns nicht bei jedem gesendeten Rahmen wecken. */
    device.tx.avail->flags = 1;        /* VRING_AVAIL_F_NO_INTERRUPT */
    device.rx.avail->flags = 1;

    rx_prime();

    /* Adresse: entweder aus dem Geraetebereich oder ausgedacht. */
    if (wanted & F_NET_MAC) {
        for (int i = 0; i < 6; i++)
            g_netif.mac.b[i] = config_byte((size_t)i);
    } else {
        static const uint8_t fallback[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

        memcpy(g_netif.mac.b, fallback, 6);
    }

    status_add(ST_DRIVER_OK);
    queue_notify(&device.rx, 0);

    strlcpy(nic->model,
            device.modern ? "virtio-net (1.0)" : "virtio-net (alte Bauform)",
            sizeof(nic->model));
    nic->ops = &virtio_ops;
    nic->state = &device;
    nic->speed_mbit = 0;               /* die Karte nennt keine Zahl */

    char mac[24];

    mac_format(&g_netif.mac, mac, sizeof(mac));
    kprintf("Netzwerk    : %s, %s\n", nic->model, mac);
    return true;
}

const struct nic_driver virtio_net_driver = {
    .family = "virtio-net",
    .probe  = virtio_net_probe,
    .attach = virtio_net_attach,
};
