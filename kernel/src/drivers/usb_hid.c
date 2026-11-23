/* usb_hid.c - Tastatur und Maus am USB-Bus.
 *
 * Tastatur und Maus sprechen das Boot-Protokoll: ein festes Paketformat,
 * das jedes BIOS versteht und das deshalb kein Auswerten der
 * Berichtsbeschreibung verlangt. Die Tastatur schickt sechs gleichzeitig
 * gedrueckte Tasten als Verwendungsnummern, die Maus Bewegung und
 * Tastenzustand.
 *
 * Ein Tablett hat kein Boot-Protokoll - und ist doch das Zeigergeraet,
 * mit dem VirtualBox und QEMU ihre virtuellen Maschinen ab Werk
 * ausstatten. Es meldet nicht, wie weit der Zeiger gewandert ist,
 * sondern wo er steht: der Zeiger des Gastgebers und der im Gast liegen
 * damit immer genau uebereinander, ohne dass die Maus eingefangen oder
 * beschleunigt werden muss. Dafuer verlangt es, seine Berichtsbeschreibung
 * zu lesen (hidparse.c) - sonst weiss man nicht, wo in den Bytes X und Y
 * stehen. Dasselbe tut dann jeder andere Zeiger ohne Boot-Protokoll.
 *
 * Die Verwendungsnummern sind nicht die Abtastcodes des PS/2-Anschlusses;
 * sie brauchen eine eigene Tabelle. Angelegt ist sie fuer die deutsche
 * Belegung - dieselbe, die auch die PS/2-Tastatur benutzt.
 */

#include "hidparse.h"
#include "keymap.h"
#include "mouseutil.h"
#include "usb.h"
#include "input.h"
#include "kstring.h"
#include "thread.h"

#define MAX_HID 8

struct hid_device {
    struct usb_device *usb;
    bool               keyboard;
    uint8_t            previous[8];    /* letztes Tastaturpaket */

    /* Ein Zeiger im Berichtsmodus: Wo seine Felder liegen, steht in
     * layout, gelesen aus der Berichtsbeschreibung. */
    bool               pointer;
    struct hid_pointer layout;
};

static struct hid_device hids[MAX_HID];
static size_t            hid_count;

/* ------------------------------------------------------------------ */
/* Verwendungsnummern der Tastatur                                     */
/* ------------------------------------------------------------------ */

/* Die acht Steuertasten stehen im ersten Byte des Pakets. */
static const uint16_t modifier_key[8] = {
    KEY_LCTRL, KEY_LSHIFT, KEY_LALT, KEY_SUPER,
    KEY_RCTRL, KEY_RSHIFT, KEY_RALT, KEY_SUPER,
};

static uint16_t translate(uint8_t usage, uint8_t modifiers)
{
    if (usage < KEYMAP_HID_FIRST || usage > KEYMAP_HID_LAST)
        return 0;

    const struct keymap *map = keymap_current();
    size_t index = usage - KEYMAP_HID_FIRST;
    bool shift = (modifiers & 0x22) != 0;       /* linke oder rechte */
    bool altgr = (modifiers & 0x40) != 0;       /* rechte Alt-Taste  */

    if (altgr && map->hid_altgr[index])
        return map->hid_altgr[index];
    if (shift && map->hid_shift[index])
        return map->hid_shift[index];
    return map->hid_base[index];
}

/* ------------------------------------------------------------------ */
/* Tastaturpakete                                                      */
/* ------------------------------------------------------------------ */

static bool contains(const uint8_t *keys, uint8_t usage)
{
    for (int i = 0; i < 6; i++)
        if (keys[i] == usage)
            return true;
    return false;
}

static void handle_keyboard(struct hid_device *hid, const uint8_t *report)
{
    uint8_t modifiers = report[0];
    uint8_t previous_modifiers = hid->previous[0];

    /* Steuertasten einzeln nachziehen. */
    for (int bit = 0; bit < 8; bit++) {
        bool now = (modifiers >> bit) & 1;
        bool before = (previous_modifiers >> bit) & 1;

        if (now != before)
            keyboard_inject(modifier_key[bit], now);
    }

    /* Losgelassene Tasten: standen vorher im Paket, jetzt nicht mehr. */
    for (int i = 0; i < 6; i++) {
        uint8_t usage = hid->previous[2 + i];

        if (usage > 3 && !contains(report + 2, usage))
            keyboard_inject(translate(usage, previous_modifiers), false);
    }

    /* Neu gedrueckte Tasten. */
    for (int i = 0; i < 6; i++) {
        uint8_t usage = report[2 + i];

        if (usage > 3 && !contains(hid->previous + 2, usage))
            keyboard_inject(translate(usage, modifiers), true);
    }

    memcpy(hid->previous, report, 8);
}

