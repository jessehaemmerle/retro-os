/* usermode.S - der erste Sprung nach Ring 3.
 *
 * Zurueck aus einem Interrupt geht es mit iretq - also wird ein passender
 * Rahmen von Hand aufgebaut: Stapelsegment, Stapelzeiger, Flags,
 * Codesegment und Einsprungadresse.
 */

.section .text
.global enter_user_mode

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

.section .note.GNU-stack,"",@progbits
