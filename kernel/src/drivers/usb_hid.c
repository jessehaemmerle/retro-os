/* usb_hid.c - Tastatur und Maus am USB-Bus.
 *
 * Beide sprechen das Boot-Protokoll: ein festes Paketformat, das jedes
 * BIOS versteht und das deshalb kein Auswerten der Berichtsbeschreibung
 * verlangt. Die Tastatur schickt sechs gleichzeitig gedrueckte Tasten
 * als Verwendungsnummern, die Maus Bewegung und Tastenzustand.
 *
 * Die Verwendungsnummern sind nicht die Abtastcodes des PS/2-Anschlusses;
 * sie brauchen eine eigene Tabelle. Angelegt ist sie fuer die deutsche
 * Belegung - dieselbe, die auch die PS/2-Tastatur benutzt.
 */

#include "usb.h"
#include "input.h"
#include "kstring.h"
#include "thread.h"

#define MAX_HID 8

struct hid_device {
    struct usb_device *usb;
    bool               keyboard;
    uint8_t            previous[8];    /* letztes Tastaturpaket */
};

static struct hid_device hids[MAX_HID];
static size_t            hid_count;

/* ------------------------------------------------------------------ */
/* Verwendungsnummern der Tastatur                                     */
/* ------------------------------------------------------------------ */

/* Die Tabellen laufen von Verwendung 0x04 bis 0x64. */
#define USAGE_FIRST 0x04
#define USAGE_LAST  0x64
#define USAGE_COUNT (USAGE_LAST - USAGE_FIRST + 1)

static const uint16_t base_map[USAGE_COUNT] = {
    /* 04 */ 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j',
    /* 0e */ 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
    /* 18 */ 'u', 'v', 'w', 'x', 'z', 'y',        /* deutsche Belegung */
    /* 1e */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    /* 28 */ KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_TAB, ' ',
    /* 2d */ 223 /* sz */, 180 /* Akut */, 252 /* ue */, '+',
    /* 31 */ '#', '#', 246 /* oe */, 228 /* ae */, '^',
    /* 36 */ ',', '.', '-',
    /* 39 */ KEY_CAPSLOCK,
    /* 3a */ KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    /* 40 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* 46 */ 0 /* Druck */, 0 /* Rollen */, 0 /* Pause */,
    /* 49 */ KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END,
    /* 4e */ KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    /* 53 */ 0 /* Num */, '/', '*', '-', '+', KEY_ENTER,
    /* 59 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', ',',
    /* 64 */ '<',
};

static const uint16_t shift_map[USAGE_COUNT] = {
    /* 04 */ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
    /* 0e */ 'K', 'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
    /* 18 */ 'U', 'V', 'W', 'X', 'Z', 'Y',
    /* 1e */ '!', '"', 167 /* Paragraf */, '$', '%', '&', '/', '(',
    /* 26 */ ')', '=',
    /* 28 */ KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_TAB, ' ',
    /* 2d */ '?', '`', 220 /* Ue */, '*',
    /* 31 */ '\'', '\'', 214 /* Oe */, 196 /* Ae */, 176 /* Grad */,
    /* 36 */ ';', ':', '_',
    /* 39 */ KEY_CAPSLOCK,
    /* 3a */ KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    /* 40 */ KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    /* 46 */ 0, 0, 0,
    /* 49 */ KEY_INSERT, KEY_HOME, KEY_PAGEUP, KEY_DELETE, KEY_END,
    /* 4e */ KEY_PAGEDOWN, KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
    /* 53 */ 0, '/', '*', '-', '+', KEY_ENTER,
    /* 59 */ '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', ',',
    /* 64 */ '>',
};

static const uint16_t altgr_map[USAGE_COUNT] = {
    /* 04 */ 0, 0, 0, 0, 8364 /* Euro */, 0, 0, 0, 0, 0,
    /* 0e */ 0, 0, 181 /* Mikro */, 0, 0, 0, '@', 0, 0, 0,
    /* 18 */ 0, 0, 0, 0, 0, 0,
    /* 1e */ 0, 178 /* hoch2 */, 179 /* hoch3 */, 0, 0, 0, '{', '[',
    /* 26 */ ']', '}',
    /* 28 */ 0, 0, 0, 0, 0,
    /* 2d */ '\\', 0, 0, '~',
    /* 31 */ 0, 0, 0, 0, 0,
    /* 36 */ 0, 0, 0,
    /* 39 */ 0,
    /* 3a */ 0, 0, 0, 0, 0, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0,
    /* 46 */ 0, 0, 0,
    /* 49 */ 0, 0, 0, 0, 0,
    /* 4e */ 0, 0, 0, 0, 0,
    /* 53 */ 0, 0, 0, 0, 0, 0,
    /* 59 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 64 */ '|',
};

/* Die acht Steuertasten stehen im ersten Byte des Pakets. */
static const uint16_t modifier_key[8] = {
    KEY_LCTRL, KEY_LSHIFT, KEY_LALT, KEY_SUPER,
    KEY_RCTRL, KEY_RSHIFT, KEY_RALT, KEY_SUPER,
};

static uint16_t translate(uint8_t usage, uint8_t modifiers)
{
    if (usage < USAGE_FIRST || usage > USAGE_LAST)
        return 0;

    size_t index = usage - USAGE_FIRST;
    bool shift = (modifiers & 0x22) != 0;       /* linke oder rechte */
    bool altgr = (modifiers & 0x40) != 0;       /* rechte Alt-Taste  */

    if (altgr && altgr_map[index])
        return altgr_map[index];
    if (shift && shift_map[index])
        return shift_map[index];
    return base_map[index];
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

/* ------------------------------------------------------------------ */
/* Anmelden und Abfragen                                               */
/* ------------------------------------------------------------------ */

void usb_hid_attach(struct usb_device *dev)
{
    const struct usb_device_info *info = usb_device_details(dev);

    if (!info || info->interface_class != USB_CLASS_HID)
        return;
    if (info->interface_subclass != HID_SUBCLASS_BOOT)
        return;
    if (hid_count >= MAX_HID)
        return;

    bool keyboard = info->interface_protocol == HID_PROTOCOL_KEYBOARD;
    bool mouse = info->interface_protocol == HID_PROTOCOL_MOUSE;

    if (!keyboard && !mouse)
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

        if (hid->keyboard && length >= 8)
            handle_keyboard(hid, report);
        else if (!hid->keyboard && length >= 3)
            handle_mouse(report, length);
    }
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
