/* syscall.c - Bearbeitung der Systemaufrufe.
 *
 * Jeder Aufruf kommt mit Zeigern in den Adressraum des Programms. Der
 * Kernel darf sie nicht einfach benutzen: er kopiert sie ueber die direkte
 * Abbildung, nachdem er geprueft hat, dass sie tatsaechlich zu diesem
 * Prozess gehoeren. Zeigt eine Adresse ins Nichts, gibt es einen Fehler
 * statt eines Absturzes.
 */

#include "syscall.h"
#include "arch.h"
#include "kstring.h"
#include "mm.h"
#include "process.h"
#include "thread.h"
#include "vfs.h"
#include "vmm.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_FMASK  0xC0000084

#define COPY_CHUNK 512

void syscall_entry(void);
extern uint64_t syscall_kernel_rsp;

static void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" ::
                     "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;

    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

void syscall_set_kernel_stack(uint64_t top)
{
    syscall_kernel_rsp = top;
}

void syscall_init(void)
{
    /* Den Befehl "syscall" ueberhaupt erst freischalten. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);

    /* Segmente: Kernel bei 0x08, Benutzer abgeleitet aus 0x10. */
    wrmsr(MSR_STAR, ((uint64_t)0x08 << 32) | ((uint64_t)0x10 << 48));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* Beim Eintritt Interrupts und Richtungsflag loeschen. */
    wrmsr(MSR_FMASK, (1 << 9) | (1 << 10) | (1 << 18));

    kprintf("Systemaufrufe: bereit (%u Nummern)\n", (unsigned)SYS_COUNT);
}

/* --- Hilfen fuer Zeiger aus dem Benutzerprogramm --- */

static bool user_read(struct process *proc, void *dst, uint64_t src,
                      size_t length)
{
    if (src < USER_BASE || src + length > USER_STACK_TOP)
        return false;
    return vmm_copy_from_user(&proc->space, dst, src, length);
}

static bool user_write(struct process *proc, uint64_t dst, const void *src,
                       size_t length)
{
    if (dst < USER_BASE || dst + length > USER_STACK_TOP)
        return false;
    return vmm_copy_to_user(&proc->space, dst, src, length);
}

/* Liest eine Zeichenkette aus dem Programm - hoechstens size Zeichen. */
static bool user_string(struct process *proc, char *dst, uint64_t src,
                        size_t size)
{
    for (size_t i = 0; i < size - 1; i++) {
        char c;

        if (!user_read(proc, &c, src + i, 1))
            return false;

        dst[i] = c;
        if (c == '\0')
            return true;
    }
    dst[size - 1] = '\0';
    return true;
}

/* --- Dateizugriffe --- */

static int alloc_fd(struct process *proc)
{
    for (int i = 3; i < PROCESS_FILES_MAX; i++) {
        if (!proc->files[i])
            return i;
    }
    return -1;
}

static int64_t do_open(struct process *proc, uint64_t path_ptr, uint64_t mode)
{
    char path[FS_PATH_MAX];

    if (!user_string(proc, path, path_ptr, sizeof(path)))
        return SYS_ERR_INVAL;

    struct fs_node *node = fs_lookup(fs_root(), path);

    /* Modus 1 legt die Datei bei Bedarf an. */
    if (!node && mode == 1) {
        node = fs_create_path(fs_root(), path, FS_FILE);
        if (node)
            fs_write(node, "", 0);
    }

    if (!node || node->type != FS_FILE)
        return SYS_ERR_NOENT;
    if (!fs_load(node))
        return SYS_ERR_NOENT;

    int fd = alloc_fd(proc);
    if (fd < 0)
        return SYS_ERR_BADFD;

    proc->files[fd] = node;
    proc->file_pos[fd] = 0;
    return fd;
}

static int64_t do_read(struct process *proc, int64_t fd, uint64_t buffer,
                       uint64_t length)
{
    char chunk[COPY_CHUNK];

    if (fd == 0) {
        size_t take = MIN(length, sizeof(chunk));
        size_t n = process_take_input(proc, chunk, take);

        if (n == 0)
            return 0;
        if (!user_write(proc, buffer, chunk, n))
            return SYS_ERR_INVAL;
        return (int64_t)n;
    }

    if (fd < 3 || fd >= PROCESS_FILES_MAX || !proc->files[fd])
        return SYS_ERR_BADFD;

    struct fs_node *node = proc->files[fd];
    size_t position = proc->file_pos[fd];

    if (position >= node->size)
        return 0;

    size_t take = MIN(length, node->size - position);
    size_t done = 0;

    while (done < take) {
        size_t part = MIN(take - done, sizeof(chunk));

        memcpy(chunk, node->data + position + done, part);
        if (!user_write(proc, buffer + done, chunk, part))
            return SYS_ERR_INVAL;
        done += part;
    }

    proc->file_pos[fd] += done;
    return (int64_t)done;
}

