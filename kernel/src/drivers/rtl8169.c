/* rtl8169.c - Realtek RTL8169, RTL8168 und RTL8111.
 *
 * Der verbreitetste Netzwerkchip ueberhaupt: In den meisten Mainboards
 * und in vielen Notebooks sitzt einer davon. Anders als der aeltere
 * RTL8139 arbeitet er mit Deskriptorringen, aehnlich den Intel-Karten -
 * nur liegen die Felder anders und der letzte Deskriptor traegt eine
 * Endmarke statt eines Rueckverweises.
 *
 * Ein Hinweis in eigener Sache: QEMU bildet diesen Chip nicht nach.
 * Der Treiber liess sich hier deshalb nicht gegen echte Silizium-
 * Antworten pruefen, sondern nur gegen das Handbuch. Auf einem Rechner
 * mit diesem Chip ist er der erste Kandidat fuer eine Fehlersuche.
 */

#include "net.h"
#include "nic.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"

/* --- Register --- */
#define REG_MAC0        0x00
#define REG_MAR0        0x08
#define REG_TNPDS       0x20      /* Adresse des Senderings   */
#define REG_CMD         0x37
#define REG_TPPOLL      0x38      /* Sendevorgang anstossen   */
#define REG_IMR         0x3C
#define REG_ISR         0x3E
#define REG_TCR         0x40
#define REG_RCR         0x44
#define REG_9346CR      0x50      /* Registersperre           */
#define REG_CONFIG1     0x52
#define REG_PHYSTATUS   0x6C
#define REG_RMS         0xDA      /* groesster Empfangsrahmen */
#define REG_RDSAR       0xE4      /* Adresse des Empfangsrings */
#define REG_ETTHR       0xEC      /* Schwelle beim Senden      */

#define CMD_RESET       (1u << 4)
#define CMD_RX_ENABLE   (1u << 3)
#define CMD_TX_ENABLE   (1u << 2)

#define TPPOLL_NPQ      (1u << 6)

#define CR9346_UNLOCK   0xC0
#define CR9346_LOCK     0x00

#define RCR_AAP         (1u << 0)
#define RCR_APM         (1u << 1)
#define RCR_AM          (1u << 2)
#define RCR_AB          (1u << 3)

/* --- Deskriptorbits --- */
#define DESC_OWN        (1u << 31)   /* der Chip ist zustaendig */
#define DESC_EOR        (1u << 30)   /* letzter Eintrag im Ring */
#define DESC_FS         (1u << 29)   /* erster Teil des Rahmens */
#define DESC_LS         (1u << 28)   /* letzter Teil            */
#define DESC_RX_RES     (1u << 21)   /* Empfangsfehler          */

#define RING_SIZE       32
#define BUFFER_BYTES    2048

/* Ein Deskriptor ist sechzehn Byte gross und muss auf einer Grenze von
 * 256 Byte beginnen. */
struct rtl_desc {
    uint32_t command;       /* Besitz, Marken und Laenge */
    uint32_t vlan;
    uint64_t address;
} PACKED;

struct rtl8169 {
    volatile uint8_t *mmio;
    uint16_t          io;
    bool              use_mmio;

    struct rtl_desc  *rx_ring;
    struct rtl_desc  *tx_ring;
    uint64_t          rx_ring_phys, tx_ring_phys;
    uint8_t          *buffers;
    uint64_t          buffers_phys;

    uint32_t          rx_cursor, tx_cursor;
};

static struct rtl8169 device;

/* Der Chip haengt je nach Ausfuehrung an Ein-/Ausgabeadressen oder im
 * Speicher. Beide Wege fuehren zu denselben Registern. */
static uint8_t r8(uint16_t reg)
{
    return device.use_mmio ? *(volatile uint8_t *)(device.mmio + reg)
                           : inb((uint16_t)(device.io + reg));
}

static uint16_t r16(uint16_t reg)
{
    return device.use_mmio ? *(volatile uint16_t *)(device.mmio + reg)
                           : inw((uint16_t)(device.io + reg));
}

static uint32_t r32(uint16_t reg)
{
    return device.use_mmio ? *(volatile uint32_t *)(device.mmio + reg)
                           : inl((uint16_t)(device.io + reg));
}

static void w8(uint16_t reg, uint8_t value)
{
    if (device.use_mmio)
        *(volatile uint8_t *)(device.mmio + reg) = value;
    else
        outb((uint16_t)(device.io + reg), value);
}

static void w16(uint16_t reg, uint16_t value)
{
    if (device.use_mmio)
        *(volatile uint16_t *)(device.mmio + reg) = value;
    else
        outw((uint16_t)(device.io + reg), value);
}

static void w32(uint16_t reg, uint32_t value)
{
    if (device.use_mmio)
        *(volatile uint32_t *)(device.mmio + reg) = value;
    else
        outl((uint16_t)(device.io + reg), value);
}

static void w64(uint16_t reg, uint64_t value)
{
    w32(reg, (uint32_t)value);
    w32((uint16_t)(reg + 4), (uint32_t)(value >> 32));
}

