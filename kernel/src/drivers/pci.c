/* pci.c - PCI-Bus ueber den alten Konfigurationsmechanismus 1.
 *
 * Zwei I/O-Ports genuegen, um den gesamten Bus abzufragen: in 0xCF8 wird
 * die Adresse geschrieben, aus 0xCFC das Datenwort gelesen. Das reicht fuer
 * alles, was RetroOS braucht - Festplattencontroller und Netzwerkkarte.
 */

#include "pci.h"
#include "apic.h"
#include "io.h"
#include "kstring.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static struct pci_device devices[PCI_MAX_DEVICES];
static size_t            device_count;

static uint32_t config_read(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t address = (1u << 31)
                     | ((uint32_t)bus  << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)func << 8)
                     | (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

static void config_write(uint8_t bus, uint8_t slot, uint8_t func,
                         uint8_t offset, uint32_t value)
{
    uint32_t address = (1u << 31)
                     | ((uint32_t)bus  << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)func << 8)
                     | (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

uint32_t pci_read32(const struct pci_device *dev, uint8_t offset)
{
    return config_read(dev->bus, dev->slot, dev->func, offset);
}

uint16_t pci_read16(const struct pci_device *dev, uint8_t offset)
{
    uint32_t v = config_read(dev->bus, dev->slot, dev->func, offset);

    return (uint16_t)((v >> ((offset & 2) * 8)) & 0xFFFF);
}

void pci_write32(const struct pci_device *dev, uint8_t offset, uint32_t value)
{
    config_write(dev->bus, dev->slot, dev->func, offset, value);
}

void pci_write16(const struct pci_device *dev, uint8_t offset, uint16_t value)
{
    uint32_t old = config_read(dev->bus, dev->slot, dev->func, offset);
    uint32_t shift = (offset & 2) * 8;

    old &= ~(0xFFFFu << shift);
    old |= (uint32_t)value << shift;
    config_write(dev->bus, dev->slot, dev->func, offset, old);
}

void pci_enable_bus_master(const struct pci_device *dev)
{
    uint16_t command = pci_read16(dev, 0x04);

    command |= (1 << 0);   /* I/O-Zugriff       */
    command |= (1 << 1);   /* Speicherzugriff   */
    command |= (1 << 2);   /* Busmaster (DMA)   */
    pci_write16((struct pci_device *)dev, 0x04, command);
}

/* Die Groesse eines Bereichs steht nirgends - man ermittelt sie, indem
 * man lauter Einsen schreibt und liest, welche Bits stehen bleiben. */
static uint64_t probe_bar_size(struct pci_device *dev, int index, bool wide)
{
    uint8_t offset = (uint8_t)(0x10 + index * 4);
    uint32_t low = config_read(dev->bus, dev->slot, dev->func, offset);
    uint32_t high = wide ? config_read(dev->bus, dev->slot, dev->func,
                                       (uint8_t)(offset + 4)) : 0;

    config_write(dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFFu);
    if (wide)
        config_write(dev->bus, dev->slot, dev->func, (uint8_t)(offset + 4),
                     0xFFFFFFFFu);

    uint32_t mask_low = config_read(dev->bus, dev->slot, dev->func, offset);
    uint32_t mask_high = wide ? config_read(dev->bus, dev->slot, dev->func,
                                            (uint8_t)(offset + 4)) : 0;

    config_write(dev->bus, dev->slot, dev->func, offset, low);
    if (wide)
        config_write(dev->bus, dev->slot, dev->func, (uint8_t)(offset + 4),
                     high);

    uint64_t mask = ((uint64_t)mask_high << 32) | mask_low;

    if (low & 1)
        mask |= 0x3ull;
    else
        mask |= 0xFull;
    if (mask == 0 || mask == ~0ull)
        return 0;
    return (~mask) + 1;
}

static void read_bars(struct pci_device *dev)
{
    for (int i = 0; i < 6; i++) {
        uint32_t raw = config_read(dev->bus, dev->slot, dev->func,
                                   (uint8_t)(0x10 + i * 4));

        if (raw & 1) {
            dev->bar[i] = raw & ~0x3u;
            dev->bar_is_io[i] = true;
            dev->bar_size[i] = probe_bar_size(dev, i, false);
            continue;
        }

        bool wide = ((raw >> 1) & 3) == 2;

        dev->bar[i] = raw & ~0xFu;
        dev->bar_is_io[i] = false;
        dev->bar_size[i] = probe_bar_size(dev, i, wide);

        /* Eine 64-Bit-Adresse belegt zwei Eintraege. Moderne Geraete
         * liegen durchaus oberhalb von vier Gigabyte. */
        if (wide) {
            uint32_t high = config_read(dev->bus, dev->slot, dev->func,
                                        (uint8_t)(0x10 + (i + 1) * 4));

            dev->bar[i] |= (uint64_t)high << 32;
            i++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Faehigkeiten und MSI                                                */
/* ------------------------------------------------------------------ */

uint8_t pci_find_capability(const struct pci_device *dev, uint8_t id)
{
    uint16_t status = pci_read16(dev, 0x06);

    if (!(status & (1 << 4)))       /* kennt keine Faehigkeitenliste */
        return 0;

    uint8_t offset = (uint8_t)(pci_read32(dev, 0x34) & 0xFC);

    for (int guard = 0; guard < 48 && offset >= 0x40; guard++) {
        uint32_t entry = pci_read32(dev, offset);

        if ((entry & 0xFF) == id)
            return offset;
        offset = (uint8_t)((entry >> 8) & 0xFC);
        if (!offset)
            break;
    }
    return 0;
}

void pci_set_intx(const struct pci_device *dev, bool enabled)
{
    uint16_t command = pci_read16(dev, 0x04);

    if (enabled)
        command &= (uint16_t)~(1 << 10);
    else
        command |= (uint16_t)(1 << 10);
    pci_write16((struct pci_device *)dev, 0x04, command);
}

static bool enable_msix(const struct pci_device *dev, uint8_t cap,
                        uint8_t vector)
{
    uint32_t head = pci_read32(dev, cap);
    uint32_t table = pci_read32(dev, (uint8_t)(cap + 4));
    uint32_t entries = ((head >> 16) & 0x7FF) + 1;
    uint8_t  bar_index = (uint8_t)(table & 7);
    uint32_t table_offset = table & ~7u;

    if (bar_index > 5 || dev->bar[bar_index] == 0 || dev->bar_is_io[bar_index])
        return false;

    volatile uint32_t *entry = phys_to_virt(dev->bar[bar_index] +
                                            table_offset);

    /* Alle Eintraege stilllegen, den ersten auf unseren Vektor legen. */
    for (uint32_t i = 0; i < entries; i++)
        entry[i * 4 + 3] = 1;

    uint64_t address = apic_msi_address();

    entry[0] = (uint32_t)address;
    entry[1] = (uint32_t)(address >> 32);
    entry[2] = apic_msi_data(vector);
    entry[3] = 0;                        /* Sperre aufheben */

    /* Einschalten und die Gesamtsperre loesen. */
    head |= 1u << 31;
    head &= ~(1u << 30);
    pci_write32((struct pci_device *)dev, cap, head);
    return true;
}

static bool enable_msi(const struct pci_device *dev, uint8_t cap,
                       uint8_t vector)
{
    uint16_t control = pci_read16(dev, (uint8_t)(cap + 2));
    bool wide = (control & (1 << 7)) != 0;
    uint64_t address = apic_msi_address();

    pci_write32((struct pci_device *)dev, (uint8_t)(cap + 4),
                (uint32_t)address);
    if (wide) {
        pci_write32((struct pci_device *)dev, (uint8_t)(cap + 8),
                    (uint32_t)(address >> 32));
        pci_write16((struct pci_device *)dev, (uint8_t)(cap + 12),
                    (uint16_t)apic_msi_data(vector));
    } else {
        pci_write16((struct pci_device *)dev, (uint8_t)(cap + 8),
                    (uint16_t)apic_msi_data(vector));
    }

    /* Genau eine Unterbrechung, dann einschalten. */
    control &= (uint16_t)~(7 << 4);
    control |= 1;
    pci_write16((struct pci_device *)dev, (uint8_t)(cap + 2), control);
    return true;
}

bool pci_enable_msi(const struct pci_device *dev, uint8_t vector)
{
    if (!apic_available())
        return false;

    uint8_t cap = pci_find_capability(dev, PCI_CAP_MSIX);

    if (cap && enable_msix(dev, cap, vector)) {
        pci_set_intx(dev, false);
        return true;
    }

    cap = pci_find_capability(dev, PCI_CAP_MSI);
    if (cap && enable_msi(dev, cap, vector)) {
        pci_set_intx(dev, false);
        return true;
    }
    return false;
}

static void probe(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t id = config_read(bus, slot, func, 0x00);

    if ((id & 0xFFFF) == 0xFFFF)
        return;
    if (device_count >= PCI_MAX_DEVICES)
        return;

    struct pci_device *dev = &devices[device_count];

    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor_id = (uint16_t)(id & 0xFFFF);
    dev->device_id = (uint16_t)(id >> 16);

    uint32_t classes = config_read(bus, slot, func, 0x08);
    dev->prog_if    = (uint8_t)((classes >> 8)  & 0xFF);
    dev->subclass   = (uint8_t)((classes >> 16) & 0xFF);
    dev->class_code = (uint8_t)((classes >> 24) & 0xFF);

    dev->irq_line = (uint8_t)(config_read(bus, slot, func, 0x3C) & 0xFF);

    read_bars(dev);
    device_count++;
}

void pci_init(void)
{
    device_count = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = config_read((uint8_t)bus, slot, 0, 0x00);

            if ((id & 0xFFFF) == 0xFFFF)
                continue;

            probe((uint8_t)bus, slot, 0);

            /* Bit 7 im Header-Typ zeigt ein Geraet mit mehreren Funktionen. */
            uint32_t header = config_read((uint8_t)bus, slot, 0, 0x0C);
            if ((header >> 16) & 0x80) {
                for (uint8_t func = 1; func < 8; func++)
                    probe((uint8_t)bus, slot, func);
            }
        }
    }

    kprintf("PCI         : %u Geraete gefunden\n", (unsigned)device_count);
    for (size_t i = 0; i < device_count; i++) {
        const struct pci_device *d = &devices[i];

        kprintf("  %02u:%02u.%u  %04x:%04x  %s\n",
                d->bus, d->slot, d->func, d->vendor_id, d->device_id,
                pci_class_name(d->class_code, d->subclass));
    }
}

size_t pci_device_count(void) { return device_count; }

const struct pci_device *pci_device_at(size_t index)
{
    return index < device_count ? &devices[index] : NULL;
}

const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    for (size_t i = 0; i < device_count; i++) {
        if (devices[i].class_code == class_code && devices[i].subclass == subclass)
            return &devices[i];
    }
    return NULL;
}

const struct pci_device *pci_find_device(uint16_t vendor, uint16_t device)
{
    for (size_t i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor && devices[i].device_id == device)
            return &devices[i];
    }
    return NULL;
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass)
{
    switch (class_code) {
    case 0x01:
        switch (subclass) {
        case 0x01: return "IDE-Controller";
        case 0x06: return "SATA-Controller (AHCI)";
        case 0x08: return "NVMe-Controller";
        default:   return "Massenspeicher";
        }
    case 0x02: return "Netzwerkkarte";
    case 0x03: return "Grafikkarte";
    case 0x04: return "Multimedia";
    case 0x06: return "Bruecke";
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB-Controller";
        default:   return "serieller Bus";
        }
    default:   return "sonstiges Geraet";
    }
}
