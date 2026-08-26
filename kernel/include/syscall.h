/* syscall.h - die Schnittstelle zwischen Benutzerprogramm und Kernel.
 *
 * Aufgerufen wird mit dem Befehl "syscall": die Nummer steht in rax, die
 * Werte in rdi, rsi, rdx, r10, r8 und r9. Das Ergebnis kommt in rax
 * zurueck; negative Werte sind Fehler.
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include "retro.h"

enum {
    SYS_EXIT      = 0,   /* (code)                          */
    SYS_WRITE     = 1,   /* (fd, puffer, laenge) -> Anzahl  */
    SYS_READ      = 2,   /* (fd, puffer, laenge) -> Anzahl  */
    SYS_OPEN      = 3,   /* (pfad, modus) -> fd             */
    SYS_CLOSE     = 4,   /* (fd)                            */
    SYS_SEEK      = 5,   /* (fd, position) -> Position      */
    SYS_SBRK      = 6,   /* (zuwachs) -> alte Obergrenze    */
    SYS_SLEEP     = 7,   /* (millisekunden)                 */
    SYS_YIELD     = 8,   /* ()                              */
    SYS_GETPID    = 9,   /* () -> Nummer                    */
    SYS_UPTIME    = 10,  /* () -> Millisekunden seit Start  */
    SYS_ARGS      = 11,  /* (puffer, laenge) -> Anzahl      */
    SYS_FILESIZE  = 12,  /* (fd) -> Groesse                 */
    SYS_READDIR   = 13,  /* (pfad, index, puffer, laenge)   */
    SYS_COUNT
};

/* Fehlerwerte (als negative Rueckgabe). */
#define SYS_ERR_BADFD    (-1)
#define SYS_ERR_NOENT    (-2)
#define SYS_ERR_INVAL    (-3)
#define SYS_ERR_NOMEM    (-4)
#define SYS_ERR_NOSYS    (-5)

/* Registerabbild, das der Einsprung in Assembler auf dem Stapel ablegt. */
struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t user_rsp;
} PACKED;

void syscall_init(void);
void syscall_dispatch(struct syscall_frame *frame);

/* Wird beim Threadwechsel gesetzt, damit der Einsprung den richtigen
 * Kernel-Stapel findet. */
void syscall_set_kernel_stack(uint64_t top);

#endif /* SYSCALL_H */
