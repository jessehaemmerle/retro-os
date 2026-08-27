/* thread.h - Kernel-Threads, Scheduler und Synchronisation.
 *
 * RetroOS schaltet die laufenden Threads im Timer-Interrupt um. Ein Thread
 * muss also nichts von den anderen wissen und darf beliebig lange rechnen,
 * ohne dass die Oberflaeche stehenbleibt.
 */
#ifndef THREAD_H
#define THREAD_H

#include "retro.h"

#define THREAD_NAME_MAX   31
#define THREAD_STACK_SIZE (64 * 1024)
#define THREAD_MAX        32

/* Je kleiner die Zahl, desto wichtiger der Thread. */
enum thread_priority {
    PRIO_HIGH   = 0,   /* Oberflaeche - muss immer fluessig bleiben */
    PRIO_NORMAL = 1,
    PRIO_LOW    = 2,
};

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_BLOCKED,
    THREAD_DEAD,
};

struct process;

struct thread {
    uint64_t  rsp;               /* gesicherter Stapelzeiger (muss zuerst kommen) */
    uint64_t  kernel_stack_top;  /* fuer den Ring-Wechsel bei Systemaufrufen */

    uint32_t  id;
    char      name[THREAD_NAME_MAX + 1];
    uint8_t   state;
    uint8_t   priority;

    uint64_t  stack_base;
    uint64_t  wake_at_ms;
    void     *wait_object;

    uint64_t  cpu_ticks;         /* wie oft dieser Thread lief */
    int32_t   cpu_pin;           /* fest an einen Kern, -1 = beliebig */
    bool      reapable;          /* beendet und von seinem Stapel herunter */
    volatile bool on_cpu;        /* laeuft noch auf seinem Stapel          */
    uint32_t  last_cpu;          /* wo er zuletzt lief                */

    struct process *process;     /* NULL = reiner Kernel-Thread */
    struct thread  *next;
};

typedef void (*thread_entry_t)(void *argument);

void thread_init(void);
struct thread *thread_create(const char *name, thread_entry_t entry,
                             void *argument, enum thread_priority priority);
void thread_exit(void) NORETURN;
/* Einstieg eines weiteren Kerns in den Scheduler. */
NORETURN void thread_enter_ap(void);

struct thread *thread_current(void);
void thread_yield(void);
void thread_sleep(uint32_t milliseconds);

/* Wird aus dem Timer-Interrupt aufgerufen. */
void scheduler_tick(void);
bool scheduler_running(void);
/* Uebergibt die Kontrolle an den Scheduler; kehrt zurueck, wenn dieser
 * Thread wieder an der Reihe ist. */
void schedule(void);

size_t thread_count(void);
struct thread *thread_at(size_t index);

/* --- Sperren --------------------------------------------------------- */

struct mutex {
    volatile bool   locked;
    struct thread  *owner;
    const char     *name;
};

void mutex_init(struct mutex *m, const char *name);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
bool mutex_held(const struct mutex *m);

/* --- Warteschlangen -------------------------------------------------- */

/* Blockiert den aufrufenden Thread, bis jemand auf dasselbe Objekt weckt.
 * Wird eine Sperre uebergeben, wird sie waehrend des Wartens freigegeben. */
void wait_on(void *object, struct mutex *release, uint32_t timeout_ms);
void wake_one(void *object);
void wake_all(void *object);

/* --- Umschaltsperre ---------------------------------------------------
 *
 * Kurze Abschnitte, die nicht unterbrochen werden duerfen, schuetzt man
 * am einfachsten damit: der Timer darf weiterlaufen, es wird nur nicht
 * umgeschaltet. Anders als eine Sperre kann das nicht verklemmen, weil
 * niemand darauf wartet. */
void preempt_disable(void);
void preempt_enable(void);
bool preempt_blocked(void);

/* --- Kurzzeitige Unterbrechungssperre -------------------------------- */

static inline uint64_t irq_save(void)
{
    uint64_t flags;

    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    if (flags & (1 << 9))
        __asm__ volatile("sti" ::: "memory");
}

#endif /* THREAD_H */
