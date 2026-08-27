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

    /* --- Fenster --- */
    SYS_WIN_OPEN  = 14,  /* (titel, breite, hoehe) -> Nummer */
    SYS_WIN_DRAW  = 15,  /* (fenster, befehle, anzahl)       */
    SYS_WIN_EVENT = 16,  /* (fenster, ereignis, wartezeit)   */
    SYS_WIN_CLOSE = 17,  /* (fenster)                        */

    /* --- Netz --- */
    SYS_CONNECT   = 18,  /* (name, port) -> Verbindung       */
    SYS_SEND      = 19,  /* (verbindung, puffer, laenge)     */
    SYS_RECV      = 20,  /* (verbindung, puffer, laenge, ms) */
    SYS_DISCONNECT = 21, /* (verbindung)                     */

    SYS_COUNT
};

/* Ein Zeichenbefehl, wie ihn das Programm hinueberreicht. Der Aufbau
 * steht so auch in userland/include/retroui.h - beide Seiten muessen
 * sich einig sein. */
#define USER_DRAW_TEXT_MAX 48

enum user_draw_op {
    UDRAW_CLEAR = 0,
    UDRAW_FILL,
    UDRAW_RECT,
    UDRAW_LINE,
    UDRAW_TEXT,
    UDRAW_PIXEL,
};

struct user_draw_cmd {
    uint32_t op;
    int32_t  x, y, w, h;
    uint32_t color;
    char     text[USER_DRAW_TEXT_MAX];
} PACKED;

enum user_event_type {
    UEV_NONE = 0,
    UEV_KEY,
    UEV_MOUSE_DOWN,
    UEV_MOUSE_UP,
    UEV_MOUSE_MOVE,
    UEV_CLOSE,
};

struct user_event {
    uint32_t type;
    int32_t  x, y;
    uint32_t key;
    char     ascii;
    uint8_t  mods, button;
} PACKED;

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
/* Dasselbe fuer einen weiteren Kern. */
void syscall_init_ap(void);
/* Setzt GS auf den Bereich des Kerns, auf dem gerade gewechselt wird. */
void syscall_bind_cpu(void);
void syscall_dispatch(struct syscall_frame *frame);

/* Wird beim Threadwechsel gesetzt, damit der Einsprung den richtigen
 * Kernel-Stapel findet. */
void syscall_set_kernel_stack(uint64_t top);

#endif /* SYSCALL_H */
