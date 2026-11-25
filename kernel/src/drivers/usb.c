/* usb.c - die gemeinsame Schnittstelle der USB-Controller.
 *
 * Die Treiber darueber - HID, Speicher, Verteiler - sprechen ein
 * Geraet an und wissen nicht, an welchem Controller es haengt. Hier
 * wird entschieden, wohin der Aufruf geht: Gehoert das Geraet dem
 * OHCI-Treiber, zu ihm, sonst zu xHCI.
 *
 * Massenendpunkte (Speicher) und Verteiler gibt es an beiden.
 */

#include "usb_hcd.h"

bool usb_control(struct usb_device *dev, const struct usb_setup *setup,
                 void *buffer)
{
    if (ohci_owns(dev))
        return ohci_control(dev, setup, buffer);
    return xhci_control(dev, setup, buffer);
}

size_t usb_interrupt_poll(struct usb_device *dev, void *buffer, size_t size)
{
    if (ohci_owns(dev))
        return ohci_interrupt_poll(dev, buffer, size);
    return xhci_interrupt_poll(dev, buffer, size);
}

bool usb_bulk(struct usb_device *dev, uint8_t endpoint, void *buffer,
              uint32_t length, bool in, uint32_t *transferred)
{
    if (ohci_owns(dev))
        return ohci_bulk(dev, endpoint, buffer, length, in, transferred);
    return xhci_bulk(dev, endpoint, buffer, length, in, transferred);
}

bool usb_setup_bulk_from_config(struct usb_device *dev)
{
    if (ohci_owns(dev))
        return ohci_setup_bulk_from_config(dev);
    return xhci_setup_bulk_from_config(dev);
}

bool usb_clear_halt(struct usb_device *dev, uint8_t endpoint)
{
    /* Am OHCI muss auch die Beschreibung im Speicher zurueckgesetzt
     * werden, nicht nur das Geraet - sonst bleibt der Endpunkt
     * angehalten. */
    if (ohci_owns(dev))
        return ohci_clear_halt(dev, endpoint);

    struct usb_setup setup = {
        .request_type = 0x02,               /* an den Endpunkt */
        .request = 0x01,                    /* CLEAR_FEATURE   */
        .value = 0,                         /* ENDPOINT_HALT   */
        .index = endpoint,
        .length = 0,
    };

    return usb_control(dev, &setup, NULL);
}

const struct usb_device_info *usb_device_details(struct usb_device *dev)
{
    if (ohci_owns(dev))
        return ohci_device_details(dev);
    return xhci_device_details(dev);
}

size_t usb_device_count(void)
{
    return xhci_device_count() + ohci_device_count();
}

/* Erst die Geraete am xHCI, dann die am OHCI. */
struct usb_device *usb_device_at(size_t index)
{
    size_t x = xhci_device_count();

    if (index < x)
        return xhci_device_at(index);
    return ohci_device_at(index - x);
}

const char *usb_speed_name(uint8_t speed)
{
    switch (speed) {
    case 1: return "Full Speed";
    case 2: return "Low Speed";
    case 3: return "High Speed";
    case 4: return "SuperSpeed";
    case 5: return "SuperSpeed+";
    default: return "unbekannt";
    }
}
