/* nic.c - Auswahl und Betrieb der Netzwerkkarte.
 *
 * Die Reihenfolge der Treiber ist nicht beliebig: Wo eine virtuelle
 * Maschine virtio anbietet, ist das der schnellste Weg, und die
 * neueren Intel-Karten wollen den e1000e-Treiber, nicht den alten.
 * Deshalb wird von oben nach unten gefragt, und der erste, der sich
 * zustaendig fuehlt, bekommt die Karte.
 */

#include "nic.h"
#include "net.h"
#include "kstring.h"

#define MAX_SEEN 8

static const struct nic_driver *const drivers[] = {
    &virtio_net_driver,
    &igb_driver,
    &e1000e_driver,
    &e1000_driver,
    &rtl8169_driver,
    &rtl8139_driver,
};

static struct nic active;
static bool       have_nic;

static char   seen[MAX_SEEN][64];
static size_t seen_count;

/* Ein paar Karten, die RetroOS erkennt, aber (noch) nicht bedient -
 * die Meldung soll dem Benutzer sagen, woran es liegt. */
static const char *known_unsupported(uint16_t vendor, uint16_t device)
{
    if (vendor == 0x15AD && device == 0x07B0)
        return "VMware vmxnet3";
    if (vendor == 0x1022 && device == 0x2000)
        return "AMD PCnet";
    if (vendor == 0x10EC && device == 0x8125)
        return "Realtek RTL8125 (2,5 GBit)";
    return NULL;
}

static void remember(const struct pci_device *pci, const char *name)
{
    if (seen_count >= MAX_SEEN)
        return;
    ksnprintf(seen[seen_count], sizeof(seen[0]), "%04x:%04x %s",
              pci->vendor_id, pci->device_id, name);
    seen_count++;
}

bool nic_init(void)
{
    memset(&active, 0, sizeof(active));
    have_nic = false;
    seen_count = 0;

    for (size_t i = 0; i < pci_device_count(); i++) {
        const struct pci_device *pci = pci_device_at(i);

        /* Klasse 2, Unterklasse 0 ist ein Ethernet-Controller. */
        if (!pci || pci->class_code != 0x02 || pci->subclass != 0x00)
            continue;

        const struct nic_driver *driver = NULL;

        for (size_t k = 0; k < ARRAY_LEN(drivers); k++) {
            if (drivers[k]->probe && drivers[k]->probe(pci)) {
                driver = drivers[k];
                break;
            }
        }

        if (!driver) {
            const char *name = known_unsupported(pci->vendor_id,
                                                 pci->device_id);

            remember(pci, name ? name : "ohne Treiber");
            kprintf("Netzwerk    : %04x:%04x - kein Treiber%s%s\n",
                    pci->vendor_id, pci->device_id,
                    name ? " fuer " : "", name ? name : "");
            continue;
        }

        if (have_nic) {
            /* Eine zweite Karte wird vermerkt, aber nicht benutzt -
             * RetroOS fuehrt nur eine Schnittstelle. */
            remember(pci, driver->family);
            continue;
        }

        struct nic candidate;

        memset(&candidate, 0, sizeof(candidate));
        candidate.pci = pci;
        candidate.family = driver->family;
        strlcpy(candidate.model, driver->family, sizeof(candidate.model));

        if (!driver->attach(pci, &candidate)) {
            remember(pci, "antwortet nicht");
            kprintf("Netzwerk    : %s bei %02x:%02x.%u antwortet nicht\n",
                    driver->family, pci->bus, pci->slot, pci->func);
            continue;
        }

        active = candidate;
        have_nic = true;
        remember(pci, candidate.model);
    }

    return have_nic;
}

bool nic_present(void) { return have_nic; }

const char *nic_model(void)
{
    return have_nic ? active.model : "keine gefunden";
}

const char *nic_family(void)
{
    return have_nic && active.family ? active.family : "";
}

uint32_t nic_speed(void)
{
    return have_nic ? active.speed_mbit : 0;
}

bool nic_link_up(void)
{
    if (!have_nic)
        return false;
    if (!active.ops->link_up)
        return true;               /* die Karte sagt es nicht - annehmen */
    return active.ops->link_up(&active);
}

bool nic_send(const void *frame, uint16_t length)
{
    if (!have_nic || length == 0)
        return false;
    return active.ops->send(&active, frame, length);
}

uint16_t nic_receive(void *buffer, uint16_t capacity)
{
    if (!have_nic)
        return 0;
    return active.ops->receive(&active, buffer, capacity);
}

size_t nic_seen_count(void) { return seen_count; }

const char *nic_seen_at(size_t index)
{
    return index < seen_count ? seen[index] : NULL;
}
