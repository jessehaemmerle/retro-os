/* process.c - ELF-Programme laden und in Ring 3 starten.
 *
 * Eine ELF-Datei beschreibt in ihren Programmkopfzeilen, welche Teile
 * wohin in den Speicher gehoeren. Mehr braucht es nicht: fuer jedes
 * ladbare Segment werden Seiten angelegt, der Inhalt hineinkopiert und
 * die Abbildung im Adressraum des Prozesses eingetragen. Danach wird ein
 * Stapel bereitgestellt und in den Ring 3 gesprungen.
 */

#include "process.h"
#include "arch.h"
#include "kstring.h"
#include "mm.h"
#include "net.h"
#include "syscall.h"
#include "uiapi.h"
#include "thread.h"

void enter_user_mode(uint64_t entry, uint64_t stack) NORETURN;

/* --- ELF-Strukturen (nur, was gebraucht wird) --- */

#define PT_LOAD 1

/* Rechte eines Segments, wie sie in der Programmdatei stehen. */
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4
#define ET_EXEC 2

struct elf64_ehdr {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} PACKED;

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} PACKED;

static struct process processes[PROCESS_MAX];
static uint32_t       next_pid = 1;

void process_init(void)
{
    memset(processes, 0, sizeof(processes));
}

struct process *process_current(void)
{
    struct thread *t = thread_current();

    return t ? t->process : NULL;
}

size_t process_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].used)
            n++;
    }
    return n;
}

struct process *process_at(size_t index)
{
    size_t n = 0;

    for (size_t i = 0; i < PROCESS_MAX; i++) {
        if (!processes[i].used)
            continue;
        if (n++ == index)
            return &processes[i];
    }
    return NULL;
}

/* --- Ausgabe und Eingabe ------------------------------------------- */

void process_append_output(struct process *proc, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        uint32_t next = (proc->out_head + 1) % PROCESS_OUT_SIZE;

        if (next == proc->out_tail)
            break;              /* Puffer voll - Rest verwerfen */

        proc->out[proc->out_head] = text[i];
        proc->out_head = next;
    }
}

size_t process_read_output(struct process *proc, char *buffer, size_t size)
{
    size_t n = 0;

    while (n + 1 < size && proc->out_tail != proc->out_head) {
        buffer[n++] = proc->out[proc->out_tail];
        proc->out_tail = (proc->out_tail + 1) % PROCESS_OUT_SIZE;
    }
    buffer[n] = '\0';
    return n;
}

void process_write_input(struct process *proc, const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        uint32_t next = (proc->in_head + 1) % PROCESS_IN_SIZE;

        if (next == proc->in_tail)
            break;

        proc->in[proc->in_head] = text[i];
        proc->in_head = next;
    }
}

size_t process_take_input(struct process *proc, char *buffer, size_t size)
{
    size_t n = 0;

    while (n < size && proc->in_tail != proc->in_head) {
        buffer[n++] = proc->in[proc->in_tail];
        proc->in_tail = (proc->in_tail + 1) % PROCESS_IN_SIZE;
    }
    return n;
}

/* --- Laden ---------------------------------------------------------- */

static bool load_elf(struct process *proc, const uint8_t *image, size_t size,
                     uint64_t *entry, char *error, size_t error_size)
{
    if (size < sizeof(struct elf64_ehdr)) {
        strlcpy(error, "Die Datei ist zu klein fuer ein Programm.", error_size);
        return false;
    }

    const struct elf64_ehdr *header = (const struct elf64_ehdr *)image;

    if (memcmp(header->ident, "\177ELF", 4) != 0) {
        strlcpy(error, "Das ist keine ELF-Datei.", error_size);
        return false;
    }
    if (header->ident[4] != 2 || header->machine != 0x3E) {
        strlcpy(error, "Das Programm ist nicht fuer x86-64 uebersetzt.",
                error_size);
        return false;
    }
    if (header->type != ET_EXEC) {
        strlcpy(error, "Nur unverschiebbare Programme koennen geladen werden.",
                error_size);
        return false;
    }

    for (uint16_t i = 0; i < header->phnum; i++) {
        uint64_t offset = header->phoff + (uint64_t)i * header->phentsize;

        if (offset + sizeof(struct elf64_phdr) > size) {
            strlcpy(error, "Die Programmkopfzeilen sind unvollstaendig.",
                    error_size);
            return false;
        }

        const struct elf64_phdr *ph = (const struct elf64_phdr *)(image + offset);

        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;

        if (ph->vaddr < USER_BASE || ph->vaddr + ph->memsz >= USER_STACK_TOP) {
            strlcpy(error, "Das Programm will an eine unerlaubte Adresse.",
                    error_size);
            return false;
        }
        if (ph->offset + ph->filesz > size) {
            strlcpy(error, "Ein Segment liegt ausserhalb der Datei.", error_size);
            return false;
        }

        /* Zum Fuellen muss das Segment beschreibbar sein - die Rechte
         * aus der Datei bekommt es unmittelbar danach. */
        if (!vmm_alloc_range(&proc->space, ph->vaddr, ph->memsz,
                             PTE_PRESENT | PTE_USER | PTE_WRITE | PTE_NX)) {
            strlcpy(error, "Zu wenig Speicher fuer das Programm.", error_size);
            return false;
        }
        if (ph->filesz > 0 &&
            !vmm_copy_to_user(&proc->space, ph->vaddr, image + ph->offset,
                              ph->filesz)) {
            strlcpy(error, "Das Programm liess sich nicht kopieren.", error_size);
            return false;
        }

        /* Code darf nicht beschrieben, Daten nicht ausgefuehrt werden.
         * Ein Programm, das sich selbst umschreiben will, faellt damit
         * auf - so wie es sich gehoert. */
        uint64_t flags = PTE_PRESENT | PTE_USER;

        if (ph->flags & PF_W)
            flags |= PTE_WRITE;
        if (!(ph->flags & PF_X))
            flags |= PTE_NX;

        vmm_protect_range(&proc->space, ph->vaddr, ph->memsz, flags);

        uint64_t top = ALIGN_UP(ph->vaddr + ph->memsz, PAGE_SIZE);
        if (top > proc->space.heap_break)
            proc->space.heap_break = top;
    }

    *entry = header->entry;
    return true;
}

