/* start.S - der Einstiegspunkt jedes Programms.
 *
 * Der Kernel springt hierher; von hier geht es nach main(). Kehrt main()
 * zurueck, wird der Rueckgabewert als Ergebnis des Programms gemeldet.
 */

.section .text
.global _start
.extern main
.extern sys_exit

_start:
    xorq %rbp, %rbp           /* Ende der Aufrufkette markieren */
    call main
    movl %eax, %edi
    call sys_exit
    hlt

.section .note.GNU-stack,"",@progbits
