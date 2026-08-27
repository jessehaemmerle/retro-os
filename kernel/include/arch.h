/* arch.h - CPU-nahe Einrichtung: Segmente, Interrupts, Timer. */
#ifndef ARCH_H
#define ARCH_H

#include "retro.h"

/* Registerzustand, wie ihn die Interrupt-Stubs auf dem Stack ablegen. */
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} PACKED;

typedef void (*irq_handler_t)(struct registers *regs);

#define SEGMENT_KERNEL_CODE 0x08
#define SEGMENT_KERNEL_DATA 0x10
#define SEGMENT_USER_DATA   0x18
#define SEGMENT_USER_CODE   0x20

#define IDT_ENTRIES         256
#define IRQ_BASE            32     /* Vektor der ersten Altgeraete-Leitung */
#define IRQ_COUNT           16
#define IRQ_VECTOR_DYNAMIC  48     /* ab hier vergibt der Kern selbst      */
#define IRQ_VECTOR_TOP      96     /* erster nicht mehr belegter Vektor    */
#define IRQ_VECTOR_SPURIOUS 255

void gdt_init(void);
/* Dasselbe fuer einen weiteren Kern - eigene Tabelle, eigenes TSS. */
void gdt_init_ap(void);
/* Legt fest, auf welchen Stapel die CPU wechselt, wenn ein Interrupt
 * aus einem Benutzerprogramm kommt. */
void tss_set_kernel_stack(uint64_t top);
void idt_init(void);
/* Laedt die (gemeinsame) Tabelle in das Register dieses Kerns. */
void idt_load(void);

/* IRQ 0-15; der Handler laeuft mit gesperrten Interrupts. */
void irq_install(uint8_t irq, irq_handler_t handler);

/* Fuer MSI: der Kern vergibt einen freien Vektor und merkt sich die
 * Behandlung dazu. Gibt -1 zurueck, wenn keiner mehr frei ist. */
int32_t irq_alloc_vector(irq_handler_t handler);
void    irq_free_vector(uint8_t vector);
void    irq_install_vector(uint8_t vector, irq_handler_t handler);

/* Verdeckt, ob dahinter der 8259 oder der APIC steckt. */
void irq_unmask(uint8_t irq);
void irq_mask(uint8_t irq);
void irq_send_eoi(uint8_t vector);

void pic_init(void);
void pic_disable(void);
void pic_mask(uint8_t irq, bool masked);
void pic_send_eoi(uint8_t irq);

void pit_init(uint32_t frequency_hz);

/* Waehlt den besten Zeitgeber: den des lokalen APIC, sonst den PIT. */
void timer_init(uint32_t frequency_hz);
/* Startet den Zeitgeber dieses Kerns - jeder braucht seinen eigenen. */
void timer_init_ap(void);
/* Wartet, ohne den Scheduler zu bemuehen. */
void timer_wait_ms(uint32_t ms);
bool timer_uses_apic(void);

/* Millisekunden seit Systemstart. */
uint64_t timer_ms(void);
uint64_t timer_ticks(void);
void     timer_sleep(uint32_t ms);

#endif /* ARCH_H */
