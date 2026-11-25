/* pcnet.c - AMD PCnet (79C970A bis 79C973).
 *
 * Standardkarte aelterer Gastsysteme in VirtualBox, auch in QEMU und
 * VMware nachgebildet. Der Treiber schaltet sie in den 32-Bit-Stil,
 * baut einen Initialisierungsblock mit zwei Ringen und arbeitet ohne
 * Unterbrechungen: Die Karte schreibt Rahmen in Puffer, der Kern
 * schaut nach.
 */

#include "net.h"
#include "nic.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"
#include "pci.h"
#include "pcnethw.h"

#define RING_SIZE   (1u << PCNET_RING_LOG2)
#define LIMIT_4G    0x100000000ull

struct pcnet {
    uint16_t           io;
    struct pcnet_desc *rx, *tx;
    uint8_t           *rx_buf, *tx_buf;
    uint64_t           phys;
    size_t             pages;
    uint32_t           rx_at, tx_at;
};

static struct pcnet device;

static uint32_t csr_read(uint32_t index)
{
    outl(device.io + PCNET_RAP, index);
    return inl(device.io + PCNET_RDP);
}

static void csr_write(uint32_t index, uint32_t value)
{
    outl(device.io + PCNET_RAP, index);
    outl(device.io + PCNET_RDP, value);
}

static uint32_t bcr_read(uint32_t index)
{
    outl(device.io + PCNET_RAP, index);
    return inl(device.io + PCNET_BDP);
}

static void bcr_write(uint32_t index, uint32_t value)
{
    outl(device.io + PCNET_RAP, index);
    outl(device.io + PCNET_BDP, value);
}

static bool pcnet_send(struct nic *nic, const void *frame, uint16_t length)
{
    UNUSED(nic);

    if (length == 0 || length > PCNET_BUF_BYTES - 4)
        return false;

    volatile struct pcnet_desc *d = &device.tx[device.tx_at];

    /* Gehoert der Platz noch der Karte, ist der Ring voll. */
    for (int guard = 0; guard < 1000000 && (d->status & PCNET_DESC_OWN); guard++)
        io_wait();
    if (d->status & PCNET_DESC_OWN)
        return false;

    uint8_t *buf = device.tx_buf + (size_t)device.tx_at * PCNET_BUF_BYTES;
    uint16_t padded = length;

    memcpy(buf, frame, length);

    /* Unter 60 Byte fuellt die Karte nicht selbst auf, solange das
     * nicht eingeschaltet ist - sicher ist sicher. */
    if (padded < 60) {
        memset(buf + length, 0, (size_t)(60 - length));
        padded = 60;
    }

    d->base = (uint32_t)(device.phys + (uint64_t)(buf - (uint8_t *)device.rx));
    d->length = pcnet_buffer_length(padded);
    d->message = 0;
    __asm__ volatile("" ::: "memory");
    d->status = (uint16_t)(PCNET_DESC_OWN | PCNET_DESC_STP | PCNET_DESC_ENP);
    __asm__ volatile("" ::: "memory");

    device.tx_at = (device.tx_at + 1) % RING_SIZE;
    csr_write(0, PCNET_CSR0_TDMD);         /* nicht auf die Abfrage warten */

    g_netif.tx_packets++;
    g_netif.tx_bytes += length;
    return true;
}

static uint16_t pcnet_receive(struct nic *nic, void *buffer, uint16_t capacity)
{
    UNUSED(nic);

    volatile struct pcnet_desc *d = &device.rx[device.rx_at];

    if (!pcnet_rx_ready(d->status))
        return 0;                           /* nichts da */

    uint16_t copied = 0;
    uint32_t payload = pcnet_rx_payload(d->message);

    if (pcnet_rx_usable(d->status) && payload > 0 && payload <= capacity) {
        memcpy(buffer,
               device.rx_buf + (size_t)device.rx_at * PCNET_BUF_BYTES, payload);
        copied = (uint16_t)payload;
        g_netif.rx_packets++;
        g_netif.rx_bytes += payload;
    } else {
        g_netif.rx_dropped++;
    }

    /* Den Platz an die Karte zurueckgeben. */
    d->length = pcnet_buffer_length(PCNET_BUF_BYTES);
    d->message = 0;
    __asm__ volatile("" ::: "memory");
    d->status = PCNET_DESC_OWN;

    device.rx_at = (device.rx_at + 1) % RING_SIZE;
    return copied;
}

static const struct nic_ops pcnet_ops = {
    .send    = pcnet_send,
    .receive = pcnet_receive,
    .link_up = NULL,
};

static bool pcnet_probe(const struct pci_device *pci)
{
    /* 79C970A (PCnet-PCI II) und 79C973 (FAST III) melden dieselbe
     * Kennung. */
    return pci->vendor_id == 0x1022 && pci->device_id == 0x2000;
}

