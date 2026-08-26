/* pic.c - der klassische 8259A-Interruptcontroller.
 *
 * Ein moderner Rechner haette einen APIC, aber der PIC genuegt fuer
 * Tastatur, Maus und Timer voellig und funktioniert auf jeder x86-Maschine.
 */

#include "arch.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x11   /* Initialisierung mit ICW4 */
#define ICW4_8086 0x01

void pic_init(void)
{
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT);  io_wait();
    outb(PIC2_CMD, ICW1_INIT);  io_wait();
    outb(PIC1_DATA, 32);        io_wait();  /* Master auf Vektor 32 */
    outb(PIC2_DATA, 40);        io_wait();  /* Slave  auf Vektor 40 */
    outb(PIC1_DATA, 0x04);      io_wait();  /* Slave haengt an IRQ2 */
    outb(PIC2_DATA, 0x02);      io_wait();
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    UNUSED(mask1);
    UNUSED(mask2);

    /* Zunaechst alles sperren; Treiber geben ihren IRQ selbst frei. */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_mask(uint8_t irq, bool masked)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t  bit  = (uint8_t)(1 << (irq & 7));
    uint8_t  val  = inb(port);

    if (masked)
        val |= bit;
    else
        val &= (uint8_t)~bit;

    outb(port, val);

    /* Der Slave erreicht die CPU nur ueber IRQ2 des Masters. */
    if (irq >= 8 && !masked)
        pic_mask(2, false);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}
