/* mouse.c - PS/2-Maus an Port 2 des 8042.
 *
 * Die Maus sendet Pakete zu drei Byte (Tasten/Vorzeichen, dx, dy). Nach der
 * "Magic Sequence" mit den Abtastraten 200/100/80 meldet sie sich als Typ 3
 * und liefert ein viertes Byte fuer das Scrollrad.
 *
 * Zwei Dinge halten den Treiber auch dort am Leben, wo der Anschluss
 * nicht so sauber emuliert wird wie in QEMU.
 *
 * Erstens wird nicht blind losgeschickt: Antwortet auf den
 * Ruecksetzbefehl niemand, gibt es keine Maus, und der Treiber sagt das
 * und haelt still. Sonst wartet jeder folgende Befehl auf eine Antwort,
 * die nie kommt - und das beim Hochfahren, wo noch niemand zusieht.
 *
 * Zweitens ist der Interrupt nicht der einzige Weg herein. Bleibt IRQ 12
 * aus - eine Umlegung, die nicht stimmt, ein Emulator, der ihn nur
 * manchmal ausloest -, holt die Hauptschleife die Bytes selbst ab. Und
 * findet der Tastatur-Interrupt ein Byte, das der Maus gehoert, reicht
 * er es hierher weiter, statt es als Tastendruck zu verwerfen.
 */

#include "input.h"
#include "arch.h"
#include "io.h"
#include "ps2.h"
#include "spinlock.h"

static uint8_t  packet[4];
static uint8_t  packet_index;
static uint8_t  packet_size = 3;

static volatile int32_t pos_x, pos_y;
static volatile int32_t acc_dx, acc_dy;
static volatile int8_t  acc_scroll;
static volatile bool    btn_left, btn_right, btn_middle;
static volatile bool    dirty;

static int32_t limit_x = 1023, limit_y = 767;

/* Haengt eine Maus am Anschluss, und kommen ihre Bytes per Interrupt?
 * Beides wird gebraucht, um ehrlich sagen zu koennen, woran es liegt,
 * wenn sich der Zeiger nicht bewegt. */
static bool     attached;
static uint32_t irq_count;
static uint32_t polled_count;

/* Zwei Wege lesen am selben Anschluss - der Interrupt und die
 * Hauptschleife. Ohne Sperre koennten beide dasselbe Byte holen, und
 * das eine Paket waere dann um ein Byte verschoben. */
static struct spinlock mouse_lock = SPINLOCK_INIT("maus");

static void handle_packet(void)
{
    uint8_t flags = packet[0];

    /* Bit 3 ist bei gueltigen Paketen immer gesetzt. */
    if (!(flags & 0x08)) {
        packet_index = 0;
        return;
    }

    /* Ueberlauf-Bits bedeuten eine unbrauchbare Bewegung. */
    if (flags & 0xC0)
        return;

    int32_t dx = (int32_t)packet[1];
    int32_t dy = (int32_t)packet[2];

    if (flags & 0x10)
        dx -= 256;
    if (flags & 0x20)
        dy -= 256;

    /* Die Maus zaehlt nach oben positiv, der Bildschirm nach unten. */
    dy = -dy;

    pos_x = CLAMP(pos_x + dx, 0, limit_x);
    pos_y = CLAMP(pos_y + dy, 0, limit_y);

    acc_dx += dx;
    acc_dy += dy;

    if (packet_size == 4) {
        int8_t z = (int8_t)(packet[3] & 0x0F);
        if (z & 0x08)
            z |= (int8_t)0xF0;      /* Vorzeichen ergaenzen */
        acc_scroll = (int8_t)(acc_scroll + z);
    }

    btn_left   = (flags & 0x01) != 0;
    btn_right  = (flags & 0x02) != 0;
    btn_middle = (flags & 0x04) != 0;

    dirty = true;
}

/* Bewegung und Tasten von aussen uebernehmen - der Weg der USB-Maus.
 * Die Zaehlweise ist dieselbe wie beim PS/2-Paket: nach rechts und nach
 * unten positiv. */
void mouse_inject(int32_t dx, int32_t dy, int8_t scroll,
                  bool left, bool right, bool middle)
{
    pos_x = CLAMP(pos_x + dx, 0, limit_x);
    pos_y = CLAMP(pos_y + dy, 0, limit_y);

    acc_dx += dx;
    acc_dy += dy;
    acc_scroll = (int8_t)(acc_scroll + scroll);

    btn_left = left;
    btn_right = right;
    btn_middle = middle;

    dirty = true;
}

/* Eine absolute Zeigerposition, wie sie ein Grafiktablett meldet -
 * damit die Maus im Fenster des Emulators sofort passt. */
void mouse_inject_absolute(int32_t x, int32_t y, int8_t scroll,
                           bool left, bool right, bool middle)
{
    int32_t nx = CLAMP(x, 0, limit_x);
    int32_t ny = CLAMP(y, 0, limit_y);

    acc_dx += nx - pos_x;
    acc_dy += ny - pos_y;
    pos_x = nx;
    pos_y = ny;

    acc_scroll = (int8_t)(acc_scroll + scroll);
    btn_left = left;
    btn_right = right;
    btn_middle = middle;
    dirty = true;
}

/* Ein Byte in die Zustandsmaschine geben. Von hier aus gibt es drei
 * Wege herein: der eigene Interrupt, der der Tastatur und die
 * Hauptschleife. */
void mouse_feed_byte(uint8_t byte)
{
    packet[packet_index] = byte;

    if (packet_index == 0 && !(packet[0] & 0x08))
        return;   /* nicht synchron - Byte verwerfen */

    if (++packet_index >= packet_size) {
        packet_index = 0;
        handle_packet();
    }
}

