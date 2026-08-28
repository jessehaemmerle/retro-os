/* mm.h - Speicherverwaltung: Seitenrahmen und Kernel-Heap. */
#ifndef MM_H
#define MM_H

#include "retro.h"

/* --- physischer Speicher (Bitmap-Allokator) --- */
void     pmm_init(void);
uint64_t pmm_alloc_page(void);              /* 0 = kein Speicher mehr */
uint64_t pmm_alloc_pages(size_t count);
void     pmm_free_page(uint64_t phys);
void     pmm_free_pages(uint64_t phys, size_t count);

/* Eine Seite kann mehreren Adressraeumen zugleich gehoeren - beim
 * Abspalten eines Prozesses etwa. Deshalb zaehlt die Verwaltung mit,
 * wie viele Besitzer eine Seite hat: pmm_share_page zaehlt hoch,
 * pmm_free_page zaehlt herunter und gibt erst beim letzten wirklich
 * frei. */
void     pmm_share_page(uint64_t phys);
bool     pmm_shared(uint64_t phys);         /* mehr als ein Besitzer? */
uint64_t pmm_shared_bytes(void);            /* mehrfach genutzter Speicher */

uint64_t pmm_total_bytes(void);
uint64_t pmm_used_bytes(void);
uint64_t pmm_free_bytes(void);

/* --- Kernel-Heap --- */
void  heap_init(void);
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

uint64_t heap_used_bytes(void);
uint64_t heap_total_bytes(void);

/* Kopie einer Zeichenkette auf dem Heap. */
char *kstrdup(const char *s);

#endif /* MM_H */
