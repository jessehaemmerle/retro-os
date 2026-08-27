/* rtl8139.c - Realtek RTL8139, der Klassiker unter den Netzwerkkarten.
 *
 * Der Chip arbeitet anders als die Karten mit Deskriptorringen: Zum
 * Empfangen bekommt er einen einzigen grossen Ringpuffer, in den er
 * einen Rahmen nach dem anderen schreibt - jeweils mit einem kleinen
 * Kopf davor, der Laenge und Zustand nennt. Der Kern merkt sich, wie
 * weit er gelesen hat, und sagt es dem Chip ueber ein Register.
 *
 * Zum Senden gibt es vier feste Plaetze, die reihum benutzt werden.
 * Mehr braucht es nicht: Ist einer noch belegt, wartet der Aufrufer.
 */

#include "net.h"
#include "nic.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"

/* --- Register, alle ueber Ein-/Ausgabeadressen erreichbar --- */
#define REG_MAC0        0x00
#define REG_MAR0        0x08      /* Multicast-Filter */
#define REG_TSD0        0x10      /* Zustand der vier Sendeplaetze */
#define REG_TSAD0       0x20      /* Adressen der vier Sendeplaetze */
#define REG_RBSTART     0x30      /* Anfang des Empfangsrings */
#define REG_CMD         0x37
#define REG_CAPR        0x38      /* bis hierhin hat der Kern gelesen */
#define REG_CBR         0x3A
#define REG_IMR         0x3C
#define REG_ISR         0x3E
#define REG_TCR         0x40
#define REG_RCR         0x44
#define REG_CONFIG1     0x52
#define REG_MSR         0x58      /* Zustand der Leitung */

#define CMD_RESET       (1u << 4)
#define CMD_RX_ENABLE   (1u << 3)
#define CMD_TX_ENABLE   (1u << 2)
#define CMD_BUFE        (1u << 0)  /* Empfangsring ist leer */

#define RCR_AAP         (1u << 0)  /* alles annehmen                 */
#define RCR_APM         (1u << 1)  /* an die eigene Adresse          */
#define RCR_AM          (1u << 2)  /* Mehrfachadressen               */
#define RCR_AB          (1u << 3)  /* Rundsendungen                  */
#define RCR_WRAP        (1u << 7)  /* ueber das Ende hinausschreiben */

/* Wie gross der Empfangsring ist, muss dem Chip gesagt werden - sonst
 * legt er den naechsten Rahmen nach acht Kilobyte wieder an den Anfang,
 * waehrend der Kern noch weiter hinten liest. */
#define RCR_RBLEN_32K   (2u << 11)

#define TSD_OWN         (1u << 13) /* der Chip ist fertig            */
#define TSD_TOK         (1u << 15) /* erfolgreich gesendet           */

#define RX_STATUS_OK    (1u << 0)

/* Der Ring fasst 32 KiB. Die vier zusaetzlichen Kilobyte sind der
 * Ueberlauf, den das WRAP-Bit erlaubt: Der Chip darf ueber das Ende
 * hinausschreiben, statt einen Rahmen zu zerteilen. */
#define RX_RING_BYTES   (32 * 1024)
#define RX_RING_PAD     2048
#define TX_SLOTS        4
#define TX_SLOT_BYTES   2048

struct rtl8139 {
    uint16_t  io;
    uint8_t  *rx_ring;
    uint64_t  rx_ring_phys;
    uint8_t  *tx_slots;
    uint64_t  tx_slots_phys;
    uint32_t  rx_offset;
    uint32_t  tx_slot;
};

static struct rtl8139 device;

static uint8_t  r8(uint16_t reg)  { return inb(device.io + reg); }
static uint32_t r32(uint16_t reg) { return inl(device.io + reg); }

static void w8(uint16_t reg, uint8_t value)   { outb(device.io + reg, value); }
static void w16(uint16_t reg, uint16_t value) { outw(device.io + reg, value); }
static void w32(uint16_t reg, uint32_t value) { outl(device.io + reg, value); }

/* ------------------------------------------------------------------ */
/* Betrieb                                                             */
/* ------------------------------------------------------------------ */

