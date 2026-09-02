/* process.h - Benutzerprogramme mit eigenem Adressraum.
 *
 * Ein Prozess ist ein aus einer ELF-Datei geladenes Programm, das in
 * Ring 3 laeuft und den Kernel nur ueber Systemaufrufe erreicht. Ein
 * Fehler darin kann das System nicht mehr anhalten - die CPU faengt den
 * Zugriff ab, und der Prozess wird beendet.
 *
 * Ein Prozess kann sich abspalten: Das Kind bekommt eine Kopie des
 * Adressraums, die zunaechst gar keine ist - beide benutzen dieselben
 * Seiten, bis eine davon beschrieben wird (siehe vmm_fork). Damit sind
 * mehrere Programme zugleich moeglich, die aus einem hervorgegangen
 * sind: ein Webserver etwa, der je Verbindung ein Kind abspaltet.
 *
 * Alle, die so zusammengehoeren, bilden eine Gruppe. Ihr Anfuehrer ist
 * der Prozess, den die Konsole gestartet hat; seine Ein- und Ausgabe
 * benutzen alle gemeinsam, und endet er, endet die ganze Gruppe.
 */
#ifndef PROCESS_H
#define PROCESS_H

#include "retro.h"
#include "vmm.h"
#include "vfs.h"
#include "syscall.h"

#define PROCESS_MAX       16
#define PROCESS_FILES_MAX 8
#define PROCESS_WINDOWS_MAX 4
#define PROCESS_SOCKETS_MAX 8
#define PROCESS_SHM_MAX     4
#define PROCESS_OUT_SIZE  8192
#define PROCESS_IN_SIZE   512
#define PROCESS_ARGS_MAX  128

struct thread;
struct pipe;

struct process {
    uint32_t pid;
    char     name[32];
    char     args[PROCESS_ARGS_MAX];

    /* Unter wem laeuft das Programm? Ein Kind erbt das vom Elternteil,
     * ein frisch gestartetes vom angemeldeten Benutzer. Die Rechte im
     * Dateibaum richten sich danach - ein Programm kann also nicht mehr
     * als der Mensch, der es aufgerufen hat. */
    uint32_t uid, gid;

    struct address_space space;
    struct thread       *thread;

    /* Wer hat mich abgespalten, und wem gehoert die Konsole? */
    struct process *parent;
    struct process *leader;
    uint32_t        parent_pid;

    bool     used;
    bool     finished;
    bool     reaping;        /* jemand raeumt gerade ab */
    int      exit_code;

    /* Registerabbild fuer ein frisch abgespaltenes Kind. */
    struct syscall_frame fork_frame;

    /* Ausgabe des Programms, die die Konsole abholt. */
    char     out[PROCESS_OUT_SIZE];
    volatile uint32_t out_head, out_tail;

    /* Eingabe, die die Konsole hineinreicht. */
    char     in[PROCESS_IN_SIZE];
    volatile uint32_t in_head, in_tail;

    struct fs_node *files[PROCESS_FILES_MAX];
    size_t          file_pos[PROCESS_FILES_MAX];

    /* Eine Nummer ist entweder eine Datei oder ein Ende einer Roehre.
     * Zwei Felder statt eines Typkennzeichens: So bleibt jeder
     * bestehende Zugriff auf files[] richtig, und die Roehre ist ein
     * Zusatz und kein Umbau. */
    struct pipe    *pipes[PROCESS_FILES_MAX];
    bool            pipe_writer[PROCESS_FILES_MAX];

    /* Welche geteilten Bereiche dieses Programm eingeblendet hat -
     * beim Beenden werden sie abgeraeumt. */
    int             shm[PROCESS_SHM_MAX];

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

/* Spaltet den aufrufenden Prozess ab. Das Kind faengt hinter demselben
 * Systemaufruf wieder an, nur mit 0 als Ergebnis. Liefert NULL, wenn
 * kein Platz mehr ist. */
struct process *process_fork(struct process *parent,
                             const struct syscall_frame *frame);

/* Holt den Ausgang eines beendeten Kindes ab. pid 0 heisst "irgendeins".
 * Liefert die Nummer des abgeholten Kindes, 0 wenn in der Wartezeit
 * keines fertig wurde, und -1, wenn es gar keine Kinder gibt. */
int32_t process_wait(struct process *parent, uint32_t pid, int *code,
                     uint32_t timeout_ms);

/* Gibt den Steckplatz eines beendeten Prozesses frei - samt allem, was
 * unter ihm haengt. Danach zeigt der Zeiger ins Leere. */
void process_release(struct process *proc);

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

/* Unter welcher Nummer laeuft der Prozess? Getrennt deklariert, damit
 * die Rechtepruefung nicht den ganzen Prozesskopf braucht. */
uint32_t process_uid(struct process *proc);
uint32_t process_gid(struct process *proc);

#endif /* PROCESS_H */
