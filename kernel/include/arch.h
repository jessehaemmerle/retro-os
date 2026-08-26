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

void gdt_init(void);
void idt_init(void);

/* IRQ 0-15; der Handler laeuft mit gesperrten Interrupts. */
void irq_install(uint8_t irq, irq_handler_t handler);

void pic_init(void);
void pic_mask(uint8_t irq, bool masked);
void pic_send_eoi(uint8_t irq);

void pit_init(uint32_t frequency_hz);

/* Millisekunden seit Systemstart. */
uint64_t timer_ms(void);
uint64_t timer_ticks(void);
void     timer_sleep(uint32_t ms);

#endif /* ARCH_H */
