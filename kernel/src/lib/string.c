/* string.c - die wenigen libc-Funktionen, die der Kernel selbst mitbringt. */

#include "kstring.h"

void *memset(void *dst, int c, size_t n)
{
    uint8_t *p = dst;
    while (n--)
        *p++ = (uint8_t)c;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    /* Wortweise kopieren, solange es sich lohnt. */
    while (n >= 8) {
        *(uint64_t *)d = *(const uint64_t *)s;
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d == s || n == 0)
        return dst;

    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    }
    return 0;
}

void memset32(void *dst, uint32_t value, size_t count)
{
    uint32_t *p = dst;
    while (count--)
        *p++ = value;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return (int)(uint8_t)lower(*a) - (int)(uint8_t)lower(*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n && *a && lower(*a) == lower(*b)) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(uint8_t)lower(*a) - (int)(uint8_t)lower(*b);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src)
{
    char *d = dst + strlen(dst);
    while ((*d++ = *src++))
        ;
    return dst;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) {
        if (*s == (char)c)
            return (char *)s;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (; *s; s++) {
        if (*s == (char)c)
            last = s;
    }
    if (c == 0)
        return (char *)s;
    return (char *)last;
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
