/* thread.c - Scheduler und Synchronisation.
 *
 * Die Threads liegen in einer Ringliste. Der Timer-Interrupt zaehlt die
 * Zeitscheibe des laufenden Threads herunter; ist sie aufgebraucht, wird
 * beim Verlassen des Interrupts umgeschaltet. Ein Thread kann ausserdem
 * jederzeit freiwillig abgeben (thread_yield), schlafen (thread_sleep) oder
 * auf ein Ereignis warten (wait_on).
 *
 * Ausgewaehlt wird nach Wichtigkeit: unter allen lauffaehigen Threads
 * gewinnt der mit der kleinsten Zahl, bei Gleichstand geht es reihum. So
 * bleibt die Oberflaeche fluessig, auch wenn im Hintergrund eine Seite
 * geladen wird.
 *
 * RetroOS laeuft auf einem Kern; deshalb genuegt es, fuer kurze kritische
 * Abschnitte die Interrupts zu sperren. Echte Sperren gibt es trotzdem -
 * sie schuetzen laengere Abschnitte gegen andere Threads.
 */

#include "thread.h"
#include "arch.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"
#include "process.h"
#include "syscall.h"
#include "vmm.h"

#define TIME_SLICE_MS 20

void context_switch(uint64_t *save_rsp, uint64_t load_rsp);
void context_start(void);

static struct thread  threads[THREAD_MAX];
static struct thread *current;
static struct thread *idle_thread;
static uint32_t       next_id = 1;
static bool           started;
static volatile int32_t slice_remaining;
static volatile bool  resched_wanted;
static volatile int32_t preempt_depth;
static uint64_t         active_pml4;

/* --- Hilfsfunktionen ------------------------------------------------- */

static struct thread *alloc_slot(void)
{
    /* Ein noch nie benutzter Platz hat keine Nummer. */
    for (size_t i = 0; i < THREAD_MAX; i++) {
        if (threads[i].id == 0)
            return &threads[i];
    }

    /* Sonst den Platz eines beendeten Threads wiederverwenden. */
    for (size_t i = 0; i < THREAD_MAX; i++) {
        if (threads[i].state == THREAD_DEAD) {
            if (threads[i].stack_base) {
                pmm_free_pages(threads[i].stack_base,
                               THREAD_STACK_SIZE / PAGE_SIZE);
                threads[i].stack_base = 0;
            }
            return &threads[i];
        }
    }
    return NULL;
}

static bool runnable(struct thread *t)
{
    if (t->state == THREAD_READY || t->state == THREAD_RUNNING)
        return true;

    if (t->state == THREAD_SLEEPING && timer_ms() >= t->wake_at_ms) {
        t->state = THREAD_READY;
        return true;
    }

    /* Auch Wartende koennen eine Frist haben. */
    if (t->state == THREAD_BLOCKED && t->wake_at_ms &&
        timer_ms() >= t->wake_at_ms) {
        t->state = THREAD_READY;
        t->wait_object = NULL;
        t->wake_at_ms = 0;
        return true;
    }
    return false;
}

/* Waehlt den naechsten Thread: kleinste Zahl gewinnt, sonst reihum. */
static struct thread *pick_next(void)
{
    struct thread *best = NULL;
    struct thread *scan = current ? current->next : &threads[0];

    for (size_t i = 0; i < THREAD_MAX; i++) {
        if (!scan)
            scan = &threads[0];

        if (scan->id != 0 && runnable(scan)) {
            if (!best || scan->priority < best->priority)
                best = scan;
        }
        scan = scan->next;
    }

    return best ? best : idle_thread;
}

/* --- Umschalten ------------------------------------------------------ */

void schedule(void)
{
    uint64_t flags = irq_save();

    if (preempt_depth > 0) {
        /* Ein geschuetzter Abschnitt laeuft - spaeter noch einmal. */
        irq_restore(flags);
        return;
    }

    resched_wanted = false;

    struct thread *previous = current;
    struct thread *next = pick_next();

    if (next == previous) {
        slice_remaining = TIME_SLICE_MS;
        irq_restore(flags);
        return;
    }

    if (previous && previous->state == THREAD_RUNNING)
        previous->state = THREAD_READY;

    next->state = THREAD_RUNNING;
    next->cpu_ticks++;
    current = next;
    slice_remaining = TIME_SLICE_MS;

    /* Der neue Thread bringt seinen eigenen Adressraum mit; ausserdem
     * muessen CPU und Systemaufruf-Einsprung wissen, wohin sie
     * zurueckspringen, wenn aus Ring 3 etwas hereinkommt. */
    if (next->kernel_stack_top) {
        tss_set_kernel_stack(next->kernel_stack_top);
        syscall_set_kernel_stack(next->kernel_stack_top);
    }

    uint64_t wanted = next->process ? next->process->space.pml4_phys
                                    : vmm_kernel_pml4();
    if (wanted && wanted != active_pml4) {
        __asm__ volatile("mov %0, %%cr3" :: "r"(wanted) : "memory");
        active_pml4 = wanted;
    }

    /* Ab hier laeuft ein anderer Thread; die Rueckkehr erfolgt erst,
     * wenn previous wieder an der Reihe ist. */
    context_switch(&previous->rsp, next->rsp);

    irq_restore(flags);
}

