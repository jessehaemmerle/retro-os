/* pci.h - Auffinden und Ansprechen von PCI-Geraeten. */
#ifndef PCI_H
#define PCI_H

#include "retro.h"

#define PCI_MAX_DEVICES 32

struct pci_device {
    uint8_t  bus, slot, func;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  irq_line;

    uint32_t bar[6];        /* Basisadressen, bereits maskiert */
    bool     bar_is_io[6];
};

void pci_init(void);

size_t pci_device_count(void);
const struct pci_device *pci_device_at(size_t index);

/* Erstes Geraet mit passender Klasse bzw. Kennung. */
const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass);
const struct pci_device *pci_find_device(uint16_t vendor, uint16_t device);

uint32_t pci_read32(const struct pci_device *dev, uint8_t offset);
uint16_t pci_read16(const struct pci_device *dev, uint8_t offset);
void     pci_write32(const struct pci_device *dev, uint8_t offset, uint32_t value);
void     pci_write16(const struct pci_device *dev, uint8_t offset, uint16_t value);

/* Busmaster und Speicherzugriff freischalten - noetig fuer DMA-Geraete. */
void pci_enable_bus_master(const struct pci_device *dev);

const char *pci_class_name(uint8_t class_code, uint8_t subclass);

#endif /* PCI_H */
