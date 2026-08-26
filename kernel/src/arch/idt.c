/* idt.c - Interrupt Descriptor Table und Verteilung der Interrupts. */

#include "arch.h"
#include "io.h"
#include "kstring.h"
#include "thread.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} PACKED;

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} PACKED;

#define IDT_ENTRIES 256
#define IRQ_BASE    32
#define IRQ_COUNT   16

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtr;
static irq_handler_t    irq_handlers[IRQ_COUNT];

/* Die Stubs aus isr.S. */
#define DECL(n) extern void isr##n(void)
DECL(0);  DECL(1);  DECL(2);  DECL(3);  DECL(4);  DECL(5);  DECL(6);  DECL(7);
DECL(8);  DECL(9);  DECL(10); DECL(11); DECL(12); DECL(13); DECL(14); DECL(15);
DECL(16); DECL(17); DECL(18); DECL(19); DECL(20); DECL(21); DECL(22); DECL(23);
DECL(24); DECL(25); DECL(26); DECL(27); DECL(28); DECL(29); DECL(30); DECL(31);
DECL(32); DECL(33); DECL(34); DECL(35); DECL(36); DECL(37); DECL(38); DECL(39);
DECL(40); DECL(41); DECL(42); DECL(43); DECL(44); DECL(45); DECL(46); DECL(47);
#undef DECL

static void *stubs[48] = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
    isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
};

static const char *exception_name[32] = {
    "Division durch Null", "Debug", "NMI", "Breakpoint",
    "Overflow", "Bound Range", "Ungueltiger Opcode", "Kein Koprozessor",
    "Double Fault", "Coprocessor Segment", "Ungueltiges TSS", "Segment fehlt",
    "Stack-Fehler", "Allgemeine Schutzverletzung", "Page Fault", "reserviert",
    "x87-Fehler", "Alignment Check", "Machine Check", "SIMD-Fehler",
    "Virtualisierung", "Control Protection", "reserviert", "reserviert",
    "reserviert", "reserviert", "reserviert", "Hypervisor Injection",
    "VMM Communication", "Security Exception", "reserviert", "reserviert",
};

static void set_gate(int vec, void *handler)
{
    uint64_t addr = (uint64_t)handler;

    idt[vec].offset_low  = addr & 0xFFFF;
    idt[vec].selector    = 0x08;
    idt[vec].ist         = 0;
    idt[vec].type_attr   = 0x8E;   /* present, Ring 0, Interrupt Gate */
    idt[vec].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vec].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vec].zero        = 0;
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    memset(irq_handlers, 0, sizeof(irq_handlers));

    for (int i = 0; i < 48; i++)
        set_gate(i, stubs[i]);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    __asm__ volatile("lidt %0" :: "m"(idtr));
}

bool scheduler_should_switch(void);

void irq_install(uint8_t irq, irq_handler_t handler)
{
    if (irq >= IRQ_COUNT)
        return;

    irq_handlers[irq] = handler;
    pic_mask(irq, false);
}

void isr_dispatch(struct registers *regs)
{
    if (regs->int_no >= IRQ_BASE && regs->int_no < IRQ_BASE + IRQ_COUNT) {
        uint8_t irq = (uint8_t)(regs->int_no - IRQ_BASE);

        if (irq_handlers[irq])
            irq_handlers[irq](regs);

        pic_send_eoi(irq);

        /* Erst quittieren, dann umschalten - sonst bleibt der Controller
         * haengen, waehrend ein anderer Thread laeuft. */
        if (scheduler_should_switch())
            schedule();
        return;
    }

    /* Alles andere ist eine CPU-Ausnahme und damit ein Programmfehler. */
    const char *name = regs->int_no < 32 ? exception_name[regs->int_no]
                                         : "unbekannt";
    uint64_t cr2 = 0;
    if (regs->int_no == 14)
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    panic("Ausnahme %u (%s)\n"
          "  Fehlercode : 0x%lx\n"
          "  RIP        : 0x%lx\n"
          "  RSP        : 0x%lx\n"
          "  CR2        : 0x%lx",
          (unsigned)regs->int_no, name, regs->err_code,
          regs->rip, regs->rsp, cr2);
}
