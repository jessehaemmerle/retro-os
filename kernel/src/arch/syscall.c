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
#include "cpu.h"
#include "kstring.h"
#include "mm.h"
#include "process.h"
#include "thread.h"
#include "vfs.h"
#include "gui.h"
#include "ipc.h"
#include "net.h"
#include "uiapi.h"
#include "vmm.h"

#define MSR_EFER   0xC0000080
#define MSR_STAR   0xC0000081
#define MSR_LSTAR  0xC0000082
#define MSR_FMASK  0xC0000084

#define COPY_CHUNK 512

void syscall_entry(void);

/* Ein Bereich je Kern, auf den GS zeigt. Der Einsprung in Assembler
 * liest daraus seinen Kernelstapel und legt den des Benutzers ab. */
struct syscall_area {
    uint64_t kernel_rsp;
    uint64_t scratch_rsp;
} ALIGNED(64);

static struct syscall_area areas[CPU_MAX];

#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_GS_BASE        0xC0000101

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

/* Wie bei der TSS: Kern lesen und Feld schreiben muessen zusammen
 * geschehen. Ein Wechsel dazwischen wuerde den Einsprung eines
 * fremden Kerns auf diesen Stapel zeigen lassen. */
void syscall_set_kernel_stack(uint64_t top)
{
    uint64_t flags = irq_save();

    areas[cpu_current()->index].kernel_rsp = top;
    irq_restore(flags);
}

/* Traegt fuer diesen Kern ein, wohin GS zeigen soll.
 *
 * Beide Register bekommen denselben Wert. Der Grund ist der Wechsel
 * zwischen Kernen: Wandert ein Thread mitten im Systemaufruf auf einen
 * anderen Kern, tauscht das swapgs beim Ruecksprung sonst die
 * Kennung des alten Kerns in das Register des neuen - und der naechste
 * Systemaufruf dort greift ins Leere. Stehen in beiden dieselben
 * Adressen, ist der Tausch gleichgueltig.
 *
 * Der Preis: Ein Benutzerprogramm kann GS nicht fuer sich benutzen.
 * RetroOS-Programme tun das nicht. */
void syscall_bind_cpu(void)
{
    uint64_t flags = irq_save();
    uint64_t area = (uint64_t)&areas[cpu_current()->index];

    wrmsr(MSR_KERNEL_GS_BASE, area);
    wrmsr(MSR_GS_BASE, area);
    irq_restore(flags);
}

static void bind_gs(void)
{
    syscall_bind_cpu();
}

/* Die MSRs gelten je Kern und muessen auf jedem eigens gesetzt
 * werden. */
static void setup_msrs(void)
{
    /* Den Befehl "syscall" ueberhaupt erst freischalten. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);

    /* Segmente: Kernel bei 0x08, Benutzer abgeleitet aus 0x10. */
    wrmsr(MSR_STAR, ((uint64_t)0x08 << 32) | ((uint64_t)0x10 << 48));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /* Beim Eintritt Interrupts und Richtungsflag loeschen. */
    wrmsr(MSR_FMASK, (1 << 9) | (1 << 10) | (1 << 18));

    bind_gs();
}

void syscall_init_ap(void)
{
    setup_msrs();
}

void syscall_init(void)
{
    setup_msrs();

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
        if (!proc->files[i] && !proc->pipes[i])
            return i;
    }
    return -1;
}

/* --- Roehren und geteilter Speicher -------------------------------- */

static int64_t do_pipe(struct process *proc, uint64_t out_ptr)
{
    int read_fd = alloc_fd(proc);

    if (read_fd < 0)
        return SYS_ERR_BADFD;

    /* Den Platz gleich belegen, sonst gaebe alloc_fd zweimal dieselbe
     * Nummer zurueck. */
    proc->pipes[read_fd] = (struct pipe *)1;

    int write_fd = alloc_fd(proc);

    proc->pipes[read_fd] = NULL;
    if (write_fd < 0)
        return SYS_ERR_BADFD;

    struct pipe *p = pipe_create();

    if (!p)
        return SYS_ERR_NOMEM;

    int32_t pair[2] = { read_fd, write_fd };

    if (!user_write(proc, out_ptr, pair, sizeof(pair))) {
        pipe_close(p, false);
        pipe_close(p, true);
        return SYS_ERR_INVAL;
    }

    proc->pipes[read_fd]       = p;
    proc->pipe_writer[read_fd] = false;
    proc->pipes[write_fd]       = p;
    proc->pipe_writer[write_fd] = true;
    return 0;
}