/* Der Einstieg des neuen Threads: noch im Kernel, gleich in Ring 3. */
static void process_entry(void *argument)
{
    struct process *proc = argument;
    uint64_t entry = (uint64_t)proc->files[0];   /* zwischengeparkt */
    uint64_t stack = USER_STACK_TOP - 32;

    proc->files[0] = NULL;

    tss_set_kernel_stack(thread_current()->kernel_stack_top);
    syscall_set_kernel_stack(thread_current()->kernel_stack_top);
    vmm_switch(&proc->space);

    enter_user_mode(entry, stack);
}

struct process *process_start(const char *path, const char *args,
                              char *error, size_t error_size)
{
    struct fs_node *file = fs_lookup(fs_root(), path);

    if (!file || file->type != FS_FILE) {
        ksnprintf(error, error_size, "%s wurde nicht gefunden.", path);
        return NULL;
    }
    if (!fs_load(file)) {
        strlcpy(error, "Die Datei laesst sich nicht lesen.", error_size);
        return NULL;
    }

    struct process *proc = NULL;
    for (size_t i = 0; i < PROCESS_MAX; i++) {
        if (!processes[i].used) {
            proc = &processes[i];
            break;
        }
    }
    if (!proc) {
        strlcpy(error, "Es laufen schon zu viele Programme.", error_size);
        return NULL;
    }

    memset(proc, 0, sizeof(*proc));
    proc->pid = next_pid++;

    const char *slash = strrchr(path, '/');
    strlcpy(proc->name, slash ? slash + 1 : path, sizeof(proc->name));
    if (args)
        strlcpy(proc->args, args, sizeof(proc->args));

    if (!vmm_create(&proc->space)) {
        strlcpy(error, "Der Adressraum liess sich nicht anlegen.", error_size);
        return NULL;
    }

    uint64_t entry = 0;
    if (!load_elf(proc, file->data, file->size, &entry, error, error_size)) {
        vmm_destroy(&proc->space);
        return NULL;
    }

    /* Stapel des Programms. */
    if (!vmm_alloc_range(&proc->space, USER_STACK_TOP - USER_STACK_SIZE,
                         USER_STACK_SIZE,
                         PTE_PRESENT | PTE_USER | PTE_WRITE | PTE_NX)) {
        strlcpy(error, "Kein Platz fuer den Stapel.", error_size);
        vmm_destroy(&proc->space);
        return NULL;
    }

    proc->used = true;
    proc->files[0] = (struct fs_node *)entry;   /* kurz zwischengeparkt */

    proc->thread = thread_create(proc->name, process_entry, proc, PRIO_NORMAL);
    if (!proc->thread) {
        strlcpy(error, "Kein freier Thread.", error_size);
        vmm_destroy(&proc->space);
        proc->used = false;
        return NULL;
    }

    proc->thread->process = proc;
    return proc;
}

/* Fenster und Verbindungen gehoeren dem Programm; endet es, muessen
 * sie weg - auch dann, wenn es sich nicht selbst darum gekuemmert
 * hat. */
static void release_resources(struct process *proc)
{
    uiapi_release(proc);

    for (int i = 0; i < PROCESS_SOCKETS_MAX; i++) {
        if (proc->sockets[i]) {
            tcp_close(proc->sockets[i]);
            proc->sockets[i] = NULL;
        }
    }
}

void process_exit(struct process *proc, int code)
{
    proc->exit_code = code;
    proc->finished = true;

    release_resources(proc);

    /* Der Adressraum wird erst freigegeben, wenn niemand mehr darin
     * laeuft - deshalb vorher zurueck in den Kernel-Adressraum. */
    vmm_switch_kernel();
    thread_current()->process = NULL;
    vmm_destroy(&proc->space);
}

void process_kill(struct process *proc)
{
    if (!proc || !proc->used)
        return;

    proc->finished = true;
    proc->exit_code = -1;

    release_resources(proc);

    if (proc->thread && proc->thread->state != THREAD_DEAD)
        proc->thread->state = THREAD_DEAD;

    vmm_destroy(&proc->space);
    proc->used = false;
}
