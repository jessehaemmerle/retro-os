/* usbconfig.h - die Konfigurationsbeschreibung eines USB-Geraets lesen.
 *
 * Ein Geraet gibt auf Anfrage eine Folge von Beschreibungen heraus:
 * erst die Konfiguration, dann ihre Schnittstellen, dazu jeweils deren
 * Endpunkte - und bei HID-Geraeten, dazwischen, eine Beschreibung der
 * Berichtsbeschreibung.
 *
 * Gelesen werden muss das von jedem Controller, nicht nur von einem:
 * der xHCI-Treiber braucht es, der OHCI-Treiber ebenso. Die Funktion
 * steht darum hier, ohne Zugriff auf irgendeine Hardware - und laesst
 * sich mit den Bytes echter Geraete auf dem Entwicklungsrechner
 * pruefen.
 */
#ifndef USBCONFIG_H
#define USBCONFIG_H

#include "retro.h"

struct usb_config {
    uint8_t  configuration;          /* Wert fuer SET_CONFIGURATION */
    bool     have_interface;

    uint8_t  interface_number;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;

    /* Laenge der HID-Berichtsbeschreibung, 0 wenn keine gemeldet. */
    uint16_t hid_report_length;

    /* Der Unterbrechungseingang der Schnittstelle, 0 = keiner. */
    uint8_t  int_address;            /* mit Richtungsbit */
    uint16_t int_packet;
    uint8_t  int_interval;

    /* Massenendpunkte, als Nummer ohne Richtungsbit; 0 = keiner. */
    uint8_t  bulk_in, bulk_out;
    uint16_t bulk_in_packet, bulk_out_packet;
};

/* Liest eine vollstaendige Konfigurationsbeschreibung (die Bytes ab dem
 * ersten Konfigurationsdeskriptor, wTotalLength lang).
 *
 * Gesucht wird die erste Schnittstelle, die einen Unterbrechungseingang
 * hat; kommt danach noch eine, ist Schluss. Gibt es keine solche, gilt
 * die letzte. Das ist bei Tastatur, Maus und Tablett dieselbe - und
 * bei einem Speicher, der keinen Unterbrechungseingang braucht,
 * ebenfalls.
 *
 * false, wenn gar keine Schnittstelle gefunden wurde. */
bool usb_parse_config(const uint8_t *desc, size_t length,
                      struct usb_config *out);

#endif /* USBCONFIG_H */
