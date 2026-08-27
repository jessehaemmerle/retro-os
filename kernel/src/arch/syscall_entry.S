/* syscall_entry.S - Einsprung aus dem Benutzerprogramm in den Kernel.
 *
 * Der Befehl "syscall" wechselt nur den Ring und legt die Ruecksprung-
 * adresse in rcx und die Flags in r11 ab - um den Stapel muss sich der
 * Kernel selbst kuemmern. Waehrend der ersten beiden Befehle sind die
 * Interrupts gesperrt (dafuer sorgt das FMASK-Register).
 *
 * Mit mehreren Kernen darf die Ablage fuer den Benutzerstapel keine
 * einzelne Variable mehr sein: Zwei Kerne koennten gleichzeitig
 * hereinkommen. Deshalb zeigt GS auf einen Bereich je Kern - swapgs
 * holt ihn hervor und legt ihn beim Ruecksprung wieder weg.
 *
 *     %gs:0   Kernelstapel dieses Kerns
 *     %gs:8   Ablage fuer den Stapel des Benutzers
 */

.section .text
.global syscall_entry
.extern syscall_dispatch

syscall_entry:
    swapgs
    movq %rsp, %gs:8
    movq %gs:0, %rsp

    pushq %gs:8                       /* Stapel des Benutzers          */
    pushq %rax
    pushq %rbx
    pushq %rcx                        /* Ruecksprungadresse            */
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11                        /* Flags des Benutzers           */
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15

    cld
    movq %rsp, %rdi
    sti                               /* Systemaufrufe duerfen warten  */
    call syscall_dispatch
    cli

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
    popq %rsp

    swapgs
    sysretq

.section .note.GNU-stack,"",@progbits
