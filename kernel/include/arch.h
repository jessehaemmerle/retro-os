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

void gdt_init(void);
/* Legt fest, auf welchen Stapel die CPU wechselt, wenn ein Interrupt
 * aus einem Benutzerprogramm kommt. */
void tss_set_kernel_stack(uint64_t top);
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
