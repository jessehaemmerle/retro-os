/* usb_hcd.h - was ein USB-Controller dem Rest des Systems bietet.
 *
 * RetroOS spricht zwei Controller: xHCI, wie ihn jeder Chipsatz seit
 * etwa 2010 hat, und OHCI, den aeltere Chipsaetze - und VirtualBox ab
 * Werk - mitbringen. Tastatur-, Maus- und Speichertreiber sollen davon
 * nichts wissen muessen; sie rufen usb_control(), usb_interrupt_poll()
 * und die anderen aus usb.h. Die stehen in usb.c und reichen an den
 * Controller weiter, dem das Geraet gehoert.
 *
 * Ein Geraet wird ueber einen undurchsichtigen Zeiger angesprochen. Wem
 * er gehoert, sagt ohci_owns(); was nicht OHCI ist, ist xHCI.
 */
#ifndef USB_HCD_H
#define USB_HCD_H

#include "usb.h"

/* --- xHCI (xhci.c) --- */
bool   xhci_control(struct usb_device *dev, const struct usb_setup *setup,
                    void *buffer);
size_t xhci_interrupt_poll(struct usb_device *dev, void *buffer, size_t size);
bool   xhci_bulk(struct usb_device *dev, uint8_t endpoint, void *buffer,
                 uint32_t length, bool in, uint32_t *transferred);
bool   xhci_setup_bulk_from_config(struct usb_device *dev);
const struct usb_device_info *xhci_device_details(struct usb_device *dev);
size_t xhci_device_count(void);
struct usb_device *xhci_device_at(size_t index);

/* --- OHCI (ohci.c) --- */
bool   ohci_owns(const struct usb_device *dev);
bool   ohci_control(struct usb_device *dev, const struct usb_setup *setup,
                    void *buffer);
size_t ohci_interrupt_poll(struct usb_device *dev, void *buffer, size_t size);
bool   ohci_bulk(struct usb_device *dev, uint8_t endpoint, void *buffer,
                 uint32_t length, bool in, uint32_t *transferred);
bool   ohci_setup_bulk_from_config(struct usb_device *dev);
bool   ohci_clear_halt(struct usb_device *dev, uint8_t endpoint);
const struct usb_device_info *ohci_device_details(struct usb_device *dev);
size_t ohci_device_count(void);
struct usb_device *ohci_device_at(size_t index);

#endif /* USB_HCD_H */
