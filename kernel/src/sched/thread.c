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
 * Seit RetroOS mehrere Kerne benutzt, hat jeder von ihnen seinen eigenen
 * laufenden Thread, seine eigene Zeitscheibe und seinen eigenen
 * Leerlauffaden; die Liste der Threads dagegen ist gemeinsam und steht
 * unter einer Sperre.
 *
 * Beim Umschalten wird die Sperre nicht vom Wechselnden freigegeben,
 * sondern von dem, der als naechstes darauf zurueckkommt.
 *
 * Ausserdem traegt jeder Thread ein Merkmal, ob er noch auf seinem
 * Stapel steht. Wer sich schlafen legt, aendert seinen Zustand, bevor
 * er tatsaechlich umschaltet - dazwischen duerfte ihn kein anderer Kern
 * fortsetzen, sonst liefen zwei auf demselben Stapel. Das Merkmal
 * loescht erst der, der danach auf demselben Kern weitermacht.
 */

#include "thread.h"
#include "arch.h"
#include "cpu.h"
#include "spinlock.h"
#include "io.h"
#include "kstring.h"
#include "mm.h"
#include "process.h"
#include "syscall.h"
#include "vmm.h"

#define TIME_SLICE_MS 20

void context_switch(uint64_t *save_rsp, uint64_t load_rsp);

void sched_release_after_switch(void);
static void wake_one_locked(void *object);
static void wake_all_locked(void *object);
void context_start(void);

static struct thread  threads[THREAD_MAX];
static uint32_t       next_id = 1;
static bool           started;

/* Schuetzt die Tabelle und alles, was der Scheduler daran aendert. */
static struct spinlock sched_lock = SPINLOCK_INIT("scheduler");

/* Wo der naechste Durchlauf zu suchen anfaengt - je Kern eine andere
 * Stelle, damit nicht alle ueber denselben Thread herfallen. */
static uint32_t rotate[CPU_MAX];

/* Wird aus context_start heraus gerufen: Ein frisch erzeugter Thread
 * kommt nicht aus schedule() zurueck und muss die Sperre, die der
 * Wechselnde hielt, selbst freigeben. */
void sched_release_after_switch(void)
{
    struct cpu *self = cpu_current();

    /* Der Thread, den dieser Kern eben verlassen hat, steht jetzt
     * wirklich nicht mehr auf seinem Stapel. */
    if (self->departed) {
        self->departed->on_cpu = false;
        self->departed = NULL;
    }

    spin_unlock(&sched_lock);
}

/* --- Hilfsfunktionen ------------------------------------------------- */

static struct thread *alloc_slot(void)
{
    /* Ein noch nie benutzter Platz hat keine Nummer. */
    for (size_t i = 0; i < THREAD_MAX; i++) {
        if (threads[i].id == 0)
            return &threads[i];
    }

