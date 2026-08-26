/* shim.c - der Kernel-Kleinkram, den die Kryptodateien erwarten.
 * Auf dem Entwicklungsrechner wird er von der libc bedient. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t g_hhdm_offset;

void kprintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

void panic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fprintf(stderr, "PANIC: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);

    if (size > 0) {
        size_t copy = (len >= size) ? size - 1 : len;

        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

void memset32(void *dst, uint32_t value, size_t count)
{
    uint32_t *p = dst;

    while (count--)
        *p++ = value;
}