/* ------------------------------------------------------------------ */
/* Betrieb                                                             */
/* ------------------------------------------------------------------ */

static uint8_t *rx_buffer(uint32_t index)
{
    return device.buffers + (size_t)index * BUFFER_BYTES;
}

static uint8_t *tx_buffer(uint32_t index)
{
    return device.buffers + (size_t)(RING_SIZE + index) * BUFFER_BYTES;
}

static bool rtl8169_send(struct nic *nic, const void *frame, uint16_t length)
{
    UNUSED(nic);

    if (length == 0 || length > BUFFER_BYTES)
        return false;

    struct rtl_desc *desc = &device.tx_ring[device.tx_cursor];

    /* Solange das Besitzbit steht, gehoert der Eintrag dem Chip. */
    for (int guard = 0; guard < 1000000; guard++) {
        if (!(desc->command & DESC_OWN))
            break;
        io_wait();
    }
    if (desc->command & DESC_OWN)
        return false;

    uint16_t padded = length;

    memcpy(tx_buffer(device.tx_cursor), frame, length);
    if (padded < 60) {
        memset(tx_buffer(device.tx_cursor) + length, 0,
               (size_t)(60 - length));
        padded = 60;
    }

    uint32_t command = DESC_OWN | DESC_FS | DESC_LS | padded;

    if (device.tx_cursor == RING_SIZE - 1)
        command |= DESC_EOR;

    desc->vlan = 0;
    __asm__ volatile("sfence" ::: "memory");
    desc->command = command;
    __asm__ volatile("sfence" ::: "memory");

    device.tx_cursor = (device.tx_cursor + 1) % RING_SIZE;

    /* Dem Chip sagen, dass etwas zu holen ist. */
    w8(REG_TPPOLL, TPPOLL_NPQ);

    g_netif.tx_packets++;
    g_netif.tx_bytes += length;
    return true;
}

static uint16_t rtl8169_receive(struct nic *nic, void *buffer,
                                uint16_t capacity)
{
    UNUSED(nic);

    struct rtl_desc *desc = &device.rx_ring[device.rx_cursor];

    if (desc->command & DESC_OWN)
        return 0;                       /* noch nicht gefuellt */

    uint16_t copied = 0;

    /* Die untersten vierzehn Bit sind die Laenge, einschliesslich der
     * vier Pruefbytes am Ende. */
    uint16_t length = (uint16_t)(desc->command & 0x3FFF);

    if (!(desc->command & DESC_RX_RES) && length > 4) {
        uint16_t payload = (uint16_t)(length - 4);

        if (payload <= capacity) {
            memcpy(buffer, rx_buffer(device.rx_cursor), payload);
            copied = payload;
            g_netif.rx_packets++;
            g_netif.rx_bytes += payload;
        } else {
            g_netif.rx_dropped++;
        }
    } else {
        g_netif.rx_dropped++;
    }

    /* Den Eintrag wieder dem Chip ueberlassen. */
    uint32_t command = DESC_OWN | BUFFER_BYTES;

    if (device.rx_cursor == RING_SIZE - 1)
        command |= DESC_EOR;
    desc->vlan = 0;
    __asm__ volatile("sfence" ::: "memory");
    desc->command = command;

    device.rx_cursor = (device.rx_cursor + 1) % RING_SIZE;

    /* Quittieren, was der Chip gemeldet hat - wir fragen die Ringe zwar
     * selbst ab, aber ein stehengebliebenes Bit haelt manche Ausfuehrung
     * des Chips davon ab, weiterzumachen. */
    uint16_t pending = r16(REG_ISR);

    if (pending)
        w16(REG_ISR, pending);
    return copied;
}

static bool rtl8169_link_up(struct nic *nic)
{
    UNUSED(nic);
    return (r8(REG_PHYSTATUS) & (1u << 1)) != 0;
}

static const struct nic_ops rtl8169_ops = {
    .send    = rtl8169_send,
    .receive = rtl8169_receive,
    .link_up = rtl8169_link_up,
};

/* ------------------------------------------------------------------ */
/* Einrichten                                                          */
/* ------------------------------------------------------------------ */

static bool rtl8169_probe(const struct pci_device *pci)
{
    if (pci->vendor_id != 0x10EC)
        return false;

    switch (pci->device_id) {
    case 0x8129:        /* RTL8129              */
    case 0x8161:        /* RTL8168 in neueren   */
    case 0x8167:        /* RTL8110SC            */
    case 0x8168:        /* RTL8168, RTL8111     */
    case 0x8169:        /* RTL8169              */
    case 0x8136:        /* RTL8101E, RTL8102E   */
        return true;
    default:
        return false;
    }
}