/* ------------------------------------------------------------------ */
/* Mauspakete                                                          */
/* ------------------------------------------------------------------ */

static void handle_mouse(const uint8_t *report, size_t length)
{
    uint8_t buttons = report[0];
    int32_t dx = (int8_t)report[1];
    int32_t dy = (int8_t)report[2];
    int8_t  scroll = length > 3 ? (int8_t)report[3] : 0;

    mouse_inject(dx, dy, scroll,
                 (buttons & 1) != 0, (buttons & 2) != 0, (buttons & 4) != 0);
}

/* Ein Bericht eines Zeigers im Berichtsmodus. */
static void handle_pointer(const struct hid_device *hid,
                           const uint8_t *report, size_t length)
{
    const struct hid_pointer *p = &hid->layout;

    /* Mit Berichtsnummer steht sie vorn - und ein Bericht einer anderen
     * Nummer gehoert zu etwas anderem. */
    if (p->has_id && (length < 1 || report[0] != p->id))
        return;

    /* Ein Bericht, in dem nicht alles liegt, was wir lesen wollen, wird
     * verworfen, statt aus dem Puffer hinaus zu lesen. */
    if (length * 8 < p->report_bits)
        return;

    /* HID-Taste 1, 2, 3 sind links, rechts, Mitte - dieselbe
     * Reihenfolge wie MOUSE_BTN_... */
    uint8_t buttons = 0;

    for (int i = 0; i < 3; i++) {
        if (p->button_bit[i] >= 0 &&
            hid_get_bits(report, length, (uint16_t)p->button_bit[i], 1, false))
            buttons |= (uint8_t)(1u << i);
    }

    int32_t wheel = 0;

    if (p->wheel.present)
        wheel = hid_get_bits(report, length, p->wheel.bit, p->wheel.size,
                             p->wheel.is_signed);
    wheel = CLAMP(wheel, -127, 127);

    int32_t x = hid_get_bits(report, length, p->x.bit, p->x.size, p->x.is_signed);
    int32_t y = hid_get_bits(report, length, p->y.bit, p->y.size, p->y.is_signed);

    bool l = (buttons & MOUSE_BTN_LEFT) != 0;
    bool r = (buttons & MOUSE_BTN_RIGHT) != 0;
    bool m = (buttons & MOUSE_BTN_MIDDLE) != 0;

    if (!p->x.relative && !p->y.relative) {
        /* Absolut: Der Wertebereich des Geraets wird auf 0..65535
         * umgerechnet, den Rest macht der Zeiger mit den Grenzen von
         * jetzt - die Oberflaeche kann inzwischen eine andere sein. */
        mouse_inject_unit(hid_scale(x, p->x.min, p->x.max, 65535),
                          hid_scale(y, p->y.min, p->y.max, 65535),
                          (int8_t)wheel, l, r, m);
    } else {
        mouse_inject(x, y, (int8_t)wheel, l, r, m);
    }
}

/* ------------------------------------------------------------------ */
/* Anmelden und Abfragen                                               */
/* ------------------------------------------------------------------ */

/* Liest die Berichtsbeschreibung eines Geraets und meldet es als Zeiger
 * im Berichtsmodus an, wenn es einer ist. true heisst: angemeldet.
 *
 * only_absolute: Bei einer Boot-Maus gilt das nur fuer ein absolutes
 * Geraet. Eine gewoehnliche Maus bleibt beim Boot-Protokoll, das jede
 * kennt und das sich bewaehrt hat; ein Tablett, das sich als Maus
 * ausgibt, wuerde dort nur Unsinn liefern. */
static bool attach_report_pointer(struct usb_device *dev,
                                  const struct usb_device_info *info,
                                  bool only_absolute)
{
    uint8_t description[1024];
    uint16_t length = info->hid_report_length;

    if (length == 0 || length > sizeof(description)) {
        if (!only_absolute)
            kprintf("USB         : HID-Geraet ohne lesbare Beschreibung\n");
        return false;
    }

    struct usb_setup setup = {
        .request_type = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (uint16_t)(USB_DESC_HID_REPORT << 8),
        .index = info->interface_number,
        .length = length,
    };

    if (!usb_control(dev, &setup, description)) {
        if (!only_absolute)
            kprintf("USB         : Berichtsbeschreibung nicht lesbar\n");
        return false;
    }

    struct hid_device *hid = &hids[hid_count];

    memset(hid, 0, sizeof(*hid));

    /* Kein Zeiger - eine Fernbedienung, ein Spielgeraet -: nicht unser
     * Geraet, aber auch kein Fehler. */
    if (!hid_parse_pointer(description, length, &hid->layout)) {
        if (!only_absolute)
            kprintf("USB         : HID-Geraet ist kein Zeiger\n");
        return false;
    }

    bool absolute = !hid->layout.x.relative && !hid->layout.y.relative;

    if (only_absolute && !absolute)
        return false;

    /* Nur bei Aenderungen melden, nicht in festem Takt. */
    setup = (struct usb_setup){
        .request_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        .request = USB_REQ_SET_IDLE,
        .value = 0,
        .index = info->interface_number,
        .length = 0,
    };
    usb_control(dev, &setup, NULL);

    hid->usb = dev;
    hid->pointer = true;
    hid_count++;

    kprintf("USB         : Zeiger angemeldet (%s, X %d..%d, Y %d..%d)\n",
            absolute ? "Tablett, absolut" : "Maus im Berichtsmodus",
            (int)hid->layout.x.min, (int)hid->layout.x.max,
            (int)hid->layout.y.min, (int)hid->layout.y.max);
    return true;
}