    /* Sonst den Platz eines beendeten Threads wiederverwenden - aber
     * erst, wenn er seinen Stapel auch verlassen hat. Zwischen
     * "beendet" und dem letzten Umschalten laeuft er noch darauf. */
    for (size_t i = 0; i < THREAD_MAX; i++) {
        if (threads[i].state == THREAD_DEAD && threads[i].reapable) {
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

/* Waehlt den naechsten Thread: kleinste Zahl gewinnt, sonst reihum.
 * Was gerade auf einem anderen Kern laeuft, bleibt liegen. */
static struct thread *pick_next(struct cpu *self)
{
    struct thread *best = NULL;
    uint32_t start = rotate[self->index];
    uint32_t chosen = start;

    for (uint32_t k = 0; k < THREAD_MAX; k++) {
        uint32_t i = (start + k) % THREAD_MAX;
        struct thread *t = &threads[i];

        if (t->id == 0)
            continue;
        if (t->state == THREAD_RUNNING && t != self->current)
            continue;                 /* laeuft schon woanders */
        if (t->on_cpu && t != self->current)
            continue;                 /* noch nicht vom Stapel herunter */
        if (t->cpu_pin >= 0 && (uint32_t)t->cpu_pin != self->index)
            continue;                 /* gehoert einem anderen Kern */
        if (!runnable(t))
            continue;

        if (!best || t->priority < best->priority) {
            best = t;
            chosen = i + 1;
        }
    }

    rotate[self->index] = chosen % THREAD_MAX;
    return best ? best : self->idle;
}

/* --- Umschalten ------------------------------------------------------ */

void schedule(void)
{
    uint64_t flags = irq_save();
    struct cpu *self = cpu_current();

    if (self->preempt_depth > 0) {
        /* Ein geschuetzter Abschnitt laeuft - spaeter noch einmal. */
        irq_restore(flags);
        return;
    }

    self->resched_wanted = false;

    spin_lock(&sched_lock);

    struct thread *previous = self->current;
    struct thread *next = pick_next(self);

    if (next == previous) {
        self->slice_remaining = TIME_SLICE_MS;
        spin_unlock(&sched_lock);
        irq_restore(flags);
        return;
    }

    if (previous && previous->state == THREAD_RUNNING)
        previous->state = THREAD_READY;

    /* Ist der bisherige Thread beendet, wird sein Platz erst nach
     * diesem Wechsel frei - danach steht niemand mehr auf seinem
     * Stapel. Die Sperre gibt erst der naechste frei, deshalb kann
     * niemand dazwischenkommen. */
    if (previous && previous->state == THREAD_DEAD)
        previous->reapable = true;

    next->state = THREAD_RUNNING;
    next->on_cpu = true;
    next->cpu_ticks++;
    next->last_cpu = self->index;
    self->current = next;
    self->departed = previous;
    self->slice_remaining = TIME_SLICE_MS;
    self->switches++;

    /* Der neue Thread bringt seinen eigenen Adressraum mit; ausserdem
     * muessen CPU und Systemaufruf-Einsprung wissen, wohin sie
     * zurueckspringen, wenn aus Ring 3 etwas hereinkommt. */
    /* GS muss auf den Bereich dieses Kerns zeigen - der Thread kann
     * von einem anderen kommen. */
    syscall_bind_cpu();

    if (next->kernel_stack_top) {
        tss_set_kernel_stack(next->kernel_stack_top);
        syscall_set_kernel_stack(next->kernel_stack_top);
    }

    uint64_t wanted = next->process ? next->process->space.pml4_phys
                                    : vmm_kernel_pml4();
    uint64_t active;

    __asm__ volatile("mov %%cr3, %0" : "=r"(active));
    if (wanted && (wanted & ~0xFFFull) != (active & ~0xFFFull))
        __asm__ volatile("mov %0, %%cr3" :: "r"(wanted) : "memory");

    /* Ab hier laeuft ein anderer Thread; die Rueckkehr erfolgt erst,
     * wenn previous wieder an der Reihe ist. Die Sperre gibt der frei,
     * der als naechstes hier herauskommt. */
    context_switch(&previous->rsp, next->rsp);

    /* Ab hier laeuft wieder ein anderer Thread - moeglicherweise auf
     * einem anderen Kern als vorhin. */
    sched_release_after_switch();
    irq_restore(flags);
}

void scheduler_tick(void)
{
    struct cpu *self = cpu_current();

    self->ticks++;

    if (!started)
        return;

    if (--self->slice_remaining <= 0)
        self->resched_wanted = true;

    /* Schlafende Threads, deren Zeit um ist, wieder wecken. Das macht
     * nur der Bootkern - einmal je Takt genuegt, und es spart den
     * uebrigen Kernen den Zugriff auf die gemeinsame Liste. */
    if (self->index != 0)
        return;

    if (!spin_held(&sched_lock)) {
        spin_lock(&sched_lock);
        for (size_t i = 0; i < THREAD_MAX; i++) {
            struct thread *t = &threads[i];

            if (t->id == 0)
                continue;
            if ((t->state == THREAD_SLEEPING || t->state == THREAD_BLOCKED) &&
                t->wake_at_ms && timer_ms() >= t->wake_at_ms) {
                t->state = THREAD_READY;
                t->wait_object = NULL;
                t->wake_at_ms = 0;
                self->resched_wanted = true;
            }
        }
        spin_unlock(&sched_lock);
    }
}

bool scheduler_should_switch(void)
{
    struct cpu *self = cpu_current();

    return started && self->resched_wanted && self->preempt_depth == 0;
}

void preempt_disable(void)
{
    uint64_t flags = irq_save();

    cpu_current()->preempt_depth++;
    irq_restore(flags);
}

bool preempt_blocked(void)
{
    return cpu_current()->preempt_depth > 0;
}

void preempt_enable(void)
{
    uint64_t flags = irq_save();
    struct cpu *self = cpu_current();
    bool due = false;

    if (self->preempt_depth > 0 && --self->preempt_depth == 0 &&
        self->resched_wanted)
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

/* Die Ringliste ist mit mehreren Kernen entfallen - der Scheduler geht
 * die Tabelle durch und faengt bei jedem Kern woanders an. Der Zeiger
 * bleibt nur, damit alter Code, der ihn liest, nichts Falsches sieht. */
static void link_thread(struct thread *t)
{
    t->next = NULL;
}

static struct thread *create_thread(const char *name, thread_entry_t entry,
                                    void *argument,
                                    enum thread_priority priority,
                                    int32_t pin, uint8_t exact_priority)
{
    /* Der Stapel wird vor der Sperre geholt: pmm_alloc_pages hat seine
     * eigene und die beiden ineinander zu schachteln waere eine
     * Verklemmung in Wartestellung. */
    uint64_t stack = pmm_alloc_pages(THREAD_STACK_SIZE / PAGE_SIZE);

    if (!stack)
        return NULL;

    uint64_t flags = spin_lock_irq(&sched_lock);
    struct thread *t = alloc_slot();

    if (!t) {
        spin_unlock_irq(&sched_lock, flags);
        pmm_free_pages(stack, THREAD_STACK_SIZE / PAGE_SIZE);
        return NULL;
    }

    memset(t, 0, sizeof(*t));
    t->id = next_id++;
    strlcpy(t->name, name, sizeof(t->name));
    t->priority   = (uint8_t)priority;
    t->state      = THREAD_READY;
    t->stack_base = stack;
    t->cpu_pin    = pin;
    if (exact_priority)
        t->priority = exact_priority;

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
    spin_unlock_irq(&sched_lock, flags);
    return t;
}

struct thread *thread_create(const char *name, thread_entry_t entry,
                             void *argument, enum thread_priority priority)
{
    return create_thread(name, entry, argument, priority, -1, 0);
}

NORETURN void thread_exit(void)
{
    uint64_t flags = spin_lock_irq(&sched_lock);
    struct thread *self = cpu_current()->current;

    self->state = THREAD_DEAD;
    wake_all_locked(self);
    spin_unlock_irq(&sched_lock, flags);

    schedule();

    /* Wird nie erreicht. */
    for (;;)
        hlt();
}

struct thread *thread_current(void)
{
    return cpu_current()->current;
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
        panic("thread_sleep() im geschuetzten Abschnitt (%s)",
              cpu_current()->current->name);

    uint64_t flags = spin_lock_irq(&sched_lock);
    struct thread *self = cpu_current()->current;

    self->wake_at_ms = timer_ms() + milliseconds;
    self->state = THREAD_SLEEPING;
    spin_unlock_irq(&sched_lock, flags);

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
        uint64_t flags = spin_lock_irq(&sched_lock);

        if (!m->locked) {
            m->locked = true;
            m->owner  = cpu_current()->current;
            spin_unlock_irq(&sched_lock, flags);
            return;
        }

        if (m->owner == cpu_current()->current) {
            /* Selbstblockade waere ein Programmfehler. */
            spin_unlock_irq(&sched_lock, flags);
            panic("mutex_lock(%s): Sperre bereits vom selben Thread gehalten",
                  m->name ? m->name : "?");
        }

        struct thread *self = cpu_current()->current;

        self->wait_object = m;
        self->state = THREAD_BLOCKED;
        self->wake_at_ms = 0;
        spin_unlock_irq(&sched_lock, flags);

        schedule();
    }
}

void mutex_unlock(struct mutex *m)
{
    uint64_t flags = spin_lock_irq(&sched_lock);

    m->locked = false;
    m->owner  = NULL;
    wake_one_locked(m);
    spin_unlock_irq(&sched_lock, flags);
}

bool mutex_held(const struct mutex *m)
{
    return m->locked && m->owner == cpu_current()->current;
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
        panic("wait_on() im geschuetzten Abschnitt (%s)",
              cpu_current()->current->name);

    uint64_t flags = spin_lock_irq(&sched_lock);
    struct thread *self = cpu_current()->current;

    self->wait_object = object;
    self->state = THREAD_BLOCKED;
    self->wake_at_ms = timeout_ms ? timer_ms() + timeout_ms : 0;
    spin_unlock_irq(&sched_lock, flags);

    if (release)
        mutex_unlock(release);

    schedule();

    if (release)
        mutex_lock(release);
}

/* Die inneren Fassungen setzen voraus, dass die Sperre schon gehalten
 * wird - so lassen sie sich aus Stellen rufen, die sie ohnehin haben. */
static void wake_one_locked(void *object)
{
    for (size_t i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];

        if (t->id != 0 && t->state == THREAD_BLOCKED &&
            t->wait_object == object) {
            t->state = THREAD_READY;
            t->wait_object = NULL;
            t->wake_at_ms = 0;
            cpu_current()->resched_wanted = true;
            break;
        }
    }
}

void wake_one(void *object)
{
    uint64_t flags = spin_lock_irq(&sched_lock);

    wake_one_locked(object);
    spin_unlock_irq(&sched_lock, flags);
}

static void wake_all_locked(void *object)
{
    for (size_t i = 0; i < THREAD_MAX; i++) {
        struct thread *t = &threads[i];

        if (t->id != 0 && t->state == THREAD_BLOCKED &&
            t->wait_object == object) {
            t->state = THREAD_READY;
            t->wait_object = NULL;
            t->wake_at_ms = 0;
            cpu_current()->resched_wanted = true;
        }
    }
}

void wake_all(void *object)
{
    uint64_t flags = spin_lock_irq(&sched_lock);

    wake_all_locked(object);
    spin_unlock_irq(&sched_lock, flags);
}

/* --- Leerlauf -------------------------------------------------------- */

static void idle_entry(void *argument)
{
    UNUSED(argument);

    for (;;) {
        __asm__ volatile("sti; hlt");
    }
}

/* Jeder Kern braucht seinen eigenen Leerlauffaden - sonst wuerden sich
 * zwei um denselben streiten, und einer bliebe ohne. */
static struct thread *make_idle(struct cpu *self)
{
    char name[16];

    ksnprintf(name, sizeof(name), "leerlauf%u", (unsigned)self->index);

    /* Bindung und Wichtigkeit muessen schon beim Anlegen stehen: Waere
     * der Faden auch nur einen Augenblick ungebunden und lauffaehig,
     * koennte ein anderer Kern ihn sich nehmen - und dann liefe
     * derselbe Thread auf zweien. */
    return create_thread(name, idle_entry, NULL, PRIO_LOW,
                         (int32_t)self->index, 255);
}

void thread_init(void)
{
    struct cpu *self = cpu_current();

    memset(threads, 0, sizeof(threads));

    /* Der bereits laufende Code wird zum ersten Thread. */
    struct thread *main_thread = &threads[0];

    main_thread->id = next_id++;
    strlcpy(main_thread->name, "kernel", sizeof(main_thread->name));
    main_thread->state = THREAD_RUNNING;
    main_thread->priority = PRIO_NORMAL;
    main_thread->cpu_pin = -1;
    main_thread->next = NULL;

    main_thread->on_cpu = true;
    self->current = main_thread;
    self->slice_remaining = TIME_SLICE_MS;
    started = true;

    self->idle = make_idle(self);
    if (!self->idle)
        panic("Der Leerlauf-Thread laesst sich nicht anlegen");

    kprintf("Scheduler   : bereit, Zeitscheibe %u ms\n", TIME_SLICE_MS);
}

/* Einstieg eines weiteren Kerns: Er bekommt einen Leerlauffaden als
 * ersten Thread und faellt von da an in den gewoehnlichen Wechsel. */
NORETURN void thread_enter_ap(void)
{
    struct cpu *self = cpu_current();

    self->idle = make_idle(self);
    if (!self->idle) {
        for (;;)
            __asm__ volatile("hlt");
    }

    uint64_t flags = spin_lock_irq(&sched_lock);

    self->idle->state = THREAD_RUNNING;
    self->idle->on_cpu = true;
    self->current = self->idle;
    self->slice_remaining = TIME_SLICE_MS;
    spin_unlock_irq(&sched_lock, flags);

    syscall_init_ap();
    timer_init_ap();

    /* Auf den eigenen Stapel wechseln. Bis hierher laeuft der Kern auf
     * dem, den der Bootloader mitgegeben hat - der ist klein und
     * gehoert uns nicht. Die Sperre gibt context_start frei. */
    uint64_t discard = 0;

    spin_lock(&sched_lock);
    context_switch(&discard, self->idle->rsp);

    __builtin_unreachable();
}