static bool pcnet_attach(const struct pci_device *pci, struct nic *nic)
{
    memset(&device, 0, sizeof(device));

    for (int i = 0; i < 6; i++) {
        if (pci->bar_is_io[i] && pci->bar[i]) {
            device.io = (uint16_t)pci->bar[i];
            break;
        }
    }
    if (!device.io)
        return false;

    pci_enable_bus_master(pci);

    /* Die MAC-Adresse steht in den ersten sechs Byte - und nur VOR dem
     * Umschalten auf 32-Bit-Zugriff: Danach antwortet die Karte auf
     * Bytelesen dort mit lauter Einsen, und RetroOS meldete sich im Netz
     * mit der Absenderadresse ff:ff:ff:ff:ff:ff. */
    uint8_t mac[6];

    for (int i = 0; i < 6; i++)
        mac[i] = inb(device.io + PCNET_APROM + i);
    if ((mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0 ||
        (mac[0] & mac[1] & mac[2] & mac[3] & mac[4] & mac[5]) == 0xFF)
        return false;
    for (int i = 0; i < 6; i++)
        g_netif.mac.b[i] = mac[i];

    /* Zuruecksetzen (im 16-Bit-Zustand liegt das Register bei 0x14,
     * im 32-Bit-Zustand bei 0x18), dann in den 32-Bit-Zugriff
     * schalten: ein Doppelwort-Schreiben auf das Datenregister. */
    (void)inw(device.io + PCNET_WIO_RESET);
    (void)inl(device.io + PCNET_RESET);
    for (int i = 0; i < 1000; i++)
        io_wait();
    outl(device.io + PCNET_RDP, 0);

    /* Antwortet die Karte? Das Adressregister muss zurueckgeben, was
     * hineinkam. */
    outl(device.io + PCNET_RAP, 88);
    if ((inl(device.io + PCNET_RAP) & 0xFF) != 88)
        return false;

    csr_write(0, PCNET_CSR0_STOP);

    /* 32-Bit-Deskriptoren (Software-Stil 2); Medien selbst waehlen. */
    bcr_write(PCNET_BCR_SWS, 2);
    bcr_write(PCNET_BCR_MC, bcr_read(PCNET_BCR_MC) | 0x0002);

    /* CSR4: Auffuellen kurzer Rahmen durch die Karte (Bit 11) und
     * keine Meldung ueber Sendeanfang (Bit 12 aus). */
    csr_write(4, csr_read(4) | 0x0800);

    /* Speicher: zwei Ringe, Puffer, Initialisierungsblock - alles in
     * einem Stueck unter 4 GiB, die Karte kennt nur 32-Bit-Adressen. */
    size_t ring_bytes = RING_SIZE * sizeof(struct pcnet_desc);
    size_t buf_bytes = (size_t)2 * RING_SIZE * PCNET_BUF_BYTES;
    size_t total = 2 * ring_bytes + PCNET_INIT_BYTES + buf_bytes;

    device.pages = ALIGN_UP(total, PAGE_SIZE) / PAGE_SIZE;
    device.phys = pmm_alloc_pages_below(device.pages, LIMIT_4G);
    if (!device.phys) {
        kprintf("Netzwerk    : PCnet braucht Speicher unter 4 GiB\n");
        return false;
    }

    uint8_t *base = phys_to_virt(device.phys);

    memset(base, 0, device.pages * PAGE_SIZE);

    device.rx = (struct pcnet_desc *)base;
    device.tx = (struct pcnet_desc *)(base + ring_bytes);
    uint8_t *init = base + 2 * ring_bytes;

    device.rx_buf = init + PCNET_INIT_BYTES;
    /* Auf 16 Byte ausrichten (der Block hat 28). */
    device.rx_buf = (uint8_t *)ALIGN_UP((uintptr_t)device.rx_buf, 16);
    device.tx_buf = device.rx_buf + (size_t)RING_SIZE * PCNET_BUF_BYTES;

    /* tx_buf-Adresse wird relativ zu "rx" berechnet: gleicher Block. */
    uint64_t rx_buf_phys = device.phys + (uint64_t)(device.rx_buf - base);

    for (uint32_t i = 0; i < RING_SIZE; i++) {
        device.rx[i].base = (uint32_t)(rx_buf_phys + (uint64_t)i * PCNET_BUF_BYTES);
        device.rx[i].length = pcnet_buffer_length(PCNET_BUF_BYTES);
        device.rx[i].status = PCNET_DESC_OWN;
    }

    uint64_t init_phys = device.phys + 2 * ring_bytes;

    pcnet_build_init(init, mac, PCNET_RING_LOG2, PCNET_RING_LOG2,
                     (uint32_t)device.phys,
                     (uint32_t)(device.phys + ring_bytes));

    csr_write(1, (uint32_t)(init_phys & 0xFFFF));
    csr_write(2, (uint32_t)((init_phys >> 16) & 0xFFFF));
    csr_write(0, PCNET_CSR0_INIT);

    bool done = false;

    for (int i = 0; i < 100000; i++) {
        if (csr_read(0) & PCNET_CSR0_IDON) {
            done = true;
            break;
        }
        io_wait();
    }
    if (!done) {
        pmm_free_pages(device.phys, device.pages);
        return false;
    }

    csr_write(0, PCNET_CSR0_IDON);          /* Meldung loeschen          */
    csr_write(0, PCNET_CSR0_STRT);          /* los - ohne Unterbrechung  */

    strlcpy(nic->model, "AMD PCnet", sizeof(nic->model));
    nic->ops = &pcnet_ops;
    nic->state = &device;
    nic->speed_mbit = 100;

    char text[24];

    mac_format(&g_netif.mac, text, sizeof(text));
    kprintf("Netzwerk    : AMD PCnet, %s\n", text);
    return true;
}

const struct nic_driver pcnet_driver = {
    .family = "AMD PCnet",
    .probe  = pcnet_probe,
    .attach = pcnet_attach,
};
