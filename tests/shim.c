/* shim.c - der Kernel-Kleinkram, den die Kryptodateien erwarten.
 * Auf dem Entwicklungsrechner wird er von der libc bedient. */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

uint64_t g_hhdm_offset;

void kprintf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    return vsnprintf(buf, size, fmt, ap);
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

/* ------------------------------------------------------------------ */
/* Halde und Zeichenflaeche - fuer die Bildtests                        */
/* ------------------------------------------------------------------ */

void *kmalloc(size_t size)              { return malloc(size ? size : 1); }
void *kzalloc(size_t size)              { return calloc(1, size ? size : 1); }
void *krealloc(void *ptr, size_t size)  { return realloc(ptr, size ? size : 1); }
void  kfree(void *ptr)                  { free(ptr); }

struct rect { int32_t x, y, w, h; };

static int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }

struct rect rect_intersect(struct rect a, struct rect b)
{
    int32_t x0 = imax(a.x, b.x), y0 = imax(a.y, b.y);
    int32_t x1 = imin(a.x + a.w, b.x + b.w);
    int32_t y1 = imin(a.y + a.h, b.y + b.h);
    struct rect r = { x0, y0, imax(x1 - x0, 0), imax(y1 - y0, 0) };

    return r;
}

bool rect_contains(struct rect r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

bool rect_intersects(struct rect a, struct rect b)
{
    struct rect r = rect_intersect(a, b);

    return r.w > 0 && r.h > 0;
}

/* Uhrzeit und Systemtakt - auf dem Entwicklungsrechner nur Attrappen. */
struct datetime {
    uint16_t year;
    uint8_t  month, day;
    uint8_t  hour, minute, second;
};

void rtc_read(struct datetime *out)
{
    out->year = 2026; out->month = 8; out->day = 26;
    out->hour = 12; out->minute = 0; out->second = 0;
}

static uint64_t fake_ms;

uint64_t timer_ms(void)   { return fake_ms += 1; }
uint64_t timer_ticks(void) { return fake_ms; }
