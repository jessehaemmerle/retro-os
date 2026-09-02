/* ipc.c - Roehren und geteilter Speicher.
 *
 * Beides liegt in festen Feldern und nicht im Heap: Ein Prozess, der
 * abstuerzt, laesst so nichts zurueck, was niemand mehr aufraeumen
 * kann, und die Zahl der Bereiche ist ohnehin klein.
 *
 * Die Sperre ist eine einzige fuer beide Sorten. Sie wird nur fuer das
 * Umkopieren und Zaehlen gehalten, nie ueber ein Warten - wer mit
 * gezogener Sperre schlaeft, laesst alle anderen drehen.
 */

#include "ipc.h"

#include "arch.h"
#include "kstring.h"
#include "log.h"
#include "mm.h"
#include "spinlock.h"
#include "thread.h"
#include "user.h"

struct pipe {
    bool     used;
    uint8_t  data[PIPE_BUFFER];
    size_t   head, tail;      /* head schreibt, tail liest */
    size_t   fill;
    uint32_t readers, writers;
};

struct shm_segment {
    bool     used;
    bool     named;           /* Name noch vergeben?      */
    char     name[SHM_NAME_MAX + 1];
    size_t   pages;
    uint64_t phys[SHM_PAGES_MAX];
    uint32_t attached;        /* wie viele Adressraeume   */
    uint32_t uid;
};

static struct pipe        pipes[PIPE_MAX];
static struct shm_segment segments[SHM_MAX];
static struct spinlock    lock = SPINLOCK_INIT("ipc");

void ipc_init(void)
{
    memset(pipes, 0, sizeof(pipes));
    memset(segments, 0, sizeof(segments));
}

/* ------------------------------------------------------------------ */
/* Roehren                                                             */
/* ------------------------------------------------------------------ */

struct pipe *pipe_create(void)
{
    uint64_t flags = spin_lock_irq(&lock);
    struct pipe *found = NULL;

    for (size_t i = 0; i < PIPE_MAX && !found; i++) {
        if (pipes[i].used)
            continue;
        memset(&pipes[i], 0, sizeof(pipes[i]));
        pipes[i].used    = true;
        pipes[i].readers = 1;
        pipes[i].writers = 1;
        found = &pipes[i];
    }

    spin_unlock_irq(&lock, flags);
    return found;
}

void pipe_share(struct pipe *p, bool writer)
{
    if (!p)
        return;

    uint64_t flags = spin_lock_irq(&lock);

    if (writer)
        p->writers++;
    else
        p->readers++;
    spin_unlock_irq(&lock, flags);
}

void pipe_close(struct pipe *p, bool writer)
{
    if (!p)
        return;

    uint64_t flags = spin_lock_irq(&lock);

    if (writer && p->writers)
        p->writers--;
    else if (!writer && p->readers)
        p->readers--;

    bool gone = p->readers == 0 && p->writers == 0;

    if (gone)
        memset(p, 0, sizeof(*p));
    spin_unlock_irq(&lock, flags);

    /* Wer auf Nachschub wartet, soll das Ende mitbekommen - sonst
     * haengt ein Leser bis zum Zeitablauf an einer Roehre, in die
     * niemand mehr schreibt. */
    if (!gone)
        wake_all(p);
}

int64_t pipe_write(struct pipe *p, const void *data, size_t length)
{
    if (!p || !p->used)
        return -1;

    uint64_t flags = spin_lock_irq(&lock);

    /* In eine Roehre ohne Leser zu schreiben ist ein Fehler und kein
     * stilles Wegwerfen - das Programm soll es merken. */
    if (!p->readers) {
        spin_unlock_irq(&lock, flags);
        return -1;
    }

    size_t room = PIPE_BUFFER - p->fill;
    size_t take = MIN(length, room);
    const uint8_t *src = data;

    for (size_t i = 0; i < take; i++) {
        p->data[p->head] = src[i];
        p->head = (p->head + 1) % PIPE_BUFFER;
    }
    p->fill += take;

    spin_unlock_irq(&lock, flags);

    if (take)
        wake_all(p);
    return (int64_t)take;
}

int64_t pipe_read(struct pipe *p, void *out, size_t length,
                  uint32_t timeout_ms)
{
    if (!p || !p->used)
        return -1;

    uint64_t deadline = timer_ms() + timeout_ms;

    for (;;) {
        uint64_t flags = spin_lock_irq(&lock);

        if (p->fill) {
            size_t take = MIN(length, p->fill);
            uint8_t *dst = out;

            for (size_t i = 0; i < take; i++) {
                dst[i] = p->data[p->tail];
                p->tail = (p->tail + 1) % PIPE_BUFFER;
            }
            p->fill -= take;
            spin_unlock_irq(&lock, flags);

            /* Ein Schreiber, der auf Platz gewartet hat, darf weiter. */
            wake_all(p);
            return (int64_t)take;
        }

        bool ended = p->writers == 0;

        spin_unlock_irq(&lock, flags);

        /* Leer und kein Schreiber mehr: Das ist das Ende und kein
         * Grund, weiter zu warten. */
        if (ended)
            return -1;
        if (timer_ms() >= deadline)
            return 0;

        /* Hoechstens bis zum Ablauf schlafen - geweckt wird ohnehin,
         * sobald jemand schreibt oder das letzte Ende zugeht. */
        uint64_t left = deadline - timer_ms();

        wait_on(p, NULL, (uint32_t)MIN(left, 50));
    }
}

size_t pipe_pending(const struct pipe *p)
{
    return p && p->used ? p->fill : 0;
}