void scheduler_tick(void)
{
    if (!started)
        return;

    if (--slice_remaining <= 0)
        resched_wanted = true;

    /* Schlafende Threads, deren Zeit um ist, wieder wecken. */
    for (size_t i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];

        if (t->id == 0)
            continue;
        if ((t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED) &&
            t->wake_at_ms && timer_ms() >= t->wake_at_ms) {
            t->state = THREAD_READY;
            t->wait_object = NULL;
            t->wake_at_ms = 0;
            resched_wanted = true;
        }
    }
}

bool scheduler_should_switch(void)
{
    return started && resched_wanted && preempt_depth == 0;
}

void preempt_disable(void)
{
    uint64_t flags = irq_save();

    preempt_depth++;
    irq_restore(flags);
}

bool preempt_blocked(void)
{
    return preempt_depth > 0;
}

void preempt_enable(void)
{
    uint64_t flags = irq_save();
    bool due = false;

    if (preempt_depth > 0 && --preempt_depth == 0 && resched_wanted)
        due = true;
    irq_restore(flags);

    if (due)
        schedule();
}

bool scheduler_running(void)
{
    return started;
}

/* --- Threads erzeugen ------------------------------------------------ */

static void link_thread(struct thread *t)
{
    /* In die Ringliste einhaengen. */
    if (!current) {
        t->next = t;
        return;
    }

    t->next = current->next;
    current->next = t;
}

struct thread *thread_create(const char *name, thread_entry_t entry,
                             void *argument, enum thread_priority priority)
{
    uint64_t flags = irq_save();
    struct thread *t = alloc_slot();

    if (!t) {
        irq_restore(flags);
        return NULL;
    }

    uint64_t stack = pmm_alloc_pages(THREAD_STACK_SIZE / PAGE_SIZE);
    if (!stack) {
        irq_restore(flags);
        return NULL;
    }

    memset(t, 0, sizeof(*t));
    t->id = next_id++;
    strlcpy(t->name, name, sizeof(t->name));
    t->priority   = (uint8_t)priority;
    t->state      = THREAD_READY;
    t->stack_base = stack;

    uint8_t *top = (uint8_t *)phys_to_virt(stack) + THREAD_STACK_SIZE;
    t->kernel_stack_top = (uint64_t)top;

    /* Startstapel so vorbereiten, dass context_switch nach context_start
     * zurueckkehrt und dort Funktion und Argument vorfindet. */
    uint64_t *sp = (uint64_t *)top;

    *--sp = (uint64_t)context_start;   /* Ruecksprungadresse */
    *--sp = 0;                         /* rbp */
    *--sp = 0;                         /* rbx */
    *--sp = (uint64_t)entry;           /* r12 */
    *--sp = (uint64_t)argument;        /* r13 */
    *--sp = 0;                         /* r14 */
    *--sp = 0;                         /* r15 */

    t->rsp = (uint64_t)sp;

    link_thread(t);
    irq_restore(flags);
    return t;
}

NORETURN void thread_exit(void)
{
    uint64_t flags = irq_save();

    current->state = THREAD_DEAD;
    wake_all(current);
    irq_restore(flags);

    schedule();

    /* Wird nie erreicht. */
    for (;;)
        hlt();
}

struct thread *thread_current(void)
{
    return current;
}

void thread_yield(void)
{
    if (started)
        schedule();
}

void thread_sleep(uint32_t milliseconds)
{
    if (!started) {
        timer_sleep(milliseconds);
        return;
    }

    /* Wer die Umschaltung gesperrt hat, darf nicht schlafen: es koennte
     * niemand weitermachen. Das ist immer ein Programmfehler, und er faellt
     * sonst nur als raetselhaftes Stocken auf. */
    if (preempt_blocked())
        panic("thread_sleep() im geschuetzten Abschnitt (%s)", current->name);

    uint64_t flags = irq_save();

    current->wake_at_ms = timer_ms() + milliseconds;
    current->state = THREAD_SLEEPING;
    irq_restore(flags);

    schedule();
}

