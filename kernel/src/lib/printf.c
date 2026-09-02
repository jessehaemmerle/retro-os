/* printf.c - schlanke printf-Familie fuer Kernel-Meldungen.
 *
 * Unterstuetzt: %c %s %d %i %u %x %X %p %% sowie Breite ("%8d"),
 * Nullauffuellung ("%04x"), Linksbuendigkeit ("%-8s") und das
 * Laengen-Praefix "l"/"ll"/"z".
 */

#include "retro.h"
#include "kstring.h"
#include "serial.h"

#include <stdarg.h>
#include "spinlock.h"

struct sink {
    char  *buf;   /* NULL => Ausgabe auf die serielle Konsole */
    size_t size;
    size_t used;
};

static void sink_putc(struct sink *s, char c)
{
    if (s->buf) {
        if (s->used + 1 < s->size)
            s->buf[s->used] = c;
    } else {
        serial_putc(c);
    }
    s->used++;
}

static void sink_puts(struct sink *s, const char *str)
{
    while (*str)
        sink_putc(s, *str++);
}

static void format_number(char *out, uint64_t value, unsigned base, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int  n = 0;

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

static void emit_padded(struct sink *s, const char *text, int width,
                        bool left, bool zero, bool negative)
{
    int len = (int)strlen(text) + (negative ? 1 : 0);
    int pad = width > len ? width - len : 0;

    if (negative && zero)
        sink_putc(s, '-');
    if (!left) {
        while (pad--)
            sink_putc(s, zero ? '0' : ' ');
    }
    if (negative && !zero)
        sink_putc(s, '-');
    sink_puts(s, text);
    if (left) {
        while (pad--)
            sink_putc(s, ' ');
    }
}

static void do_format(struct sink *s, const char *fmt, va_list ap)
{
    char num[24];

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sink_putc(s, *fmt);
            continue;
        }
        fmt++;

        bool left = false, zero = false;
        for (;; fmt++) {
            if (*fmt == '-')      left = true;
            else if (*fmt == '0') zero = true;
            else                  break;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        int longness = 0;
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h') {
            if (*fmt != 'h')
                longness++;
            fmt++;
        }

        switch (*fmt) {
        case 'c':
            sink_putc(s, (char)va_arg(ap, int));
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            emit_padded(s, str ? str : "(null)", width, left, false, false);
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = longness ? va_arg(ap, int64_t) : va_arg(ap, int);
            bool neg = v < 0;
            format_number(num, neg ? (uint64_t)(-v) : (uint64_t)v, 10, false);
            emit_padded(s, num, width, left, zero, neg);
            break;
        }
        case 'u': {
            uint64_t v = longness ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            format_number(num, v, 10, false);
            emit_padded(s, num, width, left, zero, false);
            break;
        }
        case 'o': {
            /* Rechtebits liest man in Achtelschritten - dafuer gibt es
             * diese Umwandlung, sonst braeuchte niemand sie. */
            uint64_t v = longness ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            format_number(num, v, 8, false);
            emit_padded(s, num, width, left, zero, false);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t v = longness ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            format_number(num, v, 16, *fmt == 'X');
            emit_padded(s, num, width, left, zero, false);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)va_arg(ap, void *);
            format_number(num, v, 16, false);
            sink_puts(s, "0x");
            emit_padded(s, num, 16, false, true, false);
            break;
        }
        case '%':
            sink_putc(s, '%');
            break;
        case '\0':
            return;
        default:
            sink_putc(s, '%');
            sink_putc(s, *fmt);
            break;
        }
    }
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct sink s = { .buf = buf, .size = size, .used = 0 };

    do_format(&s, fmt, ap);
    if (size > 0)
        buf[MIN(s.used, size - 1)] = '\0';
    return (int)s.used;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* Ohne Sperre schieben sich die Zeilen mehrerer Kerne ineinander -
 * dann steht auf der seriellen Schnittstelle Buchstabensalat. */
static struct spinlock print_lock = SPINLOCK_INIT("ausgabe");

void kprintf(const char *fmt, ...)
{
    struct sink s = { .buf = NULL, .size = 0, .used = 0 };
    va_list ap;
    uint64_t flags = spin_lock_irq(&print_lock);

    va_start(ap, fmt);
    do_format(&s, fmt, ap);
    va_end(ap);

    spin_unlock_irq(&print_lock, flags);
}
