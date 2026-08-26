/* usb.h - der USB-Stapel von RetroOS.
 *
 * Ein heutiger Rechner hat oft gar keinen PS/2-Anschluss mehr; Tastatur
 * und Maus haengen am USB-Bus. Umgesetzt ist der xHCI-Controller, wie
 * ihn jeder Chipsatz seit etwa 2010 mitbringt, dazu die Aufzaehlung der
 * angeschlossenen Geraete und das Boot-Protokoll fuer Tastatur und Maus.
 */
#ifndef USB_H
#define USB_H

#include "retro.h"

/* --- Anfragen im Steuerkanal --- */
struct usb_setup {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} PACKED;

#define USB_DIR_IN            0x80
#define USB_TYPE_STANDARD     0x00
#define USB_TYPE_CLASS        0x20
#define USB_RECIP_DEVICE      0x00
#define USB_RECIP_INTERFACE   0x01

#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_CONFIG     0x09
#define USB_REQ_SET_INTERFACE  0x0B
#define USB_REQ_SET_PROTOCOL   0x0B   /* HID, an die Schnittstelle */
#define USB_REQ_SET_IDLE       0x0A

#define USB_DESC_DEVICE        1
#define USB_DESC_CONFIG        2
#define USB_DESC_STRING        3
#define USB_DESC_INTERFACE     4
#define USB_DESC_ENDPOINT      5

#define USB_CLASS_HID          0x03
#define HID_SUBCLASS_BOOT      0x01
#define HID_PROTOCOL_KEYBOARD  0x01
#define HID_PROTOCOL_MOUSE     0x02

struct usb_device;

/* --- Was ein Treiber vom Controller braucht --- */

/* Eine Anfrage ueber den Steuerkanal. */
bool usb_control(struct usb_device *dev, const struct usb_setup *setup,
                 void *buffer);

/* Holt das zuletzt vom Geraet gemeldete Paket der Unterbrechungsleitung.
 * Gibt die Laenge zurueck, 0 wenn nichts Neues da ist. */
size_t usb_interrupt_poll(struct usb_device *dev, void *buffer, size_t size);

/* --- Aufzaehlung --- */
struct usb_device_info {
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  device_class;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;
    uint8_t  interface_number;
    uint16_t max_packet;
    uint8_t  port;
    uint8_t  speed;
};

const struct usb_device_info *usb_device_details(struct usb_device *dev);

/* --- Einrichten --- */
void xhci_init(void);
size_t usb_device_count(void);
struct usb_device *usb_device_at(size_t index);

/* Wird vom Controller gerufen, sobald ein Geraet bereit ist. */
void usb_hid_attach(struct usb_device *dev);
void usb_hid_poll_all(void);

/* Der Thread, der die Eingabegeraete abfragt. */
void usb_start_polling(void);

const char *usb_speed_name(uint8_t speed);

#endif /* USB_H */
