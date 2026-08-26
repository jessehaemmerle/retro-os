/* vmm.h - Verwaltung virtueller Adressraeume.
 *
 * Jeder Prozess bekommt eine eigene oberste Seitentabelle. Die obere
 * Haelfte (Kernel und die direkte Abbildung des physischen Speichers)
 * wird aus dem Kernel-Adressraum uebernommen und ist damit in jedem
 * Prozess gleich - so bleibt der Kernel auch nach einem Systemaufruf
 * erreichbar. Die untere Haelfte gehoert allein dem Prozess.
 */
#ifndef VMM_H
#define VMM_H

#include "retro.h"

#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITE     (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_HUGE      (1ULL << 7)
#define PTE_NX        (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Bereich, in dem Benutzerprogramme leben. */
#define USER_BASE        0x0000000000400000ULL
#define USER_STACK_TOP   0x0000700000000000ULL
#define USER_STACK_SIZE  (256 * 1024)
#define USER_HEAP_BASE   0x0000000010000000ULL

struct address_space {
    uint64_t pml4_phys;
    uint64_t heap_break;      /* Ende des Prozess-Heaps */
};

void vmm_init(void);

/* Legt einen Adressraum an, der die Kernel-Haelfte mitbenutzt. */
bool vmm_create(struct address_space *space);
void vmm_destroy(struct address_space *space);

/* Bildet eine virtuelle Seite auf eine physische ab. */
bool vmm_map(struct address_space *space, uint64_t virt, uint64_t phys,
             uint64_t flags);
/* Legt count Seiten frisch an und bildet sie ab (mit Null gefuellt). */
bool vmm_alloc_range(struct address_space *space, uint64_t virt, size_t bytes,
                     uint64_t flags);
void vmm_unmap(struct address_space *space, uint64_t virt);

/* Physische Adresse zu einer virtuellen im gegebenen Adressraum. */
uint64_t vmm_resolve(struct address_space *space, uint64_t virt);

void vmm_switch(const struct address_space *space);
void vmm_switch_kernel(void);
uint64_t vmm_kernel_pml4(void);

/* Kopiert zwischen Kernel und Benutzeradressraum, ohne umzuschalten. */
bool vmm_copy_to_user(struct address_space *space, uint64_t dst,
                      const void *src, size_t length);
bool vmm_copy_from_user(struct address_space *space, void *dst, uint64_t src,
                        size_t length);

#endif /* VMM_H */