size_t thread_count(void)
{
    size_t n = 0;

    for (size_t i = 0; i < THREAD_MAX; i++) {
        if (threads[i].id != 0 && threads[i].state != THREAD_DEAD)
            n++;
    }
    return n;
}

struct thread *thread_at(size_t index)
{
    size_t n = 0;

    for (size_t i = 0; i < THREAD_MAX; i++) {
        if (threads[i].id == 0 || threads[i].state == THREAD_DEAD)
            continue;
        if (n++ == index)
            return &threads[i];
    }
    return NULL;
}

/* --- Sperren --------------------------------------------------------- */

void mutex_init(struct mutex *m, const char *name)
{
    m->locked = false;
    m->owner  = NULL;
    m->name   = name;
}

void mutex_lock(struct mutex *m)
{
    if (!started) {
        m->locked = true;
        return;
    }

    for (;;) {
        uint64_t flags = irq_save();

        if (!m->locked) {
            m->locked = true;
            m->owner  = current;
            irq_restore(flags);
            return;
        }

        if (m->owner == current) {
            /* Selbstblockade waere ein Programmfehler. */
            irq_restore(flags);
            panic("mutex_lock(%s): Sperre bereits vom selben Thread gehalten",
                  m->name ? m->name : "?");
        }

        current->wait_object = m;
        current->state = THREAD_BLOCKED;
        current->wake_at_ms = 0;
        irq_restore(flags);

        schedule();
    }
}

void mutex_unlock(struct mutex *m)
{
    uint64_t flags = irq_save();

    m->locked = false;
    m->owner  = NULL;
    irq_restore(flags);

    wake_one(m);
}

bool mutex_held(const struct mutex *m)
{
    return m->locked && m->owner == current;
}

/* --- Warten und Wecken ----------------------------------------------- */

void wait_on(void *object, struct mutex *release, uint32_t timeout_ms)
{
    if (!started) {
        if (timeout_ms)
            timer_sleep(timeout_ms);
        return;
    }

    if (preempt_blocked())
        panic("wait_on() im geschuetzten Abschnitt (%s)", current->name);

    uint64_t flags = irq_save();

    current->wait_object = object;
    current->state = THREAD_BLOCKED;
    current->wake_at_ms = timeout_ms ? timer_ms() + timeout_ms : 0;
    irq_restore(flags);

    if (release)
        mutex_unlock(release);

    schedule();

    if (release)
        mutex_lock(release);
}

void wake_one(void *object)
{
    uint64_t flags = irq_save();

    for (size_t i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];

        if (t->id != 0 && t->state == THREAD_BLOCKED &&
            t->wait_object == object) {
            t->state = THREAD_READY;
            t->wait_object = NULL;
            t->wake_at_ms = 0;
            resched_wanted = true;
            break;
        }
    }
    irq_restore(flags);
}

void wake_all(void *object)
{
    uint64_t flags = irq_save();

    for (size_t i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];

        if (t->id != 0 && t->state == THREAD_BLOCKED &&
            t->wait_object == object) {
            t->state = THREAD_READY;
            t->wait_object = NULL;
            t->wake_at_ms = 0;
            resched_wanted = true;
        }
    }
    irq_restore(flags);
}

/* --- Leerlauf -------------------------------------------------------- */

static void idle_entry(void *argument)
{
    UNUSED(argument);

    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

void thread_init(void)
{
    memset(threads, 0, sizeof(threads));
    active_pml4 = vmm_kernel_pml4();

    /* Der bereits laufende Code wird zum ersten Thread. */
    struct thread *main_thread = &threads[0];

    main_thread->id = next_id++;
    strlcpy(main_thread->name, "kernel", sizeof(main_thread->name));
    main_thread->state = THREAD_RUNNING;
    main_thread->priority = PRIO_NORMAL;
    main_thread->next = main_thread;

    current = main_thread;
    slice_remaining = TIME_SLICE_MS;
    started = true;

    idle_thread = thread_create("leerlauf", idle_entry, NULL, PRIO_LOW);
    if (!idle_thread)
        panic("Der Leerlauf-Thread laesst sich nicht anlegen");
    idle_thread->priority = 255;       /* nur, wenn sonst nichts laeuft */

    kprintf("Scheduler   : bereit, Zeitscheibe %u ms\n", TIME_SLICE_MS);
}