void usb_hid_attach(struct usb_device *dev)
{
    const struct usb_device_info *info = usb_device_details(dev);

    if (!info || info->interface_class != USB_CLASS_HID)
        return;
    if (hid_count >= MAX_HID)
        return;

    bool boot = info->interface_subclass == HID_SUBCLASS_BOOT;
    bool keyboard = boot && info->interface_protocol == HID_PROTOCOL_KEYBOARD;
    bool mouse = boot && info->interface_protocol == HID_PROTOCOL_MOUSE;

    if (!keyboard && !mouse) {
        /* Kein Boot-Geraet. Frueher endete es hier, und ein Tablett
         * blieb stumm - ausgerechnet das, mit dem eine virtuelle
         * Maschine ihren Zeiger ab Werk ausstattet. */
        attach_report_pointer(dev, info, false);
        return;
    }

    /* Eine Boot-Maus, die in Wahrheit ein Tablett ist, wuerde im
     * Boot-Protokoll Unsinn melden. Gefragt wird, bevor umgeschaltet
     * wird. */
    if (mouse && attach_report_pointer(dev, info, true))
        return;

    /* Auf das Boot-Protokoll umschalten - sonst schickt das Geraet sein
     * eigenes Format, das erst beschrieben werden muesste. */
    struct usb_setup setup = {
        .request_type = USB_TYPE_CLASS | USB_RECIP_INTERFACE,
        .request = USB_REQ_SET_PROTOCOL,
        .value = 0,                      /* 0 = Boot, 1 = eigenes */
        .index = info->interface_number,
        .length = 0,
    };

    usb_control(dev, &setup, NULL);

    /* Nur bei Aenderungen melden, nicht in festem Takt. */
    setup.request = USB_REQ_SET_IDLE;
    setup.value = 0;
    usb_control(dev, &setup, NULL);

    struct hid_device *hid = &hids[hid_count++];

    memset(hid, 0, sizeof(*hid));
    hid->usb = dev;
    hid->keyboard = keyboard;

    kprintf("USB         : %s angemeldet\n",
            keyboard ? "Tastatur" : "Maus");
}

void usb_hid_poll_all(void)
{
    uint8_t report[16];

    for (size_t i = 0; i < hid_count; i++) {
        struct hid_device *hid = &hids[i];
        size_t length = usb_interrupt_poll(hid->usb, report, sizeof(report));

        if (length == 0)
            continue;

        if (hid->pointer)
            handle_pointer(hid, report, length);
        else if (hid->keyboard && length >= 8)
            handle_keyboard(hid, report);
        else if (!hid->keyboard && length >= 3)
            handle_mouse(report, length);
    }
}

/* Ein absolutes Geraet im Berichtsmodus: ein Tablett. */
static bool is_tablet(const struct hid_device *hid)
{
    return hid->pointer && !hid->layout.x.relative && !hid->layout.y.relative;
}

size_t usb_hid_pointer_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < hid_count; i++) {
        if (hids[i].pointer || !hids[i].keyboard)
            n++;
    }
    return n;
}

bool usb_hid_has_tablet(void)
{
    for (size_t i = 0; i < hid_count; i++) {
        if (is_tablet(&hids[i]))
            return true;
    }
    return false;
}

bool usb_hid_is_tablet(const struct usb_device *dev)
{
    for (size_t i = 0; i < hid_count; i++) {
        if (hids[i].usb == dev && is_tablet(&hids[i]))
            return true;
    }
    return false;
}

/* Ein eigener Thread fragt die Geraete ab. Die Unterbrechung des
 * Controllers meldet zwar, dass etwas da ist, aber das Abholen aus dem
 * Ring geschieht hier - so bleibt der Unterbrechungsweg kurz. */
static void hid_thread(void *argument)
{
    UNUSED(argument);

    for (;;) {
        usb_hid_poll_all();
        thread_sleep(4);
    }
}

void usb_start_polling(void)
{
    if (hid_count == 0)
        return;
    thread_create("usb-hid", hid_thread, NULL, PRIO_HIGH);
}