/* Holt ein Byte, das der Maus gehoert. false heisst: da liegt keines -
 * entweder ist der Puffer leer, oder das Byte gehoert der Tastatur.
 * Nur unter der Sperre aufrufen. */
static bool take_byte(uint8_t *out)
{
    uint8_t status = inb(0x64);

    if (status == 0xFF || !(status & 0x01) || !(status & 0x20))
        return false;
    *out = inb(0x60);
    return true;
}

static void mouse_irq(struct registers *regs)
{
    UNUSED(regs);

    uint64_t flags = spin_lock_irq(&mouse_lock);
    uint8_t  byte;

    if (take_byte(&byte)) {
        irq_count++;
        mouse_feed_byte(byte);
    }
    spin_unlock_irq(&mouse_lock, flags);
}

/* Nachsehen, ob ein Mausbyte im Controller liegt, das kein Interrupt
 * abgeholt hat. Kostet ein inb je Bilddurchlauf und rettet den Zeiger
 * ueberall dort, wo IRQ 12 nicht ankommt.
 *
 * Unterbrechungen bleiben dabei aus: Sonst koennte der eigene Interrupt
 * mitten hinein platzen und dasselbe Byte ein zweites Mal lesen. */
void mouse_pump(void)
{
    if (!attached)
        return;

    /* Die Schleife hat eine Obergrenze: Ein Anschluss, der pausenlos
     * Bytes nachliefert, soll die Oberflaeche nicht anhalten. */
    for (int i = 0; i < 64; i++) {
        uint64_t flags = spin_lock_irq(&mouse_lock);
        uint8_t  byte;
        bool     got = take_byte(&byte);

        if (got) {
            polled_count++;
            mouse_feed_byte(byte);
        }
        spin_unlock_irq(&mouse_lock, flags);

        if (!got)
            return;
    }
}

bool mouse_attached(void) { return attached; }

/* Ein Zusatz fuer die Systeminformation, und zwar nur dann, wenn es
 * etwas zu sagen gibt: Der Normalfall braucht keine Anmerkung. */
const char *mouse_note(void)
{
    if (attached && irq_count == 0 && polled_count > 0)
        return " (abgefragt)";
    return "";
}

static void set_sample_rate(uint8_t rate)
{
    ps2_mouse_command(0xF3);
    ps2_mouse_command(rate);
}

void mouse_init(int32_t screen_w, int32_t screen_h)
{
    limit_x = screen_w - 1;
    limit_y = screen_h - 1;
    pos_x = screen_w / 2;
    pos_y = screen_h / 2;

    attached = false;
    irq_count = 0;
    polled_count = 0;

    /* Die Maus am PS/2-Anschluss gibt es nur, wenn der Controller da
     * ist. Sonst bleibt es bei der USB-Maus. */
    if (!ps2_present() || !ps2_port2_present()) {
        kprintf("Maus        : keine am PS/2-Anschluss\n");
        return;
    }

    /* Zuruecksetzen und nachsehen, ob ueberhaupt jemand antwortet.
     * Eine Maus quittiert mit 0xFA, meldet danach 0xAA (Selbsttest
     * bestanden) und ihre Kennung. Kommt schon die Quittung nicht, ist
     * der Anschluss leer - dann wird hier nicht weitergemacht. */
    if (!ps2_mouse_command_ok(0xFF, NULL)) {
        kprintf("Maus        : keine Antwort am PS/2-Anschluss\n");
        kprintf("Maus        : in VirtualBox das Zeigergeraet auf "
                "\"PS/2-Maus\" stellen\n");
        return;
    }

    uint8_t selftest = 0;
    uint8_t ignored = 0;

    if (!ps2_read_byte(&selftest) || selftest != 0xAA) {
        kprintf("Maus        : Selbsttest meldet 0x%02x\n", selftest);
        return;
    }
    ps2_read_byte(&ignored);      /* Kennung, nach dem Ruecksetzen 0x00   */

    attached = true;

    ps2_mouse_command(0xF6);      /* Standardwerte                        */

    /* Scrollrad freischalten und Typ abfragen. */
    set_sample_rate(200);
    set_sample_rate(100);
    set_sample_rate(80);
    ps2_mouse_command(0xF2);
    uint8_t id = ps2_read_data();
    packet_size = (id == 3 || id == 4) ? 4 : 3;

    set_sample_rate(100);         /* 100 Pakete je Sekunde                */
    ps2_mouse_command(0xE8);      /* Aufloesung ...                        */
    ps2_mouse_command(0x03);      /* ... 8 Zaehler je Millimeter          */
    ps2_mouse_command(0xF4);      /* Datenuebertragung starten            */

    packet_index = 0;
    irq_install(12, mouse_irq);

    kprintf("Maus        : Typ %u, %u-Byte-Pakete\n", id, packet_size);
}

bool mouse_poll(struct mouse_state *out)
{
    bool changed = dirty;

    out->x      = pos_x;
    out->y      = pos_y;
    out->dx     = acc_dx;
    out->dy     = acc_dy;
    out->scroll = acc_scroll;
    out->left   = btn_left;
    out->right  = btn_right;
    out->middle = btn_middle;

    acc_dx = acc_dy = 0;
    acc_scroll = 0;
    dirty = false;

    return changed;
}

void mouse_set_position(int32_t x, int32_t y)
{
    pos_x = CLAMP(x, 0, limit_x);
    pos_y = CLAMP(y, 0, limit_y);
    dirty = true;
}
