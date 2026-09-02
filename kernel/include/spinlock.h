/* spinlock.h - Sperren, die auch zwischen Kernen gelten.
 *
 * Solange nur ein Kern lief, genuegte preempt_disable(): Wer nicht
 * verdraengt werden kann, wird auch nicht gestoert. Mit mehreren Kernen
 * stimmt das nicht mehr - zwei koennen gleichzeitig in derselben Liste
 * herumraeumen. Dafuer braucht es eine echte Sperre.
 *
 * Es ist eine Warteschlangensperre: Jeder Wartende zieht eine Nummer
 * und ist dran, wenn sie aufgerufen wird. Das ist gerecht - eine
 * einfache Testsperre kann einen Kern beliebig lange hungern lassen,
 * waehrend andere sie immer wieder wegschnappen.
 *
 * Gehalten wird sie nur kurz und niemals ueber etwas, das schlafen
 * koennte: Wer mit gezogener Sperre schlaeft, laesst alle anderen
 * drehen.
 */
#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "retro.h"

struct spinlock {
    volatile uint32_t next;      /* naechste ausgegebene Nummer */
    volatile uint32_t serving;   /* Nummer, die gerade dran ist */
    const char       *name;
};

#define SPINLOCK_INIT(text) { 0, 0, (text) }

static inline void spin_init(struct spinlock *lock, const char *name)
{
    lock->next = 0;
    lock->serving = 0;
    lock->name = name;
}

static inline void spin_lock(struct spinlock *lock)
{
    uint32_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);

    while (__atomic_load_n(&lock->serving, __ATOMIC_ACQUIRE) != ticket)
        __builtin_ia32_pause();
}

static inline void spin_unlock(struct spinlock *lock)
{
    __atomic_store_n(&lock->serving, lock->serving + 1, __ATOMIC_RELEASE);
}

static inline bool spin_held(const struct spinlock *lock)
{
    return __atomic_load_n(&lock->serving, __ATOMIC_RELAXED) !=
           __atomic_load_n(&lock->next, __ATOMIC_RELAXED);
}

/* Mit abgeschalteten Unterbrechungen: Wird die Sperre auch aus einem
 * Interrupt heraus genommen, wuerde sich der Kern sonst selbst
 * blockieren. Der Rueckgabewert gehoert in spin_unlock_irq(). */
#ifdef RETRO_HOSTED

/* Auf dem Entwicklungsrechner laufen die Testsammlungen in Ring 3, und
 * dort ist cli verboten - der Versuch endet sofort. Unterbrechungen
 * gibt es dort ohnehin keine abzuschalten, also bleibt nur die Sperre
 * selbst uebrig. */
static inline uint64_t spin_lock_irq(struct spinlock *lock)
{
    spin_lock(lock);
    return 0;
}

static inline void spin_unlock_irq(struct spinlock *lock, uint64_t flags)
{
    UNUSED(flags);
    spin_unlock(lock);
}

#else

static inline uint64_t spin_lock_irq(struct spinlock *lock)
{
    uint64_t flags;

    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irq(struct spinlock *lock, uint64_t flags)
{
    spin_unlock(lock);
    if (flags & (1u << 9))
        __asm__ volatile("sti" ::: "memory");
}

#endif /* RETRO_HOSTED */

#endif /* SPINLOCK_H */