size_t pipe_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < PIPE_MAX; i++)
        if (pipes[i].used)
            n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Geteilter Speicher                                                  */
/* ------------------------------------------------------------------ */

static void free_segment(struct shm_segment *s)
{
    for (size_t i = 0; i < s->pages; i++)
        if (s->phys[i])
            pmm_free_page(s->phys[i]);
    memset(s, 0, sizeof(*s));
}

int shm_open(const char *name, size_t bytes, bool create)
{
    if (!name || !name[0] || strlen(name) > SHM_NAME_MAX)
        return -1;

    uint64_t flags = spin_lock_irq(&lock);
    int found = -1;

    for (size_t i = 0; i < SHM_MAX; i++) {
        if (segments[i].used && segments[i].named &&
            strcmp(segments[i].name, name) == 0) {
            found = (int)i;
            break;
        }
    }

    if (found >= 0 || !create) {
        spin_unlock_irq(&lock, flags);
        return found;
    }

    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;

    if (!pages || pages > SHM_PAGES_MAX) {
        spin_unlock_irq(&lock, flags);
        return -1;
    }

    for (size_t i = 0; i < SHM_MAX && found < 0; i++)
        if (!segments[i].used)
            found = (int)i;

    if (found < 0) {
        spin_unlock_irq(&lock, flags);
        return -1;
    }

    struct shm_segment *s = &segments[found];

    memset(s, 0, sizeof(*s));
    s->used  = true;
    s->named = true;
    s->pages = pages;
    s->uid   = session_uid();
    strlcpy(s->name, name, sizeof(s->name));

    spin_unlock_irq(&lock, flags);

    /* Die Seiten ausserhalb der Sperre holen: pmm_alloc_page nimmt
     * seine eigene, und zwei Sperren ineinander sind der kuerzeste Weg
     * in eine Verklemmung. */
    for (size_t i = 0; i < pages; i++) {
        s->phys[i] = pmm_alloc_page();
        if (!s->phys[i]) {
            free_segment(s);
            return -1;
        }
        memset(phys_to_virt(s->phys[i]), 0, PAGE_SIZE);
    }

    log_info("ipc", "Bereich \"%s\" angelegt, %u Seiten", s->name,
             (unsigned)pages);
    return found;
}

uint64_t shm_attach(struct address_space *space, int id)
{
    if (!space || id < 0 || id >= SHM_MAX || !segments[id].used)
        return 0;

    struct shm_segment *s = &segments[id];

    /* Jeder Bereich hat seinen festen Platz im Adressraum. Das spart
     * die Verwaltung freier Loecher und macht die Adresse in beiden
     * Programmen dieselbe - Zeiger darin lassen sich damit sogar
     * weiterreichen. */
    uint64_t base = SHM_BASE + (uint64_t)id * SHM_STRIDE;

    for (size_t i = 0; i < s->pages; i++) {
        if (!vmm_map(space, base + i * PAGE_SIZE, s->phys[i],
                     PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX |
                     PTE_SHARED)) {
            for (size_t k = 0; k < i; k++)
                vmm_unmap(space, base + k * PAGE_SIZE);
            return 0;
        }
    }

    uint64_t flags = spin_lock_irq(&lock);

    s->attached++;
    spin_unlock_irq(&lock, flags);
    return base;
}

void shm_detach(struct address_space *space, int id)
{
    if (id < 0 || id >= SHM_MAX || !segments[id].used)
        return;

    struct shm_segment *s = &segments[id];
    uint64_t base = SHM_BASE + (uint64_t)id * SHM_STRIDE;

    if (space)
        for (size_t i = 0; i < s->pages; i++)
            vmm_unmap(space, base + i * PAGE_SIZE);

    uint64_t flags = spin_lock_irq(&lock);
    bool last = false;

    if (s->attached)
        s->attached--;
    /* Wie bei einer geloeschten, aber noch offenen Datei: Der Name ist
     * weg, der Inhalt lebt, bis der Letzte ihn loslaesst. */
    last = !s->attached && !s->named;
    spin_unlock_irq(&lock, flags);

    if (last)
        free_segment(s);
}

bool shm_unlink(const char *name)
{
    uint64_t flags = spin_lock_irq(&lock);
    struct shm_segment *found = NULL;

    for (size_t i = 0; i < SHM_MAX && !found; i++)
        if (segments[i].used && segments[i].named &&
            strcmp(segments[i].name, name) == 0)
            found = &segments[i];

    if (!found) {
        spin_unlock_irq(&lock, flags);
        return false;
    }

    found->named = false;

    bool last = found->attached == 0;

    spin_unlock_irq(&lock, flags);

    if (last)
        free_segment(found);
    return true;
}

size_t shm_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < SHM_MAX; i++)
        if (segments[i].used)
            n++;
    return n;
}

const char *shm_name(int id)
{
    if (id < 0 || id >= SHM_MAX || !segments[id].used)
        return "";
    return segments[id].named ? segments[id].name : "(ohne Namen)";
}

size_t   shm_bytes(int id)
{
    return (id >= 0 && id < SHM_MAX && segments[id].used)
         ? segments[id].pages * PAGE_SIZE : 0;
}

uint32_t shm_users(int id)
{
    return (id >= 0 && id < SHM_MAX && segments[id].used)
         ? segments[id].attached : 0;
}

uint32_t shm_owner(int id)
{
    return (id >= 0 && id < SHM_MAX && segments[id].used)
         ? segments[id].uid : 0;
}
