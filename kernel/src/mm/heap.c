/* heap.c - Kernel-Heap mit einfacher Freispeicherliste.
 *
 * Der Heap besteht aus Bloecken, die in einer doppelt verketteten Liste
 * liegen. Bei Bedarf werden neue Seiten vom PMM nachgefordert. Benachbarte
 * freie Bloecke werden beim Freigeben wieder verschmolzen, damit der Heap
 * nicht mit der Zeit zerfaellt.
 */

#include "mm.h"
#include "spinlock.h"
#include "kstring.h"
#include "thread.h"

/* Ein Kern zur Zeit in der Freiliste. */
static struct spinlock heap_lock = SPINLOCK_INIT("heap");

#define HEAP_MAGIC     0x52455452u   /* "RETR" */
#define HEAP_GROW_MIN  (256 * 1024)  /* mindestens 256 KiB nachfordern */

struct block {
    uint32_t      magic;
    uint32_t      free;
    size_t        size;      /* Nutzbytes hinter dem Header */
    struct block *next;
    struct block *prev;
};

static struct block *head;
static uint64_t      total_bytes;
static uint64_t      used_bytes;

static struct block *grow(size_t need)
{
    size_t bytes = ALIGN_UP(need + sizeof(struct block), PAGE_SIZE);
    if (bytes < HEAP_GROW_MIN)
        bytes = HEAP_GROW_MIN;

    size_t   pages = bytes / PAGE_SIZE;
    uint64_t phys  = pmm_alloc_pages(pages);
    if (!phys)
        return NULL;

    struct block *b = phys_to_virt(phys);

    b->magic = HEAP_MAGIC;
    b->free  = 1;
    b->size  = bytes - sizeof(struct block);
    b->next  = NULL;
    b->prev  = NULL;

    if (!head) {
        head = b;
    } else {
        struct block *tail = head;
        while (tail->next)
            tail = tail->next;
        tail->next = b;
        b->prev    = tail;
    }

    total_bytes += bytes;
    return b;
}

void heap_init(void)
{
    head = NULL;
    total_bytes = used_bytes = 0;

    if (!grow(HEAP_GROW_MIN))
        panic("Heap laesst sich nicht anlegen");

    kprintf("Heap        : %u KiB bereit\n", (unsigned)(total_bytes / 1024));
}

/* Teilt einen Block, wenn genug Platz fuer einen zweiten uebrig bleibt. */
static void split(struct block *b, size_t size)
{
    if (b->size < size + sizeof(struct block) + 32)
        return;

    struct block *rest = (struct block *)((uint8_t *)(b + 1) + size);

    rest->magic = HEAP_MAGIC;
    rest->free  = 1;
    rest->size  = b->size - size - sizeof(struct block);
    rest->next  = b->next;
    rest->prev  = b;

    if (b->next)
        b->next->prev = rest;
    b->next = rest;
    b->size = size;
}

/* Der Heap wird von mehreren Threads benutzt; waehrend die Liste
 * umgehaengt wird, darf nicht umgeschaltet werden. */
void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    size = ALIGN_UP(size, 16);
    uint64_t __flags = spin_lock_irq(&heap_lock);

    for (struct block *b = head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split(b, size);
            b->free = 0;
            used_bytes += b->size;
            spin_unlock_irq(&heap_lock, __flags);
            return b + 1;
        }
    }

    struct block *b = grow(size);
    if (!b) {
        spin_unlock_irq(&heap_lock, __flags);
        return NULL;
    }

    split(b, size);
    b->free = 0;
    used_bytes += b->size;
    spin_unlock_irq(&heap_lock, __flags);
    return b + 1;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

/* Verschmilzt b mit dem Nachfolger, falls beide frei und benachbart sind. */
static void merge_forward(struct block *b)
{
    struct block *n = b->next;

    if (!n || !n->free)
        return;
    if ((uint8_t *)(b + 1) + b->size != (uint8_t *)n)
        return;   /* nicht zusammenhaengend - stammt aus einem anderen Wachstum */

    b->size += n->size + sizeof(struct block);
    b->next  = n->next;
    if (n->next)
        n->next->prev = b;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    struct block *b = (struct block *)ptr - 1;

    if (b->magic != HEAP_MAGIC)
        panic("kfree(): beschaedigter Heap-Block bei %p", ptr);

    uint64_t __flags = spin_lock_irq(&heap_lock);

    if (b->free) {
        spin_unlock_irq(&heap_lock, __flags);
        return;
    }

    b->free = 1;
    used_bytes -= b->size;

    merge_forward(b);
    if (b->prev && b->prev->free)
        merge_forward(b->prev);

    spin_unlock_irq(&heap_lock, __flags);
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct block *b = (struct block *)ptr - 1;
    if (b->magic != HEAP_MAGIC)
        panic("krealloc(): beschaedigter Heap-Block bei %p", ptr);

    if (b->size >= size)
        return ptr;

    void *neu = kmalloc(size);
    if (!neu)
        return NULL;

    memcpy(neu, ptr, b->size);
    kfree(ptr);
    return neu;
}

char *kstrdup(const char *s)
{
    if (!s)
        return NULL;

    size_t len = strlen(s);
    char  *copy = kmalloc(len + 1);

    if (copy)
        memcpy(copy, s, len + 1);
    return copy;
}

uint64_t heap_used_bytes(void)  { return used_bytes; }
uint64_t heap_total_bytes(void) { return total_bytes; }
