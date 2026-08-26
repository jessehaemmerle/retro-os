/* retro.h - gemeinsame Basisdefinitionen des RetroOS-Kernels. */
#ifndef RETRO_H
#define RETRO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PACKED      __attribute__((packed))
#define ALIGNED(n)  __attribute__((aligned(n)))
#define NORETURN    __attribute__((noreturn))
#define UNUSED(x)   ((void)(x))

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)    ((a) < (b) ? (a) : (b))
#define MAX(a, b)    ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) MIN(MAX((v), (lo)), (hi))

#define PAGE_SIZE 4096ULL
#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))

/* Physischer Speicher ist ueber das Higher-Half-Direct-Mapping erreichbar. */
extern uint64_t g_hhdm_offset;

static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(phys + g_hhdm_offset);
}

/* Nur fuer Zeiger gueltig, die aus phys_to_virt() stammen - also alles,
 * was ueber PMM und Heap vergeben wurde. Geraete mit DMA brauchen das. */
static inline uint64_t virt_to_phys(const void *virt)
{
    return (uint64_t)virt - g_hhdm_offset;
}

/* Ausgaben auf die serielle Debug-Konsole. */
void kprintf(const char *fmt, ...);
int  ksnprintf(char *buf, size_t size, const char *fmt, ...);

NORETURN void panic(const char *fmt, ...);

#endif /* RETRO_H */
