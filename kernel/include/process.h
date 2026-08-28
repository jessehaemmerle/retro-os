/* process.h - Benutzerprogramme mit eigenem Adressraum.
 *
 * Ein Prozess ist ein aus einer ELF-Datei geladenes Programm, das in
 * Ring 3 laeuft und den Kernel nur ueber Systemaufrufe erreicht. Ein
 * Fehler darin kann das System nicht mehr anhalten - die CPU faengt den
 * Zugriff ab, und der Prozess wird beendet.
 */
#ifndef PROCESS_H
#define PROCESS_H

#include "retro.h"
#include "vmm.h"
#include "vfs.h"

#define PROCESS_MAX       8
#define PROCESS_FILES_MAX 8
#define PROCESS_WINDOWS_MAX 4
#define PROCESS_SOCKETS_MAX 8
#define PROCESS_OUT_SIZE  8192
#define PROCESS_IN_SIZE   512
#define PROCESS_ARGS_MAX  128

struct thread;

struct process {
    uint32_t pid;
    char     name[32];
    char     args[PROCESS_ARGS_MAX];

    struct address_space space;
    struct thread       *thread;

    bool     used;
    bool     finished;
    int      exit_code;

    /* Ausgabe des Programms, die die Konsole abholt. */
    char     out[PROCESS_OUT_SIZE];
    volatile uint32_t out_head, out_tail;

    /* Eingabe, die die Konsole hineinreicht. */
    char     in[PROCESS_IN_SIZE];
    volatile uint32_t in_head, in_tail;

    struct fs_node *files[PROCESS_FILES_MAX];
    size_t          file_pos[PROCESS_FILES_MAX];

    /* Fenster und Verbindungen, die das Programm geoeffnet hat. Beim
     * Beenden raeumt der Kernel sie ab - ein Programm, das abstuerzt,
     * soll kein Fenster zuruecklassen. */
    struct user_window *windows[PROCESS_WINDOWS_MAX];
    struct tcp_socket  *sockets[PROCESS_SOCKETS_MAX];
};

void process_init(void);

/* Laedt und startet ein Programm. Liefert NULL, wenn die Datei fehlt
 * oder kein gueltiges Programm enthaelt. */
struct process *process_start(const char *path, const char *args,
                              char *error, size_t error_size);

struct process *process_current(void);
void process_kill(struct process *proc);

/* Ausgabe abholen; liefert die Anzahl der kopierten Zeichen. */
size_t process_read_output(struct process *proc, char *buffer, size_t size);
/* Eingabe hineinreichen. */
void   process_write_input(struct process *proc, const char *text, size_t length);

/* Von Kernel-Seite: Ausgabe anhaengen, Eingabe entnehmen, beenden. */
void   process_append_output(struct process *proc, const char *text, size_t length);
size_t process_take_input(struct process *proc, char *buffer, size_t size);
void   process_exit(struct process *proc, int code);

size_t process_count(void);
struct process *process_at(size_t index);

#endif /* PROCESS_H */
