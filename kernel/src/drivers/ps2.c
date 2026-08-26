/* ps2.c - der 8042-Tastaturcontroller.
 *
 * Er bedient zwei Ports: Port 1 die Tastatur (IRQ 1), Port 2 die Maus
 * (IRQ 12). Die eigentlichen Treiber liegen in keyboard.c und mouse.c;
 * hier steht nur, was sich beide teilen.
 */

#include "input.h"
#include "io.h"
#include "ps2.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02

bool ps2_wait_write(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & STATUS_INPUT_FULL))
            return true;
    }
    return false;
}

bool ps2_wait_read(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
            return true;
    }
    return false;
}

void ps2_write_cmd(uint8_t cmd)
{
    ps2_wait_write();
    outb(PS2_CMD, cmd);
}

void ps2_write_data(uint8_t data)
{
    ps2_wait_write();
    outb(PS2_DATA, data);
}

uint8_t ps2_read_data(void)
{
    ps2_wait_read();
    return inb(PS2_DATA);
}

/* Ein Byte an das Geraet an Port 2 (Maus) schicken und Quittung lesen. */
uint8_t ps2_mouse_command(uint8_t byte)
{
    ps2_write_cmd(0xD4);
    ps2_write_data(byte);
    return ps2_read_data();
}

void ps2_init(void)
{
    /* Beide Ports voruebergehend abschalten und den Puffer leeren. */
    ps2_write_cmd(0xAD);
    ps2_write_cmd(0xA7);
    while (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
        (void)inb(PS2_DATA);

    /* Konfiguration lesen, Interrupts fuer beide Ports einschalten,
     * Scancode-Uebersetzung auf Satz 1 aktiviert lassen. */
    ps2_write_cmd(0x20);
    uint8_t config = ps2_read_data();

    config |= (1 << 0);   /* IRQ Port 1 (Tastatur) */
    config |= (1 << 1);   /* IRQ Port 2 (Maus)     */
    config |= (1 << 6);   /* Scancode-Uebersetzung */

    ps2_write_cmd(0x60);
    ps2_write_data(config);

    /* Ports wieder aktivieren. */
    ps2_write_cmd(0xAE);
    ps2_write_cmd(0xA8);
}
