/* usbhub.h - Verteiler (Hubs): Beschreibung und Anschlusszustand lesen.
 *
 * Ein Verteiler ist ein Geraet wie jedes andere, nur dass hinter ihm
 * weitere Anschluesse liegen. Er wird ueber Klassenanfragen im
 * Steuerkanal bedient: Strom ein, Anschluss zuruecksetzen, Zustand
 * abfragen. Die Antworten sind kleine Bitfelder - und genau dort lauern
 * die Fehler: ein Bit, das "langsames Geraet" heisst und die ganze
 * Aufzaehlung auf die falsche Geschwindigkeit stellt, oder ein
 * Aenderungsbit, das nie geloescht wird und den Anschluss fuer immer
 * "neu" meldet.
 *
 * Das Lesen steht darum hier, ohne Controller, und wird auf dem
 * Entwicklungsrechner geprueft. Es gilt fuer jeden Controller.
 */
#ifndef USBHUB_H
#define USBHUB_H

#include "usb.h"

#define USB_CLASS_HUB          0x09
#define USB_DESC_HUB           0x29

/* Merkmale eines Anschlusses (SetPortFeature/ClearPortFeature) */
#define HUB_FEAT_PORT_RESET    4
#define HUB_FEAT_PORT_POWER    8
#define HUB_FEAT_C_CONNECTION  16
#define HUB_FEAT_C_ENABLE      17
#define HUB_FEAT_C_RESET       20

#define HUB_MAX_PORTS          15

struct usb_hub_info {
    uint8_t ports;
    uint8_t power_good_ms;     /* nach dem Einschalten so lange warten */
};

/* Liest die Verteilerbeschreibung. false bei zu kurzen, falsch
 * benannten oder anschlusslosen. Mehr als HUB_MAX_PORTS gibt es nicht. */
bool usb_parse_hub_descriptor(const uint8_t *d, size_t length,
                              struct usb_hub_info *out);

struct usb_hub_port {
    bool connected, enabled, suspended, overcurrent, reset, powered;
    bool low_speed, high_speed;
    bool changed_connection, changed_enable, changed_reset;
};

/* Aus den vier Byte von GetPortStatus: zwei Byte Zustand, zwei Byte
 * Aenderungen, little endian. */
void usb_decode_hub_port(const uint8_t raw[4], struct usb_hub_port *out);

/* Geschwindigkeit im Sinne von usb_speed_name: 1 Full, 2 Low, 3 High. */
uint8_t usb_hub_port_speed(const struct usb_hub_port *port);

/* Die Anfragen. */
void usb_hub_get_descriptor_setup(struct usb_setup *out);
void usb_hub_port_feature_setup(struct usb_setup *out, uint8_t port,
                                uint16_t feature, bool set);
void usb_hub_port_status_setup(struct usb_setup *out, uint8_t port);

#endif /* USBHUB_H */
