/* switch.S - Wechsel zwischen zwei Threads.
 *
 * Gesichert werden nur die Register, die der Aufrufer nach der C-Konvention
 * erwarten darf (rbx, rbp, r12-r15). Alles andere hat der rufende Code
 * ohnehin schon in Sicherheit gebracht - das macht den Wechsel sehr kurz.
 */

.section .text
.global context_switch
.global context_start

/* void context_switch(uint64_t *save_rsp, uint64_t load_rsp) */
context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    movq %rsp, (%rdi)        /* aktuellen Stapelzeiger sichern */
    movq %rsi, %rsp          /* auf den neuen Stapel wechseln  */

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret

/* Einsprung eines frisch erzeugten Threads: r12 haelt die Funktion,
 * r13 das Argument. Interrupts werden hier wieder freigegeben. */
context_start:
    movq %r13, %rdi
    sti
    call *%r12
    call thread_exit
    hlt

.section .note.GNU-stack,"",@progbits