static bool rtl8169_attach(const struct pci_device *pci, struct nic *nic)
{
    memset(&device, 0, sizeof(device));

    /* Bevorzugt der Speicherbereich - die neueren Ausfuehrungen bieten
     * gar keine Ein-/Ausgabeadressen mehr an. */
    for (int i = 0; i < 6; i++) {
        if (pci->bar[i] && !pci->bar_is_io[i]) {
            device.mmio = phys_to_virt(pci->bar[i]);
            device.use_mmio = true;
            break;
        }
    }
    if (!device.use_mmio) {
        for (int i = 0; i < 6; i++) {
            if (pci->bar[i] && pci->bar_is_io[i]) {
                device.io = (uint16_t)pci->bar[i];
                break;
            }
        }
        if (!device.io)
            return false;
    }

    pci_enable_bus_master(pci);
    pci_set_intx(pci, false);   /* wir fragen die Ringe selbst ab */

    /* Zuruecksetzen und warten, bis das Bit von selbst faellt. */
    w8(REG_CMD, CMD_RESET);

    bool ready = false;

    for (int guard = 0; guard < 100000; guard++) {
        if (!(r8(REG_CMD) & CMD_RESET)) {
            ready = true;
            break;
        }
        io_wait();
    }
    if (!ready)
        return false;

    /* MAC-Adresse aus den ersten sechs Registern. */
    uint32_t low = r32(REG_MAC0);
    uint32_t high = r32(REG_MAC0 + 4);

    if (low == 0 && (high & 0xFFFF) == 0)
        return false;

    g_netif.mac.b[0] = (uint8_t)low;
    g_netif.mac.b[1] = (uint8_t)(low >> 8);
    g_netif.mac.b[2] = (uint8_t)(low >> 16);
    g_netif.mac.b[3] = (uint8_t)(low >> 24);
    g_netif.mac.b[4] = (uint8_t)high;
    g_netif.mac.b[5] = (uint8_t)(high >> 8);

    /* Ringe und Puffer in einem Stueck. Die Deskriptoren muessen an
     * einer 256-Byte-Grenze beginnen; eine Seite genuegt dafuer. */
    size_t desc_bytes = ALIGN_UP(2 * RING_SIZE * sizeof(struct rtl_desc),
                                 PAGE_SIZE);
    size_t buffer_bytes = (size_t)2 * RING_SIZE * BUFFER_BYTES;
    uint64_t phys = pmm_alloc_pages((desc_bytes + buffer_bytes) / PAGE_SIZE);

    if (!phys)
        return false;

    uint8_t *base = phys_to_virt(phys);

    memset(base, 0, desc_bytes + buffer_bytes);

    device.rx_ring = (struct rtl_desc *)base;
    device.rx_ring_phys = phys;
    device.tx_ring = (struct rtl_desc *)(base + RING_SIZE *
                                         sizeof(struct rtl_desc));
    device.tx_ring_phys = phys + RING_SIZE * sizeof(struct rtl_desc);
    device.buffers = base + desc_bytes;
    device.buffers_phys = phys + desc_bytes;

    for (uint32_t i = 0; i < RING_SIZE; i++) {
        device.rx_ring[i].address = device.buffers_phys +
                                    (uint64_t)i * BUFFER_BYTES;
        device.rx_ring[i].command = DESC_OWN | BUFFER_BYTES |
                                    (i == RING_SIZE - 1 ? DESC_EOR : 0);

        device.tx_ring[i].address = device.buffers_phys +
                                    (uint64_t)(RING_SIZE + i) * BUFFER_BYTES;
        device.tx_ring[i].command = (i == RING_SIZE - 1) ? DESC_EOR : 0;
    }

    /* Die Register hinter der Sperre freigeben. */
    w8(REG_9346CR, CR9346_UNLOCK);

    w32(REG_TCR, (3u << 24) | (7u << 8));
    w8(REG_ETTHR, 0x3F);
    w16(REG_RMS, BUFFER_BYTES);

    w64(REG_TNPDS, device.tx_ring_phys);
    w64(REG_RDSAR, device.rx_ring_phys);

    /* Erst senden und empfangen einschalten, dann die Annahmeregeln -
     * so verlangt es das Handbuch. */
    w8(REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);
    w32(REG_RCR, RCR_APM | RCR_AM | RCR_AB | (7u << 13) | (7u << 8));

    w16(REG_IMR, 0);                     /* keine Unterbrechungen */
    w16(REG_ISR, 0xFFFF);

    w8(REG_9346CR, CR9346_LOCK);

    device.rx_cursor = 0;
    device.tx_cursor = 0;

    const char *name = "Realtek RTL8169";

    if (pci->device_id == 0x8168 || pci->device_id == 0x8161)
        name = "Realtek RTL8168/8111";
    else if (pci->device_id == 0x8136)
        name = "Realtek RTL8101/8102";

    strlcpy(nic->model, name, sizeof(nic->model));
    nic->ops = &rtl8169_ops;
    nic->state = &device;

    uint8_t status = r8(REG_PHYSTATUS);

    if (status & (1u << 4))
        nic->speed_mbit = 1000;
    else if (status & (1u << 3))
        nic->speed_mbit = 100;
    else
        nic->speed_mbit = 10;

    char mac[24];

    mac_format(&g_netif.mac, mac, sizeof(mac));
    kprintf("Netzwerk    : %s, %s\n", name, mac);
    return true;
}

const struct nic_driver rtl8169_driver = {
    .family = "Realtek RTL8169",
    .probe  = rtl8169_probe,
    .attach = rtl8169_attach,
};
