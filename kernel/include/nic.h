/* nic.h - eine gemeinsame Schnittstelle fuer Netzwerkkarten.
 *
 * Frueher war der Treiber der Intel-Karte fest eingebaut. Ein heutiger
 * Rechner bringt aber irgendeine von einem halben Dutzend Familien mit -
 * Realtek im Mainboard, Intel im Notebook, virtio in der virtuellen
 * Maschine. Deshalb melden sich die Treiber hier an, und der Kern nimmt
 * die erste Karte, die antwortet.
 */
#ifndef NIC_H
#define NIC_H

#include "retro.h"
#include "pci.h"

struct mac_addr;
struct nic;

struct nic_ops {
    /* Schickt einen fertigen Ethernet-Rahmen los. */
    bool     (*send)(struct nic *nic, const void *frame, uint16_t length);
    /* Holt hoechstens einen Rahmen ab; 0 heisst "nichts da". */
    uint16_t (*receive)(struct nic *nic, void *buffer, uint16_t capacity);
    /* Steht die Verbindung? NULL, wenn die Karte es nicht sagen kann. */
    bool     (*link_up)(struct nic *nic);
};

struct nic {
    char                  model[48];
    const char           *family;     /* Name des Treibers */
    const struct nic_ops *ops;
    void                 *state;      /* gehoert dem Treiber */

    const struct pci_device *pci;
    uint32_t              speed_mbit; /* 0 = unbekannt */
};

struct nic_driver {
    const char *family;
    /* Faehlt sich dieser Treiber fuer die Karte zustaendig? */
    bool (*probe)(const struct pci_device *pci);
    /* Nimmt sie in Betrieb und fuellt nic aus. */
    bool (*attach)(const struct pci_device *pci, struct nic *nic);
};

/* Sucht auf dem PCI-Bus nach einer Karte, fuer die es einen Treiber
 * gibt, und nimmt die erste davon in Betrieb. */
bool nic_init(void);

bool        nic_present(void);
const char *nic_model(void);
const char *nic_family(void);
bool        nic_link_up(void);
uint32_t    nic_speed(void);

bool     nic_send(const void *frame, uint16_t length);
uint16_t nic_receive(void *buffer, uint16_t capacity);

/* Die Karten, die auf dem Bus gefunden wurden - auch die ohne Treiber. */
size_t      nic_seen_count(void);
const char *nic_seen_at(size_t index);

/* --- Die einzelnen Treiber --- */
extern const struct nic_driver virtio_net_driver;
extern const struct nic_driver igb_driver;
/* Gehoert die Kennung zur Reihe 82575 bis I350? e1000e fragt nach. */
bool igb_owns(uint16_t device_id);
extern const struct nic_driver e1000e_driver;
extern const struct nic_driver e1000_driver;
extern const struct nic_driver rtl8169_driver;
extern const struct nic_driver rtl8139_driver;

#endif /* NIC_H */
