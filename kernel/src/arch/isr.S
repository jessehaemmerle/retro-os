/* isr.S - Einsprungpunkte fuer Ausnahmen und Hardware-Interrupts.
 *
 * Jeder Stub legt eine einheitliche "struct registers" auf dem Stack ab und
 * ruft isr_dispatch() auf. Ausnahmen ohne Fehlercode schieben dafuer eine
 * Null nach, damit das Stack-Layout immer gleich aussieht.
 */

.section .text
.altmacro
.extern isr_dispatch

.macro ISR_NOERR num
.global isr\num
isr\num:
    pushq $0
    pushq $\num
    jmp isr_common
.endm

.macro ISR_ERR num
.global isr\num
isr\num:
    pushq $\num
    jmp isr_common
.endm

/* CPU-Ausnahmen 0-31 */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* Hardware-IRQs 0-15 landen auf Vektor 32-47 */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

/* Vektoren 48-95 vergibt der Kern selbst - fuer MSI und MSI-X der
 * PCIe-Geraete sowie den Zeitgeber des lokalen APIC. */
.irp num, 48, 49, 50, 51, 52, 53, 54, 55
ISR_NOERR %\num
.endr
.irp num, 56, 57, 58, 59, 60, 61, 62, 63
ISR_NOERR %\num
.endr
.irp num, 64, 65, 66, 67, 68, 69, 70, 71
ISR_NOERR %\num
.endr
.irp num, 72, 73, 74, 75, 76, 77, 78, 79
ISR_NOERR %\num
.endr
.irp num, 80, 81, 82, 83, 84, 85, 86, 87
ISR_NOERR %\num
.endr
.irp num, 88, 89, 90, 91, 92, 93, 94, 95
ISR_NOERR %\num
.endr

/* Vektor 255 faengt die Fehlmeldungen des lokalen APIC ab. */
ISR_NOERR 255

isr_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi          /* Zeiger auf struct registers */
    call isr_dispatch

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax

    addq $16, %rsp           /* int_no und err_code verwerfen */
    iretq

.section .note.GNU-stack,"",@progbits
