/* syscall.S - die Aufrufe selbst.
 *
 * Nummer in rax, Werte in rdi, rsi, rdx und r10 - genau die Reihenfolge,
 * die der Kernel erwartet. Der Befehl "syscall" ueberschreibt rcx und r11,
 * darum werden sie hier nicht benutzt.
 */

.section .text

/* Der Makroname darf nicht "syscall" heissen - sonst ruft sich das
 * Makro beim Befehl im Rumpf selbst auf. */
.macro DEFCALL name, number
.global \name
\name:
    movq $\number, %rax
    movq %rcx, %r10          /* viertes Argument umlegen */
    syscall
    ret
.endm

DEFCALL sys_exit,     0
DEFCALL sys_write,    1
DEFCALL sys_read,     2
DEFCALL sys_open,     3
DEFCALL sys_close,    4
DEFCALL sys_seek,     5
DEFCALL sys_sbrk,     6
DEFCALL sys_sleep,    7
DEFCALL sys_yield,    8
DEFCALL sys_getpid,   9
DEFCALL sys_uptime,  10
DEFCALL sys_args,    11
DEFCALL sys_filesize, 12
DEFCALL sys_readdir, 13

/* Fenster */
DEFCALL ui_open,     14
DEFCALL sys_ui_draw, 15
DEFCALL sys_ui_event, 16
DEFCALL ui_close,    17

/* Netz */
DEFCALL net_connect, 18
DEFCALL net_send,    19
DEFCALL sys_net_recv, 20
DEFCALL net_close,   21

.section .note.GNU-stack,"",@progbits
