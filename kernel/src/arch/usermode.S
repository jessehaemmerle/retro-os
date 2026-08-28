/* usermode.S - der erste Sprung nach Ring 3.
 *
 * Zurueck aus einem Interrupt geht es mit iretq - also wird ein passender
 * Rahmen von Hand aufgebaut: Stapelsegment, Stapelzeiger, Flags,
 * Codesegment und Einsprungadresse.
 */

.section .text
.global enter_user_mode
.global fork_return

/* void enter_user_mode(uint64_t entry, uint64_t stack) */
enter_user_mode:
    cli
    movq $0x1B, %rax          /* 0x18 | 3 - Datensegment Ring 3 */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs

    pushq $0x1B               /* SS     */
    pushq %rsi                /* RSP    */
    pushq $0x202              /* RFLAGS mit gesetztem Interrupt-Flag */
    pushq $0x23               /* CS = 0x20 | 3 */
    pushq %rdi                /* RIP    */
    iretq

/* void fork_return(const struct syscall_frame *frame) NORETURN
 *
 * Der Rueckweg eines frisch abgespaltenen Kindes. Es hat nie ein
 * "syscall" ausgefuehrt, also gibt es auch keinen Rahmen auf seinem
 * Kernelstapel - er wird hier hingelegt und dann genauso abgeraeumt
 * wie beim gewoehnlichen Ruecksprung aus einem Systemaufruf.
 *
 * Der Rahmen liegt auf dem Kernelstapel des Kindes; ihn zum
 * Stapelzeiger zu machen kostet nichts, weil der Stapel gleich darauf
 * ohnehin gewechselt wird.
 *
 * %gs bleibt unangetastet: Die beiden Basisregister zeigen nach
 * syscall_bind_cpu auf denselben Bereich, das swapgs am Ende ist also
 * folgenlos - und beim naechsten Systemaufruf stimmt es wieder. */
fork_return:
    cli
    movq $0x1B, %rax          /* Datensegmente Ring 3 */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs

    movq %rdi, %rsp

    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11                 /* Flags des Benutzers            */
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx                 /* Ruecksprungadresse             */
    popq %rbx
    popq %rax                 /* Ergebnis: 0 beim Kind          */
    popq %rsp                 /* Stapel des Benutzers           */

    swapgs
    sysretq

.section .note.GNU-stack,"",@progbits
