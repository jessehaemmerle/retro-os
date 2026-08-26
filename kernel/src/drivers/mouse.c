/* mouse.c - PS/2-Maus an Port 2 des 8042.
 *
 * Die Maus sendet Pakete zu drei Byte (Tasten/Vorzeichen, dx, dy). Nach der
 * "Magic Sequence" mit den Abtastraten 200/100/80 meldet sie sich als Typ 3
 * und liefert ein viertes Byte fuer das Scrollrad.
 */

#include "input.h"
#include "arch.h"
#include "io.h"
#include "ps2.h"

static uint8_t  packet[4];
static uint8_t  packet_index;
static uint8_t  packet_size = 3;

static volatile int32_t pos_x, pos_y;
static volatile int32_t acc_dx, acc_dy;
static volatile int8_t  acc_scroll;
static volatile bool    btn_left, btn_right, btn_middle;
static volatile bool    dirty;

static int32_t limit_x = 1023, limit_y = 767;

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

static void mouse_irq(struct registers *regs)
{
    UNUSED(regs);

    /* Nur lesen, wenn das Byte wirklich von der Maus stammt (Bit 5). */
    if (!(inb(0x64) & 0x20))
        return;

    packet[packet_index] = inb(0x60);

    if (packet_index == 0 && !(packet[0] & 0x08))
        return;   /* nicht synchron - Byte verwerfen */

    if (++packet_index >= packet_size) {
        packet_index = 0;
        handle_packet();
    }
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
    pos_x   = screen_w / 2;
    pos_y   = screen_h / 2;

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
