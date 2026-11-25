/* usbconfig.c - siehe usbconfig.h. */

#include "usbconfig.h"
#include "kstring.h"

#define DESC_INTERFACE 4
#define DESC_ENDPOINT  5
#define DESC_HID       0x21
#define DESC_HID_REPORT 0x22

bool usb_parse_config(const uint8_t *full, size_t total,
                      struct usb_config *out)
{
    if (!out)
        return false;

    memset(out, 0, sizeof(*out));

    if (!full || total < 6)
        return false;

    out->configuration = full[5];

    size_t at = 0;

    while (at + 2 <= total) {
        uint8_t length = full[at];
        uint8_t type = full[at + 1];

        /* Eine Beschreibung, die ueber das Ende hinausreicht oder gar
         * keine Laenge hat, ist das Ende. */
        if (length < 2 || at + length > total)
            break;

        if (type == DESC_INTERFACE && length >= 9) {
            /* Die erste Schnittstelle mit Unterbrechungseingang
             * genuegt. */
            if (out->have_interface && out->int_address)
                break;

            out->interface_number = full[at + 2];
            out->interface_class = full[at + 5];
            out->interface_subclass = full[at + 6];
            out->interface_protocol = full[at + 7];
            out->hid_report_length = 0;
            out->have_interface = true;
            out->int_address = 0;
        } else if (type == DESC_HID && length >= 9 && out->have_interface) {
            /* Die HID-Beschreibung zaehlt die Beschreibungen auf, die
             * das Geraet zusaetzlich hat: je Art und Laenge. Uns
             * interessiert die Berichtsbeschreibung. */
            uint8_t count = full[at + 5];

            for (uint8_t i = 0; i < count && (size_t)6 + 3u * i + 3 <= length; i++) {
                size_t entry = at + 6 + 3u * i;

                if (full[entry] == DESC_HID_REPORT)
                    out->hid_report_length =
                        (uint16_t)(full[entry + 1] | (full[entry + 2] << 8));
            }
        } else if (type == DESC_ENDPOINT && length >= 7 && out->have_interface) {
            uint8_t address = full[at + 2];
            uint8_t attributes = full[at + 3];
            uint16_t packet = (uint16_t)((full[at + 4] |
                                          (full[at + 5] << 8)) & 0x7FF);

            if ((attributes & 3) == 3 && (address & 0x80)) {
                out->int_address = address;
                out->int_packet = packet;
                out->int_interval = full[at + 6];
            } else if ((attributes & 3) == 2) {
                /* Massenendpunkt - zwei davon braucht ein Speicher. */
                if (address & 0x80) {
                    out->bulk_in = address & 0x0F;
                    out->bulk_in_packet = packet;
                } else {
                    out->bulk_out = address & 0x0F;
                    out->bulk_out_packet = packet;
                }
            }
        }
        at += length;
    }

    return out->have_interface;
}
