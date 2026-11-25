/* usbhub.c - siehe usbhub.h. */

#include "usbhub.h"
#include "kstring.h"

bool usb_parse_hub_descriptor(const uint8_t *d, size_t length,
                              struct usb_hub_info *out)
{
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    if (!d || length < 7 || d[1] != USB_DESC_HUB || d[0] < 7)
        return false;

    uint8_t ports = d[2];

    if (ports == 0 || ports > HUB_MAX_PORTS)
        return false;

    out->ports = ports;

    /* Die Zeit steht in Einheiten zu zwei Millisekunden; null gibt es
     * bei Geraeten, die es nicht so genau nehmen - dann warten wir
     * trotzdem, ein Verteiler braucht nach dem Einschalten einen
     * Moment. */
    unsigned ms = (unsigned)d[5] * 2;

    out->power_good_ms = (uint8_t)(ms < 20 ? 20 : (ms > 250 ? 250 : ms));
    return true;
}

void usb_decode_hub_port(const uint8_t raw[4], struct usb_hub_port *out)
{
    uint16_t status = (uint16_t)(raw[0] | (raw[1] << 8));
    uint16_t change = (uint16_t)(raw[2] | (raw[3] << 8));

    out->connected   = (status >> 0) & 1;
    out->enabled     = (status >> 1) & 1;
    out->suspended   = (status >> 2) & 1;
    out->overcurrent = (status >> 3) & 1;
    out->reset       = (status >> 4) & 1;
    out->powered     = (status >> 8) & 1;
    out->low_speed   = (status >> 9) & 1;
    out->high_speed  = (status >> 10) & 1;

    out->changed_connection = (change >> 0) & 1;
    out->changed_enable     = (change >> 1) & 1;
    out->changed_reset      = (change >> 4) & 1;
}

uint8_t usb_hub_port_speed(const struct usb_hub_port *port)
{
    if (port->low_speed)
        return 2;
    if (port->high_speed)
        return 3;
    return 1;
}

void usb_hub_get_descriptor_setup(struct usb_setup *out)
{
    out->request_type = USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE;
    out->request = USB_REQ_GET_DESCRIPTOR;
    out->value = (uint16_t)(USB_DESC_HUB << 8);
    out->index = 0;
    out->length = 9;
}

void usb_hub_port_feature_setup(struct usb_setup *out, uint8_t port,
                                uint16_t feature, bool set)
{
    /* An den Anschluss gerichtet (Empfaenger "andere"), Klassenanfrage. */
    out->request_type = USB_TYPE_CLASS | 0x03;
    out->request = set ? 0x03 : 0x01;          /* SET_FEATURE / CLEAR_FEATURE */
    out->value = feature;
    out->index = port;
    out->length = 0;
}

void usb_hub_port_status_setup(struct usb_setup *out, uint8_t port)
{
    out->request_type = USB_DIR_IN | USB_TYPE_CLASS | 0x03;
    out->request = 0x00;                        /* GET_STATUS */
    out->value = 0;
    out->index = port;
    out->length = 4;
}
