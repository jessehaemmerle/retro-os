/* keyboard.c - PS/2-Tastatur, Scancode-Satz 1.
 *
 * Der Treiber liest im Interrupt nur Scancodes ein, uebersetzt sie in
 * Tastenereignisse und legt sie in einen Ringpuffer. Die grafische
 * Oberflaeche holt sie dort in ihrer Hauptschleife wieder ab.
 *
 * Welches Zeichen zu einem Scancode gehoert, steht in keymap.c - die
 * Belegung laesst sich im Betrieb umschalten.
 */

#include "input.h"
#include "arch.h"
#include "io.h"
#include "keymap.h"
#include "kstring.h"
#include "ps2.h"

#define QUEUE_SIZE 64

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

    /* Beide Geraete haengen am selben Datenport. Sagt der Controller,
     * dass das wartende Byte von der Maus kommt (Bit 5), gehoert es
     * nicht hierher - es wird weitergereicht statt als Tastendruck
     * verworfen. Das rettet die Maus dort, wo IRQ 12 nicht ankommt,
     * ihre Bytes aber sehr wohl. */
    uint8_t status = inb(0x64);

    if (status == 0xFF || !(status & 0x01))
        return;

    if (status & 0x20) {
        mouse_drain();
        return;
    }

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
    } else {
        const struct keymap *map = keymap_current();

        if ((mods & MOD_ALT) && map->ps2_altgr[code])
            key = map->ps2_altgr[code];
        else if ((mods & MOD_SHIFT) && map->ps2_shift[code])
            key = map->ps2_shift[code];
        else
            key = map->ps2_base[code];
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
