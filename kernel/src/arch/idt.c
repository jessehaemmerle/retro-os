/* idt.c - Interrupt Descriptor Table und Verteilung der Interrupts. */

#include "arch.h"
#include "io.h"
#include "kstring.h"
#include "thread.h"
#include "process.h"

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

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtr;

/* Die Behandlung haengt am Vektor, nicht mehr an der IRQ-Nummer: MSI
 * kennt keine IRQ-Leitungen, sondern schreibt gleich einen Vektor. */
static irq_handler_t    handlers[IDT_ENTRIES];
static bool             vector_taken[IDT_ENTRIES];

/* Die Stubs aus isr.S. */
#define DECL(n) extern void isr##n(void)
DECL(0);  DECL(1);  DECL(2);  DECL(3);  DECL(4);  DECL(5);  DECL(6);  DECL(7);
DECL(8);  DECL(9);  DECL(10); DECL(11); DECL(12); DECL(13); DECL(14); DECL(15);
DECL(16); DECL(17); DECL(18); DECL(19); DECL(20); DECL(21); DECL(22); DECL(23);
DECL(24); DECL(25); DECL(26); DECL(27); DECL(28); DECL(29); DECL(30); DECL(31);
DECL(32); DECL(33); DECL(34); DECL(35); DECL(36); DECL(37); DECL(38); DECL(39);
DECL(40); DECL(41); DECL(42); DECL(43); DECL(44); DECL(45); DECL(46); DECL(47);
DECL(48); DECL(49); DECL(50); DECL(51); DECL(52); DECL(53); DECL(54); DECL(55);
DECL(56); DECL(57); DECL(58); DECL(59); DECL(60); DECL(61); DECL(62); DECL(63);
DECL(64); DECL(65); DECL(66); DECL(67); DECL(68); DECL(69); DECL(70); DECL(71);
DECL(72); DECL(73); DECL(74); DECL(75); DECL(76); DECL(77); DECL(78); DECL(79);
DECL(80); DECL(81); DECL(82); DECL(83); DECL(84); DECL(85); DECL(86); DECL(87);
DECL(88); DECL(89); DECL(90); DECL(91); DECL(92); DECL(93); DECL(94); DECL(95);
DECL(255);
#undef DECL

static void *stubs[IRQ_VECTOR_TOP] = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
    isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
    isr48, isr49, isr50, isr51, isr52, isr53, isr54, isr55,
    isr56, isr57, isr58, isr59, isr60, isr61, isr62, isr63,
    isr64, isr65, isr66, isr67, isr68, isr69, isr70, isr71,
    isr72, isr73, isr74, isr75, isr76, isr77, isr78, isr79,
    isr80, isr81, isr82, isr83, isr84, isr85, isr86, isr87,
    isr88, isr89, isr90, isr91, isr92, isr93, isr94, isr95,
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
    memset(handlers, 0, sizeof(handlers));
    memset(vector_taken, 0, sizeof(vector_taken));

    for (int i = 0; i < IRQ_VECTOR_TOP; i++)
        set_gate(i, stubs[i]);
    set_gate(IRQ_VECTOR_SPURIOUS, isr255);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)&idt;

    __asm__ volatile("lidt %0" :: "m"(idtr));
}

/* Die Tabelle selbst ist fuer alle Kerne dieselbe - jeder muss sie nur
 * in sein eigenes Register laden. */
void idt_load(void)
{
    __asm__ volatile("lidt %0" :: "m"(idtr));
}

bool scheduler_should_switch(void);

void irq_install(uint8_t irq, irq_handler_t handler)
{
    if (irq >= IRQ_COUNT)
        return;

    handlers[IRQ_BASE + irq] = handler;
    vector_taken[IRQ_BASE + irq] = true;
    irq_unmask(irq);
}

void irq_install_vector(uint8_t vector, irq_handler_t handler)
{
    if (vector < IRQ_BASE || vector >= IRQ_VECTOR_TOP)
        return;
    handlers[vector] = handler;
    vector_taken[vector] = true;
}

int32_t irq_alloc_vector(irq_handler_t handler)
{
    for (int32_t v = IRQ_VECTOR_DYNAMIC; v < IRQ_VECTOR_TOP; v++) {
        if (vector_taken[v])
            continue;
        vector_taken[v] = true;
        handlers[v] = handler;
        return v;
    }
    return -1;
}

void irq_free_vector(uint8_t vector)
{
    if (vector < IRQ_VECTOR_DYNAMIC || vector >= IRQ_VECTOR_TOP)
        return;
    vector_taken[vector] = false;
    handlers[vector] = NULL;
}

void isr_dispatch(struct registers *regs)
{
    /* Eine Fehlmeldung des lokalen APIC braucht keine Quittung. */
    if (regs->int_no == IRQ_VECTOR_SPURIOUS)
        return;

    if (regs->int_no >= IRQ_BASE && regs->int_no < IRQ_VECTOR_TOP) {
        uint32_t vector = (uint32_t)regs->int_no;

        if (handlers[vector])
            handlers[vector](regs);

        irq_send_eoi((uint8_t)vector);

        /* Erst quittieren, dann umschalten - sonst bleibt der Controller
         * haengen, waehrend ein anderer Thread laeuft. */
        if (scheduler_should_switch())
            schedule();
        return;
    }

    /* Alles andere ist eine CPU-Ausnahme. */
    const char *name = regs->int_no < 32 ? exception_name[regs->int_no]
                                         : "unbekannt";
    uint64_t cr2 = 0;
    if (regs->int_no == 14)
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    /* Kam sie aus einem Benutzerprogramm, stirbt nur dieses - genau
     * dafuer gibt es die Trennung in Ringe. */
    if ((regs->cs & 3) == 3) {
        struct process *proc = process_current();

        kprintf("Programm \"%s\" abgebrochen: %s (RIP 0x%lx, CR2 0x%lx)\n",
                proc ? proc->name : "?", name, regs->rip, cr2);

        if (proc) {
            char message[128];

            ksnprintf(message, sizeof(message),
                      "\n[abgebrochen: %s bei 0x%lx]\n", name, regs->rip);
            process_append_output(proc, message, strlen(message));
            process_exit(proc, -1);
        }
        thread_exit();
    }

    panic("Ausnahme %u (%s)\n"
          "  Fehlercode : 0x%lx\n"
          "  RIP        : 0x%lx\n"
          "  RSP        : 0x%lx\n"
          "  CR2        : 0x%lx",
          (unsigned)regs->int_no, name, regs->err_code,
          regs->rip, regs->rsp, cr2);
}
