/* formutil.c - siehe formutil.h. */

#include "formutil.h"
#include "kstring.h"

static bool unreserved(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
           c == '*';
}

size_t form_encode(char *out, size_t size, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;

    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (unreserved(*p) || *p == ' ') {
            if (n + 2 > size)
                goto overflow;
            out[n++] = *p == ' ' ? '+' : (char)*p;
        } else if (*p == '\r' && p[1] == '\n') {
            /* Zeilenumbrueche gehen als %0D%0A - ein Textfeld hat sie so. */
            if (n + 7 > size)
                goto overflow;
            out[n++] = '%'; out[n++] = '0'; out[n++] = 'D';
            out[n++] = '%'; out[n++] = '0'; out[n++] = 'A';
            p++;
        } else {
            if (n + 4 > size)
                goto overflow;
            out[n++] = '%';
            out[n++] = hex[*p >> 4];
            out[n++] = hex[*p & 15];
        }
    }
    if (n + 1 > size)
        goto overflow;
    out[n] = '\0';
    return n;

overflow:
    if (size)
        out[0] = '\0';
    return (size_t)-1;
}

bool form_append(char *out, size_t size, const char *name, const char *value)
{
    size_t used = strlen(out);
    size_t n = used;

    if (used && n + 1 < size)
        out[n++] = '&';
    else if (used)
        return false;

    size_t k = form_encode(out + n, size - n, name);

    if (k == (size_t)-1) {
        out[used] = '\0';
        return false;
    }
    n += k;
    if (n + 1 >= size) {
        out[used] = '\0';
        return false;
    }
    out[n++] = '=';

    k = form_encode(out + n, size - n, value ? value : "");
    if (k == (size_t)-1) {
        out[used] = '\0';
        return false;
    }
    return true;
}

bool form_get_url(char *out, size_t size, const char *action,
                  const char *query)
{
    char base[512];

    strlcpy(base, action, sizeof(base));

    char *cut = strchr(base, '#');

    if (cut)
        *cut = '\0';
    cut = strchr(base, '?');
    if (cut)
        *cut = '\0';

    int n;

    if (query && *query)
        n = ksnprintf(out, size, "%s?%s", base, query);
    else
        n = ksnprintf(out, size, "%s", base);
    return n >= 0 && (size_t)n < size;
}
