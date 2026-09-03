/* ps2.c - der 8042-Tastaturcontroller.
 *
 * Er bedient zwei Ports: Port 1 die Tastatur (IRQ 1), Port 2 die Maus
 * (IRQ 12). Die eigentlichen Treiber liegen in keyboard.c und mouse.c;
 * hier steht nur, was sich beide teilen.
 */

#include "input.h"
#include "acpi.h"
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

/* Wie ps2_read_data, sagt aber, ob ueberhaupt etwas kam. Das ist der
 * Unterschied zwischen "das Geraet hat 0x00 geantwortet" und "es ist
 * gar keines da" - und genau den braucht man, um festzustellen, ob am
 * Mausanschluss jemand haengt. */
bool ps2_read_byte(uint8_t *out)
{
    if (!ps2_wait_read())
        return false;
    *out = inb(PS2_DATA);
    return true;
}

/* Ein Byte an das Geraet an Port 2 (Maus) schicken und Quittung lesen. */
uint8_t ps2_mouse_command(uint8_t byte)
{
    uint8_t answer = 0;

    ps2_mouse_command_ok(byte, &answer);
    return answer;
}

/* Dasselbe mit Auskunft: true heisst, das Geraet hat mit 0xFA
 * quittiert. Ein 0xFE ("noch einmal") wird zweimal wiederholt - ein
 * Bus, auf dem gerade ein Paket unterwegs war, antwortet das gerne
 * einmal, und beim dritten Mal liegt es nicht mehr am Zeitpunkt. */
bool ps2_mouse_command_ok(uint8_t byte, uint8_t *answer)
{
    for (int versuch = 0; versuch < 3; versuch++) {
        uint8_t got;

        ps2_write_cmd(0xD4);
        ps2_write_data(byte);

        if (!ps2_read_byte(&got))
            return false;
        if (got == 0xFE)
            continue;
        if (answer)
            *answer = got;
        return got == 0xFA;
    }
    return false;
}

static bool present;
static bool port2;

bool ps2_present(void) { return present; }
bool ps2_port2_present(void) { return port2; }

/* Leert den Ausgabepuffer - mit Begrenzung. Fehlt der Baustein, liest
 * man an Port 0x64 lauter Einsen und wuerde sonst ewig kreisen. */
static void drain_output(void)
{
    for (int i = 0; i < 32; i++) {
        uint8_t status = inb(PS2_STATUS);

        if (status == 0xFF || !(status & STATUS_OUTPUT_FULL))
            return;
        (void)inb(PS2_DATA);
    }
}

bool ps2_init(void)
{
    present = false;

    /* Moderne Rechner sagen in den ACPI-Tabellen, dass sie keinen
     * 8042 mehr haben. Dann wird gar nicht erst angeklopft. */
    if (!acpi_has_ps2()) {
        kprintf("PS/2        : laut ACPI nicht vorhanden\n");
        return false;
    }

    /* Ein fehlender Baustein antwortet mit lauter Einsen. */
    if (inb(PS2_STATUS) == 0xFF) {
        kprintf("PS/2        : kein Controller gefunden\n");
        return false;
    }

    /* Beide Ports voruebergehend abschalten und den Puffer leeren. */
    ps2_write_cmd(0xAD);
    ps2_write_cmd(0xA7);
    drain_output();

    /* Selbsttest: der Controller muss mit 0x55 antworten. */
    ps2_write_cmd(0xAA);
    if (!ps2_wait_read() || inb(PS2_DATA) != 0x55) {
        kprintf("PS/2        : Selbsttest fehlgeschlagen\n");
        return false;
    }
    drain_output();

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

    /* Der Controller ist da - aber haengt an Port 2 auch etwas? Das ist
     * keine Spitzfindigkeit: In VirtualBox etwa ist das Zeigergeraet ab
     * Werk ein USB-Tablett, und dann ist der Mausanschluss schlicht
     * leer. Wer das nicht prueft, schickt Befehle ins Nichts und wartet
     * bei jedem einzelnen auf eine Antwort, die nie kommt.
     *
     * Der Selbsttest des Ports allein reicht dafuer nicht: Er meldet
     * auch dann 0x00, wenn niemand angeschlossen ist. Ob wirklich eine
     * Maus da ist, sagt erst ihre Antwort auf den Ruecksetzbefehl -
     * das macht mouse_init(). */
    ps2_write_cmd(0xA9);

    uint8_t result = 0xFF;

    port2 = ps2_read_byte(&result) && result == 0x00;
    if (!port2)
        kprintf("PS/2        : Port 2 meldet 0x%02x - keine Maus\n", result);

    present = true;
    return true;
}
