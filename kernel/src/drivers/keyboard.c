/* keyboard.c - PS/2-Tastatur, Scancode-Satz 1.
 *
 * Der Treiber liest im Interrupt nur Scancodes ein, uebersetzt sie in
 * Tastenereignisse und legt sie in einen Ringpuffer. Die grafische
 * Oberflaeche holt sie dort in ihrer Hauptschleife wieder ab.
 */

#include "input.h"
#include "arch.h"
#include "io.h"
#include "kstring.h"

#define QUEUE_SIZE 64

/* Scancode-Satz 1, deutsche Tastaturbelegung (QWERTZ). */
static const uint16_t base_map[0x60] = {
    [0x01] = KEY_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = 0xDF /* ss */, [0x0D] = '\'',
    [0x0E] = KEY_BACKSPACE, [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'z', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = 0xFC /* ue */, [0x1B] = '+',
    [0x1C] = KEY_ENTER, [0x1D] = KEY_LCTRL,
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l',
    [0x27] = 0xF6 /* oe */, [0x28] = 0xE4 /* ae */, [0x29] = '^',
    [0x2A] = KEY_LSHIFT, [0x2B] = '#',
    [0x2C] = 'y', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '-',
    [0x36] = KEY_RSHIFT, [0x37] = '*', [0x38] = KEY_LALT, [0x39] = ' ',
    [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x47] = KEY_HOME, [0x48] = KEY_UP, [0x49] = KEY_PAGEUP,
    [0x4A] = '-', [0x4B] = KEY_LEFT, [0x4D] = KEY_RIGHT, [0x4E] = '+',
    [0x4F] = KEY_END, [0x50] = KEY_DOWN, [0x51] = KEY_PAGEDOWN,
    [0x52] = KEY_INSERT, [0x53] = KEY_DELETE,
    [0x56] = '<', [0x57] = KEY_F11, [0x58] = KEY_F12,
};

/* Zeichen, die sich mit gedrueckter Umschalttaste aendern. */
static const uint16_t shift_map[0x60] = {
    [0x02] = '!', [0x03] = '"', [0x04] = 0xA7 /* Paragraf */, [0x05] = '$',
    [0x06] = '%', [0x07] = '&', [0x08] = '/', [0x09] = '(', [0x0A] = ')',
    [0x0B] = '=', [0x0C] = '?', [0x0D] = '`',
    [0x1B] = '*', [0x29] = 0xB0 /* Grad */, [0x2B] = '\'',
    [0x33] = ';', [0x34] = ':', [0x35] = '_', [0x56] = '>',
};

/* AltGr-Ebene: die fuer Programmierer wichtigen Zeichen. */
static const uint16_t altgr_map[0x60] = {
    [0x03] = 0xB2, [0x04] = 0xB3,
    [0x08] = '{', [0x09] = '[', [0x0A] = ']', [0x0B] = '}', [0x0C] = '\\',
    [0x10] = '@', [0x11] = 0x80 /* Euro */,
    [0x1B] = '~', [0x2B] = '|', [0x38] = KEY_RALT, [0x56] = '|',
};

static struct key_event queue[QUEUE_SIZE];
static volatile uint32_t q_head, q_tail;
static uint8_t  mods;
static bool     expect_extended;

static void queue_push(struct key_event ev)
{
    uint32_t next = (q_head + 1) % QUEUE_SIZE;

    if (next == q_tail)
        return;   /* Puffer voll - aeltestes Ereignis bleibt erhalten */

    queue[q_head] = ev;
    q_head = next;
}

static void update_mods(uint16_t key, bool pressed)
{
    uint8_t bit = 0;

    switch (key) {
    case KEY_LSHIFT: case KEY_RSHIFT: bit = MOD_SHIFT; break;
    case KEY_LCTRL:  case KEY_RCTRL:  bit = MOD_CTRL;  break;
    case KEY_LALT:   case KEY_RALT:   bit = MOD_ALT;   break;
    case KEY_CAPSLOCK:
        if (pressed)
            mods ^= MOD_CAPS;
        return;
    default:
        return;
    }

    if (pressed)
        mods |= bit;
    else
        mods &= (uint8_t)~bit;
}

static void keyboard_irq(struct registers *regs)
{
    UNUSED(regs);

    uint8_t sc = inb(0x60);

    if (sc == 0xE0) {
        expect_extended = true;
        return;
    }

    bool pressed = !(sc & 0x80);
    uint8_t code = sc & 0x7F;
    bool extended = expect_extended;
    expect_extended = false;

    if (code >= 0x60)
        return;

    uint16_t key;

    if (extended) {
        /* Die erweiterten Codes teilen sich die Nummern mit dem Ziffernblock. */
        switch (code) {
        case 0x1C: key = KEY_ENTER;    break;
        case 0x1D: key = KEY_RCTRL;    break;
        case 0x38: key = KEY_RALT;     break;
        case 0x47: key = KEY_HOME;     break;
        case 0x48: key = KEY_UP;       break;
        case 0x49: key = KEY_PAGEUP;   break;
        case 0x4B: key = KEY_LEFT;     break;
        case 0x4D: key = KEY_RIGHT;    break;
        case 0x4F: key = KEY_END;      break;
        case 0x50: key = KEY_DOWN;     break;
        case 0x51: key = KEY_PAGEDOWN; break;
        case 0x52: key = KEY_INSERT;   break;
        case 0x53: key = KEY_DELETE;   break;
        case 0x5B: key = KEY_SUPER;    break;
        case 0x35: key = '/';          break;
        default:   return;
        }
    } else if ((mods & MOD_ALT) && altgr_map[code]) {
        key = altgr_map[code];
    } else if ((mods & MOD_SHIFT) && shift_map[code]) {
        key = shift_map[code];
    } else {
        key = base_map[code];
    }

    if (!key)
        return;

    keyboard_inject(key, pressed);
}

/* Erzeugt aus einer Taste das Ereignis und legt es in die Warteschlange.
 * Den Weg gehen beide Tastaturen - die am PS/2-Anschluss und die am
 * USB-Bus; ab hier unterscheidet der Rest des Systems sie nicht mehr. */
void keyboard_inject(uint16_t key, bool pressed)
{
    if (!key)
        return;

    update_mods(key, pressed);

    struct key_event ev = { .key = key, .ascii = 0, .mods = mods,
                            .pressed = pressed };

    if (key < 0x100) {
        char c = (char)key;

        if (c >= 'a' && c <= 'z') {
            bool upper = (mods & MOD_SHIFT) != 0;

            if (mods & MOD_CAPS)
                upper = !upper;
            if (upper)
                c = (char)(c - 32);
        }
        ev.ascii = c;
        ev.key   = (uint16_t)(uint8_t)c;
    }

    queue_push(ev);
}

void keyboard_init(void)
{
    q_head = q_tail = 0;
    mods = 0;

    /* Ohne 8042 gibt es nichts anzumelden - dann kommen die Tasten
     * ueber den USB-Bus herein. */
    if (ps2_present())
        irq_install(1, keyboard_irq);
}

bool keyboard_poll(struct key_event *out)
{
    if (q_tail == q_head)
        return false;

    *out = queue[q_tail];
    q_tail = (q_tail + 1) % QUEUE_SIZE;
    return true;
}

uint8_t keyboard_mods(void)
{
    return mods;
}