static int64_t do_write(struct process *proc, int64_t fd, uint64_t buffer,
                        uint64_t length)
{
    char chunk[COPY_CHUNK];
    size_t done = 0;

    while (done < length) {
        size_t part = MIN(length - done, sizeof(chunk));

        if (!user_read(proc, chunk, buffer + done, part))
            return SYS_ERR_INVAL;

        if (fd == 1 || fd == 2) {
            process_append_output(proc, chunk, part);
        } else if (fd >= 3 && fd < PROCESS_FILES_MAX && proc->files[fd]) {
            if (!fs_append(proc->files[fd], chunk, part))
                return SYS_ERR_INVAL;
        } else {
            return SYS_ERR_BADFD;
        }
        done += part;
    }
    return (int64_t)done;
}

static int64_t do_readdir(struct process *proc, uint64_t path_ptr,
                          uint64_t index, uint64_t buffer, uint64_t length)
{
    char path[FS_PATH_MAX];

    if (!user_string(proc, path, path_ptr, sizeof(path)))
        return SYS_ERR_INVAL;

    struct fs_node *dir = fs_lookup(fs_root(), path);
    if (!dir || dir->type != FS_DIR)
        return SYS_ERR_NOENT;

    struct fs_node *entries[256];
    size_t count = fs_list(dir, entries, ARRAY_LEN(entries));

    if (index >= count)
        return 0;

    char line[FS_NAME_MAX + 24];
    ksnprintf(line, sizeof(line), "%s%s", entries[index]->name,
              entries[index]->type == FS_DIR ? "/" : "");

    size_t n = MIN(strlen(line) + 1, length);
    if (!user_write(proc, buffer, line, n))
        return SYS_ERR_INVAL;

    return (int64_t)count;
}

/* --- Verteiler --- */

void syscall_dispatch(struct syscall_frame *frame)
{
    struct process *proc = process_current();

    if (!proc) {
        frame->rax = (uint64_t)SYS_ERR_NOSYS;
        return;
    }

    uint64_t number = frame->rax;
    uint64_t a1 = frame->rdi, a2 = frame->rsi, a3 = frame->rdx;
    uint64_t a4 = frame->r10;
    int64_t result = SYS_ERR_NOSYS;

    switch (number) {
    case SYS_EXIT:
        process_exit(proc, (int)a1);
        thread_exit();
        break;

    case SYS_WRITE:
        result = do_write(proc, (int64_t)a1, a2, a3);
        break;

    case SYS_READ:
        result = do_read(proc, (int64_t)a1, a2, a3);
        break;

    case SYS_OPEN:
        result = do_open(proc, a1, a2);
        break;

    case SYS_CLOSE:
        if (a1 >= 3 && a1 < PROCESS_FILES_MAX && proc->files[a1]) {
            proc->files[a1] = NULL;
            result = 0;
        } else {
            result = SYS_ERR_BADFD;
        }
        break;

    case SYS_SEEK:
        if (a1 >= 3 && a1 < PROCESS_FILES_MAX && proc->files[a1]) {
            proc->file_pos[a1] = MIN((size_t)a2, proc->files[a1]->size);
            result = (int64_t)proc->file_pos[a1];
        } else {
            result = SYS_ERR_BADFD;
        }
        break;

    case SYS_SBRK: {
        uint64_t old = proc->space.heap_break;
        int64_t delta = (int64_t)a1;

        if (delta > 0) {
            if (!vmm_alloc_range(&proc->space, old, (size_t)delta,
                                 PTE_PRESENT | PTE_USER | PTE_WRITE)) {
                result = SYS_ERR_NOMEM;
                break;
            }
            proc->space.heap_break = ALIGN_UP(old + (uint64_t)delta, PAGE_SIZE);
        }
        result = (int64_t)old;
        break;
    }

    case SYS_SLEEP:
        thread_sleep((uint32_t)a1);
        result = 0;
        break;

    case SYS_YIELD:
        thread_yield();
        result = 0;
        break;

    case SYS_GETPID:
        result = (int64_t)proc->pid;
        break;

    case SYS_UPTIME:
        result = (int64_t)timer_ms();
        break;

    case SYS_ARGS: {
        size_t n = MIN(strlen(proc->args) + 1, (size_t)a2);

        result = user_write(proc, a1, proc->args, n) ? (int64_t)n
                                                     : SYS_ERR_INVAL;
        break;
    }

    case SYS_FILESIZE:
        if (a1 >= 3 && a1 < PROCESS_FILES_MAX && proc->files[a1])
            result = (int64_t)proc->files[a1]->size;
        else
            result = SYS_ERR_BADFD;
        break;

    case SYS_READDIR:
        result = do_readdir(proc, a1, a2, a3, a4);
        break;

    default:
        result = SYS_ERR_NOSYS;
        break;
    }

    frame->rax = (uint64_t)result;
}