static bool rtl8139_send(struct nic *nic, const void *frame, uint16_t length)
{
    UNUSED(nic);

    if (length == 0 || length > TX_SLOT_BYTES)
        return false;

    uint16_t slot = (uint16_t)(device.tx_slot * 4);

    /* Warten, bis der Chip den Platz wieder freigegeben hat. */
    for (int guard = 0; guard < 1000000; guard++) {
        if (r32((uint16_t)(REG_TSD0 + slot)) & TSD_OWN)
            break;
        io_wait();
    }
    if (!(r32((uint16_t)(REG_TSD0 + slot)) & TSD_OWN))
        return false;

    memcpy(device.tx_slots + (size_t)device.tx_slot * TX_SLOT_BYTES,
           frame, length);

    /* Kurze Rahmen muessen auf 60 Byte aufgefuellt werden - die vier
     * Pruefbytes legt der Chip selbst dazu. */
    uint16_t padded = length;

    if (padded < 60) {
        memset(device.tx_slots + (size_t)device.tx_slot * TX_SLOT_BYTES +
               length, 0, (size_t)(60 - length));
        padded = 60;
    }

    w32((uint16_t)(REG_TSAD0 + slot),
        (uint32_t)(device.tx_slots_phys +
                   (uint64_t)device.tx_slot * TX_SLOT_BYTES));
    /* Das Schreiben der Laenge startet die Uebertragung. */
    w32((uint16_t)(REG_TSD0 + slot), padded);

    device.tx_slot = (device.tx_slot + 1) % TX_SLOTS;

    g_netif.tx_packets++;
    g_netif.tx_bytes += length;
    return true;
}

static uint16_t rtl8139_receive(struct nic *nic, void *buffer,
                                uint16_t capacity)
{
    UNUSED(nic);

    if (r8(REG_CMD) & CMD_BUFE)
        return 0;                       /* nichts da */

    const uint8_t *at = device.rx_ring + device.rx_offset;
    uint16_t status = (uint16_t)(at[0] | (at[1] << 8));
    uint16_t length = (uint16_t)(at[2] | (at[3] << 8));

    /* Die Laenge schliesst die vier Pruefbytes am Ende mit ein. */
    if (length < 4 || length > RX_RING_BYTES) {
        /* Der Ring ist aus dem Tritt - von vorne anfangen. */
        w8(REG_CMD, CMD_TX_ENABLE);
        device.rx_offset = 0;
        w16(REG_CAPR, (uint16_t)0xFFF0);
        w32(REG_RCR, RCR_APM | RCR_AM | RCR_AB | RCR_WRAP | RCR_RBLEN_32K |
                     (7u << 13) | (7u << 8));
        w8(REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);
        g_netif.rx_dropped++;
        return 0;
    }

    uint16_t payload = (uint16_t)(length - 4);
    uint16_t copied = 0;

    if ((status & RX_STATUS_OK) && payload > 0 && payload <= capacity) {
        memcpy(buffer, at + 4, payload);
        copied = payload;
        g_netif.rx_packets++;
        g_netif.rx_bytes += payload;
    } else {
        g_netif.rx_dropped++;
    }

    /* Weiterruecken: Kopf plus Rahmen, auf vier Byte aufgerundet. */
    device.rx_offset = (device.rx_offset + length + 4 + 3) & ~3u;
    device.rx_offset %= RX_RING_BYTES;

    /* Der Chip erwartet den Wert um sechzehn vermindert. */
    w16(REG_CAPR, (uint16_t)(device.rx_offset - 16));
    w16(REG_ISR, 0x0001);               /* Empfang quittieren */
    return copied;
}

static bool rtl8139_link_up(struct nic *nic)
{
    UNUSED(nic);
    /* Bit 2 des Zustandsregisters meldet "kein Kabel". */
    return (r8(REG_MSR) & (1u << 2)) == 0;
}

static const struct nic_ops rtl8139_ops = {
    .send    = rtl8139_send,
    .receive = rtl8139_receive,
    .link_up = rtl8139_link_up,
};

/* ------------------------------------------------------------------ */
/* Einrichten                                                          */
/* ------------------------------------------------------------------ */