static int64_t do_shm_open(struct process *proc, uint64_t name_ptr,
                           uint64_t bytes, uint64_t create)
{
    char name[SHM_NAME_MAX + 1];

    if (!user_string(proc, name, name_ptr, sizeof(name)))
        return SYS_ERR_INVAL;

    int id = shm_open(name, (size_t)bytes, create != 0);

    return id < 0 ? SYS_ERR_NOENT : id;
}

static int64_t do_shm_map(struct process *proc, int id)
{
    /* Zweimal einblenden liefert dieselbe Adresse und zaehlt nur
     * einmal - sonst muesste das Programm mitzaehlen. */
    for (int i = 0; i < PROCESS_SHM_MAX; i++)
        if (proc->shm[i] == id + 1)
            return (int64_t)(SHM_BASE + (uint64_t)id * SHM_STRIDE);

    int slot = -1;

    for (int i = 0; i < PROCESS_SHM_MAX && slot < 0; i++)
        if (!proc->shm[i])
            slot = i;
    if (slot < 0)
        return SYS_ERR_NOMEM;

    uint64_t base = shm_attach(&proc->space, id);

    if (!base)
        return SYS_ERR_NOMEM;

    proc->shm[slot] = id + 1;
    return (int64_t)base;
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

    /* Eine Roehre am Lesende: Es wird gewartet, bis etwas kommt oder
     * der Schreiber verschwindet - ein Leser, der sofort mit "nichts
     * da" zurueckkaeme, muesste selbst kreisen. */
    if (fd >= 3 && fd < PROCESS_FILES_MAX && proc->pipes[fd]) {
        if (proc->pipe_writer[fd])
            return SYS_ERR_BADFD;

        size_t take = MIN(length, sizeof(chunk));
        int64_t n = pipe_read(proc->pipes[fd], chunk, take, 5000);

        if (n <= 0)
            return n < 0 ? 0 : 0;   /* Ende und Zeitablauf: beides 0 */
        if (!user_write(proc, buffer, chunk, (size_t)n))
            return SYS_ERR_INVAL;
        return n;
    }

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

static int64_t do_write_pipe(struct process *proc, int64_t fd, uint64_t buffer,
                             uint64_t length)
{
    char chunk[COPY_CHUNK];
    size_t take = MIN(length, sizeof(chunk));

    if (!proc->pipe_writer[fd])
        return SYS_ERR_BADFD;
    if (!user_read(proc, chunk, buffer, take))
        return SYS_ERR_INVAL;

    /* Ist die Roehre voll, wird gewartet statt abgeschnitten - sonst
     * muesste jedes Programm den Rest selbst nachreichen. */
    size_t done = 0;
    uint64_t deadline = timer_ms() + 5000;

    while (done < take) {
        int64_t n = pipe_write(proc->pipes[fd], chunk + done, take - done);

        if (n < 0)
            return done ? (int64_t)done : SYS_ERR_BADFD;
        done += (size_t)n;
        if (done < take) {
            if (timer_ms() >= deadline)
                break;
            thread_sleep(2);
        }
    }
    return (int64_t)done;
}

static int64_t do_write(struct process *proc, int64_t fd, uint64_t buffer,
                        uint64_t length)
{
    char chunk[COPY_CHUNK];
    size_t done = 0;

    if (fd >= 3 && fd < PROCESS_FILES_MAX && proc->pipes[fd])
        return do_write_pipe(proc, fd, buffer, length);

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

/* --- Fenster --- */

/* Die Zeichenbefehle kommen aus dem Programm; sie werden in Haeppchen
 * herueberkopiert und erst dann ausgefuehrt. So kann das Programm sie
 * nicht mehr aendern, waehrend der Kernel sie abarbeitet. */
static int64_t do_draw(struct process *proc, int32_t handle, uint64_t ptr,
                       uint64_t count)
{
    if (count == 0 || count > 512)
        return SYS_ERR_INVAL;

    struct user_draw_cmd batch[32];
    uint64_t done = 0;

    while (done < count) {
        size_t take = MIN((size_t)(count - done), ARRAY_LEN(batch));

        if (!user_read(proc, batch, ptr + done * sizeof(batch[0]),
                       take * sizeof(batch[0])))
            return SYS_ERR_INVAL;

        int64_t rc = uiapi_draw(proc, handle, batch, take);

        if (rc < 0)
            return rc;
        done += take;
    }
    return 0;
}

/* --- Netz --- */

static int64_t do_connect(struct process *proc, const char *host, uint16_t port)
{
    if (!net_ready())
        return SYS_ERR_NOENT;

    int slot = -1;

    for (int i = 0; i < PROCESS_SOCKETS_MAX; i++) {
        if (!proc->sockets[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return SYS_ERR_INVAL;

    /* Erst als Adresse lesen, sonst als Namen aufloesen. */
    ip_addr_t addr;

    if (!ip_parse(host, &addr) && !dns_resolve(host, &addr))
        return SYS_ERR_NOENT;

    struct tcp_socket *sock = tcp_connect(addr, port, 5000);

    if (!sock)
        return SYS_ERR_NOENT;

    proc->sockets[slot] = sock;
    return slot;
}

static struct tcp_socket *socket_of(struct process *proc, int32_t handle)
{
    if (handle < 0 || handle >= PROCESS_SOCKETS_MAX)
        return NULL;
    return proc->sockets[handle];
}

static int64_t do_send(struct process *proc, int32_t handle, uint64_t ptr,
                       uint64_t length)
{
    struct tcp_socket *sock = socket_of(proc, handle);

    if (!sock)
        return SYS_ERR_BADFD;
    if (length == 0 || length > 64 * 1024)
        return SYS_ERR_INVAL;

    char chunk[COPY_CHUNK];
    uint64_t sent = 0;

    while (sent < length) {
        size_t take = MIN((size_t)(length - sent), sizeof(chunk));

        if (!user_read(proc, chunk, ptr + sent, take))
            return SYS_ERR_INVAL;

        int written = tcp_send(sock, chunk, (uint32_t)take);

        if (written <= 0)
            return sent > 0 ? (int64_t)sent : SYS_ERR_INVAL;
        sent += (uint64_t)written;
    }
    return (int64_t)sent;
}

static int64_t do_recv(struct process *proc, int32_t handle, uint64_t ptr,
                       uint64_t length, uint32_t timeout_ms)
{
    struct tcp_socket *sock = socket_of(proc, handle);

    if (!sock)
        return SYS_ERR_BADFD;
    if (length == 0)
        return SYS_ERR_INVAL;

    char chunk[COPY_CHUNK];
    size_t take = MIN((size_t)length, sizeof(chunk));
    int got = tcp_receive(sock, chunk, (uint32_t)take, timeout_ms);

    if (got <= 0)
        return 0;
    if (!user_write(proc, ptr, chunk, (size_t)got))
        return SYS_ERR_INVAL;
    return got;
}

/* Ein freier Platz in der Steckplatzliste des Programms. */
static int free_socket_slot(struct process *proc)
{
    for (int i = 0; i < PROCESS_SOCKETS_MAX; i++) {
        if (!proc->sockets[i])
            return i;
    }
    return -1;
}

static int64_t do_listen(struct process *proc, uint16_t port)
{
    if (!net_ready())
        return SYS_ERR_NOENT;

    int slot = free_socket_slot(proc);

    if (slot < 0)
        return SYS_ERR_INVAL;

    /* Die kleinen Ports gehoeren dem System; ein Benutzerprogramm
     * bekommt sie nicht. Rechte gibt es hier sonst keine, und ohne
     * diese Grenze koennte jedes Programm den Platz eines Dienstes
     * belegen. */
    if (port < 1024)
        return SYS_ERR_INVAL;

    struct tcp_socket *sock = tcp_listen(port);

    if (!sock)
        return SYS_ERR_INVAL;

    proc->sockets[slot] = sock;
    return slot;
}

static int64_t do_accept(struct process *proc, int32_t handle,
                         uint32_t timeout_ms)
{
    struct tcp_socket *listener = socket_of(proc, handle);

    if (!listener)
        return SYS_ERR_BADFD;

    int slot = free_socket_slot(proc);

    if (slot < 0)
        return SYS_ERR_INVAL;

    struct tcp_socket *sock = tcp_accept(listener, timeout_ms);

    if (!sock)
        return SYS_ERR_NOENT;      /* in der Zeit kam keine */

    proc->sockets[slot] = sock;
    return slot;
}

static int64_t do_disconnect(struct process *proc, int32_t handle)
{
    struct tcp_socket *sock = socket_of(proc, handle);

    if (!sock)
        return SYS_ERR_BADFD;

    tcp_close(sock);
    proc->sockets[handle] = NULL;
    return 0;
}

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
        if (a1 >= 3 && a1 < PROCESS_FILES_MAX && proc->pipes[a1]) {
            pipe_close(proc->pipes[a1], proc->pipe_writer[a1]);
            proc->pipes[a1] = NULL;
            result = 0;
        } else if (a1 >= 3 && a1 < PROCESS_FILES_MAX && proc->files[a1]) {
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
                                 PTE_PRESENT | PTE_USER | PTE_WRITE | PTE_NX)) {
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

    /* --- Fenster --- */
    case SYS_WIN_OPEN: {
        char title[WIN_TITLE_MAX + 1];

        if (!user_string(proc, title, a1, sizeof(title))) {
            result = SYS_ERR_INVAL;
            break;
        }
        result = uiapi_open(proc, title, (int32_t)a2, (int32_t)a3);
        break;
    }

    case SYS_WIN_DRAW:
        result = do_draw(proc, (int32_t)a1, a2, a3);
        break;

    case SYS_WIN_EVENT: {
        struct user_event ev;

        result = uiapi_event(proc, (int32_t)a1, &ev, (uint32_t)a3);
        if (result == 1 && !user_write(proc, a2, &ev, sizeof(ev)))
            result = SYS_ERR_INVAL;
        break;
    }

    case SYS_WIN_CLOSE:
        result = uiapi_close(proc, (int32_t)a1);
        break;

    /* --- Netz --- */
    case SYS_CONNECT: {
        char host[128];

        if (!user_string(proc, host, a1, sizeof(host))) {
            result = SYS_ERR_INVAL;
            break;
        }
        result = do_connect(proc, host, (uint16_t)a2);
        break;
    }

    case SYS_SEND:
        result = do_send(proc, (int32_t)a1, a2, a3);
        break;

    case SYS_RECV:
        result = do_recv(proc, (int32_t)a1, a2, a3, (uint32_t)a4);
        break;

    case SYS_DISCONNECT:
        result = do_disconnect(proc, (int32_t)a1);
        break;

    case SYS_LISTEN:
        result = do_listen(proc, (uint16_t)a1);
        break;

    case SYS_ACCEPT:
        result = do_accept(proc, (int32_t)a1, (uint32_t)a2);
        break;

    /* --- Prozesse --- */
    case SYS_FORK: {
        struct process *child = process_fork(proc, frame);

        result = child ? (int64_t)child->pid : SYS_ERR_NOMEM;
        break;
    }

    case SYS_WAIT: {
        int code = 0;
        int32_t found = process_wait(proc, (uint32_t)a1, &code, (uint32_t)a3);

        if (found < 0) {
            result = SYS_ERR_NOCHILD;
            break;
        }
        if (found > 0 && a2 && !user_write(proc, a2, &code, sizeof(code))) {
            result = SYS_ERR_INVAL;
            break;
        }
        result = found;
        break;
    }

    /* --- Verstaendigung zwischen Programmen --- */
    case SYS_PIPE:
        result = do_pipe(proc, a1);
        break;

    case SYS_SHM_OPEN:
        result = do_shm_open(proc, a1, a2, a3);
        break;

    case SYS_SHM_MAP:
        result = do_shm_map(proc, (int)a1);
        break;

    case SYS_SHM_UNLINK: {
        char name[SHM_NAME_MAX + 1];

        if (!user_string(proc, name, a1, sizeof(name)))
            result = SYS_ERR_INVAL;
        else
            result = shm_unlink(name) ? 0 : SYS_ERR_NOENT;
        break;
    }

    default:
        result = SYS_ERR_NOSYS;
        break;
    }

    frame->rax = (uint64_t)result;
}
