/* ulib.c - das Noetigste an Bibliothek fuer Benutzerprogramme.
 *
 * Bewusst klein gehalten: Zeichenketten, Ausgabe und ein Speicher-
 * verwalter, der nur wachsen kann. Alles Weitere kann jedes Programm
 * selbst mitbringen.
 */

#include "retroos.h"

#include <stdarg.h>

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
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while ((*d++ = *src++))
        ;
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    uint8_t *p = dst;

    while (n--)
        *p++ = (uint8_t)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void print(const char *text)
{
    sys_write(1, text, strlen(text));
}

void println(const char *text)
{
    print(text);
    sys_write(1, "\n", 1);
}

static void put_number(char *out, unsigned long value, unsigned base,
                       int upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;

    if (value == 0)
        tmp[n++] = '0';
    while (value) {
        tmp[n++] = digits[value % base];
        value /= base;
    }
    for (int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    out[n] = '\0';
}

void printf(const char *format, ...)
{
    char buffer[512];
    size_t pos = 0;
    va_list ap;

    va_start(ap, format);

    for (const char *p = format; *p && pos + 32 < sizeof(buffer); p++) {
        if (*p != '%') {
            buffer[pos++] = *p;
            continue;
        }
        p++;

        int width = 0;
        int zero = 0;

        if (*p == '0') {
            zero = 1;
            p++;
        }
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (*p++ - '0');

        /* "l" vor der Art heisst: der Wert ist 64 Bit breit. Ohne das
         * las %d bisher acht Bytes, wo der Aufrufer vier uebergeben
         * hatte - eine negative Zahl kam dann als riesige positive
         * heraus. */
        int wide = 0;

        while (*p == 'l' || *p == 'z') {
            wide = 1;
            p++;
        }

        char number[24];
        const char *text = number;
        int negative = 0;

        switch (*p) {
        case 's':
            text = va_arg(ap, const char *);
            if (!text)
                text = "(null)";
            break;
        case 'd': {
            long value = wide ? va_arg(ap, long) : va_arg(ap, int);

            if (value < 0) {
                negative = 1;
                value = -value;
            }
            put_number(number, (unsigned long)value, 10, 0);
            break;
        }
        case 'u':
            put_number(number, wide ? va_arg(ap, unsigned long)
                                    : va_arg(ap, unsigned), 10, 0);
            break;
        case 'x':
            put_number(number, wide ? va_arg(ap, unsigned long)
                                    : va_arg(ap, unsigned), 16, 0);
            break;
        case 'c':
            number[0] = (char)va_arg(ap, int);
            number[1] = '\0';
            break;
        case '%':
            number[0] = '%';
            number[1] = '\0';
            break;
        default:
            number[0] = '%';
            number[1] = *p;
            number[2] = '\0';
            break;
        }

        int length = (int)strlen(text) + negative;

        for (int i = length; i < width && pos + 1 < sizeof(buffer); i++)
            buffer[pos++] = zero ? '0' : ' ';
        if (negative)
            buffer[pos++] = '-';
        for (const char *q = text; *q && pos + 1 < sizeof(buffer); q++)
            buffer[pos++] = *q;
    }

    va_end(ap);
    sys_write(1, buffer, pos);
}

/* Ein Speicherverwalter, der nur waechst: fuer kurze Programme genau
 * richtig, und er kommt mit einem einzigen Systemaufruf aus. */
static uint8_t *heap_pointer;
static size_t   heap_left;

void *malloc(size_t size)
{
    size = (size + 15) & ~(size_t)15;

    if (size > heap_left) {
        size_t want = size > 65536 ? size : 65536;
        void *block = sys_sbrk((long)want);

        if (!block || (long)block < 0)
            return NULL;

        heap_pointer = block;
        heap_left = want;
    }

    void *result = heap_pointer;

    heap_pointer += size;
    heap_left -= size;
    return result;
}

void free(void *pointer)
{
    (void)pointer;   /* Dieser Verwalter gibt nichts zurueck. */
}

int atoi(const char *text)
{
    int value = 0;
    int sign = 1;

    while (*text == ' ')
        text++;
    if (*text == '-') {
        sign = -1;
        text++;
    }
    while (*text >= '0' && *text <= '9')
        value = value * 10 + (*text++ - '0');

    return value * sign;
}
