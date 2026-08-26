/* pci.c - PCI-Bus ueber den alten Konfigurationsmechanismus 1.
 *
 * Zwei I/O-Ports genuegen, um den gesamten Bus abzufragen: in 0xCF8 wird
 * die Adresse geschrieben, aus 0xCFC das Datenwort gelesen. Das reicht fuer
 * alles, was RetroOS braucht - Festplattencontroller und Netzwerkkarte.
 */

#include "pci.h"
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

static void read_bars(struct pci_device *dev)
{
    for (int i = 0; i < 6; i++) {
        uint32_t raw = config_read(dev->bus, dev->slot, dev->func,
                                   (uint8_t)(0x10 + i * 4));

        if (raw & 1) {
            dev->bar[i] = raw & ~0x3u;
            dev->bar_is_io[i] = true;
        } else {
            dev->bar[i] = raw & ~0xFu;
            dev->bar_is_io[i] = false;

            /* 64-Bit-Adressen belegen zwei Eintraege; der obere Teil wird
             * uebersprungen, da RetroOS nur unterhalb von 4 GiB arbeitet. */
            if (((raw >> 1) & 3) == 2)
                i++;
        }
    }
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