static bool rtl8139_probe(const struct pci_device *pci)
{
    if (pci->vendor_id != 0x10EC)
        return false;
    return pci->device_id == 0x8139 || pci->device_id == 0x8138;
}

static bool rtl8139_attach(const struct pci_device *pci, struct nic *nic)
{
    memset(&device, 0, sizeof(device));

    /* Der Chip ist ueber Ein-/Ausgabeadressen erreichbar; welcher der
     * Bereiche das ist, sagt die Kennung im Adressregister. */
    for (int i = 0; i < 6; i++) {
        if (pci->bar_is_io[i] && pci->bar[i]) {
            device.io = (uint16_t)pci->bar[i];
            break;
        }
    }
    if (!device.io)
        return false;

    pci_enable_bus_master(pci);

    /* Aus dem Schlafzustand holen und zuruecksetzen. */
    w8(REG_CONFIG1, 0x00);
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

    /* Speicher fuer Ring und Sendeplaetze - zusammenhaengend, weil der
     * Chip nur eine physische Adresse bekommt. */
    size_t bytes = RX_RING_BYTES + RX_RING_PAD +
                   (size_t)TX_SLOTS * TX_SLOT_BYTES;
    uint64_t phys = pmm_alloc_pages(ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE);

    if (!phys)
        return false;

    /* Der Chip kennt nur 32-Bit-Adressen. */
    if (phys + bytes > 0xFFFFFFFFull) {
        pmm_free_pages(phys, ALIGN_UP(bytes, PAGE_SIZE) / PAGE_SIZE);
        kprintf("Netzwerk    : RTL8139 braucht Speicher unter 4 GiB\n");
        return false;
    }

    device.rx_ring = phys_to_virt(phys);
    device.rx_ring_phys = phys;
    device.tx_slots = device.rx_ring + RX_RING_BYTES + RX_RING_PAD;
    device.tx_slots_phys = phys + RX_RING_BYTES + RX_RING_PAD;
    memset(device.rx_ring, 0, bytes);

    /* MAC-Adresse steht in den ersten sechs Registern. */
    uint32_t low = r32(REG_MAC0);
    uint32_t high = r32(REG_MAC0 + 4);

    g_netif.mac.b[0] = (uint8_t)low;
    g_netif.mac.b[1] = (uint8_t)(low >> 8);
    g_netif.mac.b[2] = (uint8_t)(low >> 16);
    g_netif.mac.b[3] = (uint8_t)(low >> 24);
    g_netif.mac.b[4] = (uint8_t)high;
    g_netif.mac.b[5] = (uint8_t)(high >> 8);

    if (low == 0 && (high & 0xFFFF) == 0)
        return false;

    w32(REG_RBSTART, (uint32_t)device.rx_ring_phys);

    /* Annehmen: eigene Adresse, Rundsendungen, Mehrfachadressen; der
     * Chip darf ueber das Ringende hinausschreiben. Die beiden Felder
     * darueber sind die Schwellen, ab denen er meldet bzw. holt. */
    w32(REG_RCR, RCR_APM | RCR_AM | RCR_AB | RCR_WRAP | RCR_RBLEN_32K |
                 (7u << 13) | (7u << 8));
    w32(REG_TCR, (3u << 24) | (7u << 8));

    w16(REG_IMR, 0);                     /* keine Unterbrechungen */
    w16(REG_ISR, 0xFFFF);
    w8(REG_CMD, CMD_RX_ENABLE | CMD_TX_ENABLE);

    device.rx_offset = 0;
    device.tx_slot = 0;

    strlcpy(nic->model, "Realtek RTL8139", sizeof(nic->model));
    nic->ops = &rtl8139_ops;
    nic->state = &device;
    nic->speed_mbit = (r8(REG_MSR) & (1u << 3)) ? 10 : 100;

    char mac[24];

    mac_format(&g_netif.mac, mac, sizeof(mac));
    kprintf("Netzwerk    : Realtek RTL8139, %s\n", mac);
    return true;
}

const struct nic_driver rtl8139_driver = {
    .family = "Realtek RTL8139",
    .probe  = rtl8139_probe,
    .attach = rtl8139_attach,
};
