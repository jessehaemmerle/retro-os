/* httputil.c - siehe httputil.h. */

#include "httputil.h"
#include "kstring.h"

bool url_split(const char *url, char *host, size_t host_size,
               uint16_t *port, char *path, size_t path_size, bool *secure)
{
    const char *p = url;
    bool https = false;

    if (strncasecmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncasecmp(p, "https://", 8) == 0) {
        p += 8;
        https = true;
    }

    *port = https ? 443 : 80;
    if (secure)
        *secure = https;

    size_t n = 0;

    /* Der Rechnername endet am Pfad, an der Anfrage oder der Marke - oder
     * am Doppelpunkt vor dem Port. */
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#' &&
           n + 1 < host_size)
        host[n++] = *p++;
    host[n] = '\0';

    if (n == 0)
        return false;

    if (*p == ':') {
        p++;
        uint32_t value = 0;

        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (uint32_t)(*p++ - '0');
            if (value > 65535)
                return false;
        }
        if (value == 0)
            return false;
        *port = (uint16_t)value;
    }

    /* Eine Anfrage ohne Pfad ("host?a=b") gehoert an die Wurzel. */
    if (*p == '\0') {
        strlcpy(path, "/", path_size);
    } else if (*p == '?') {
        ksnprintf(path, path_size, "/%s", p);
    } else if (*p == '#') {
        strlcpy(path, "/", path_size);
    } else {
        strlcpy(path, p, path_size);
    }

    /* Die Marke gehoert nicht in die Anfrage. */
    char *hash = strchr(path, '#');

    if (hash)
        *hash = '\0';
    return true;
}

/* Entfernt "." und ".." aus einem Pfad (ohne Anfrage), in place. */
static void remove_dots(char *path)
{
    char out[512];
    size_t n = 0;
    const char *p = path;

    out[0] = '\0';

    while (*p) {
        /* ein Abschnitt: von '/' bis vor den naechsten '/' */
        const char *seg = p;

        if (*seg == '/')
            seg++;

        const char *end = seg;

        while (*end && *end != '/')
            end++;

        size_t len = (size_t)(end - seg);

        if (len == 1 && seg[0] == '.') {
            /* "." faellt weg; steht es am Ende, bleibt der Schraegstrich. */
            if (*end == '\0' && n < sizeof(out) - 1)
                out[n++] = '/';
        } else if (len == 2 && seg[0] == '.' && seg[1] == '.') {
            /* ".." nimmt den letzten Abschnitt zurueck (aber nie die
             * Wurzel). */
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;                             /* den Schraegstrich selbst */
            if (*end == '\0' && n < sizeof(out) - 1)
                out[n++] = '/';
        } else {
            if (n + 1 + len >= sizeof(out))
                break;
            out[n++] = '/';
            memcpy(out + n, seg, len);
            n += len;
        }
        p = end;
    }

    if (n == 0)
        out[n++] = '/';
    out[n] = '\0';
    strlcpy(path, out, 512);
}

void url_resolve(const char *base, const char *ref, char *out, size_t size)
{
    while (*ref == ' ' || *ref == '\t' || *ref == '\n' || *ref == '\r')
        ref++;

    if (!*ref) {
        strlcpy(out, base, size);
        char *hash = strchr(out, '#');

        if (hash)
            *hash = '\0';
        return;
    }

    /* Absolut: bleibt, wie es ist (ohne Marke). */
    if (strncasecmp(ref, "http://", 7) == 0 ||
        strncasecmp(ref, "https://", 8) == 0) {
        strlcpy(out, ref, size);
        char *hash = strchr(out, '#');

        if (hash)
            *hash = '\0';
        return;
    }

    char host[128], path[512];
    uint16_t port;
    bool secure = false;

    if (!url_split(base, host, sizeof(host), &port, path, sizeof(path),
                   &secure)) {
        strlcpy(out, ref, size);
        return;
    }

    const char *scheme = secure ? "https" : "http";
    uint16_t standard = secure ? 443 : 80;
    char prefix[200];

    if (port == standard)
        ksnprintf(prefix, sizeof(prefix), "%s://%s", scheme, host);
    else
        ksnprintf(prefix, sizeof(prefix), "%s://%s:%u", scheme, host, port);

    if (ref[0] == '/' && ref[1] == '/') {
        ksnprintf(out, size, "%s:%s", scheme, ref);
        char *hash = strchr(out, '#');

        if (hash)
            *hash = '\0';
        return;
    }

    if (ref[0] == '#') {                        /* nur eine Marke: dieselbe Seite */
        ksnprintf(out, size, "%s%s", prefix, path);
        return;
    }

    char result[512];

    if (ref[0] == '?') {                        /* nur eine Anfrage */
        char *query = strchr(path, '?');

        if (query)
            *query = '\0';
        ksnprintf(result, sizeof(result), "%s%s", path, ref);
    } else if (ref[0] == '/') {
        strlcpy(result, ref, sizeof(result));
    } else {
        /* Relativ zum Verzeichnis der Basis - ohne deren Anfrage, in der
         * auch Schraegstriche stehen koennen. */
        char *query = strchr(path, '?');

        if (query)
            *query = '\0';

        char *slash = strrchr(path, '/');

        if (slash)
            slash[1] = '\0';
        else
            strlcpy(path, "/", sizeof(path));
        ksnprintf(result, sizeof(result), "%s%s", path, ref);
    }

    char *hash = strchr(result, '#');

    if (hash)
        *hash = '\0';

    /* Punkte nur im Pfad entfernen, die Anfrage bleibt unberuehrt. */
    char query[512];
    char *q = strchr(result, '?');

    query[0] = '\0';
    if (q) {
        strlcpy(query, q, sizeof(query));
        *q = '\0';
    }
    remove_dots(result);

    ksnprintf(out, size, "%s%s%s", prefix, result, query);
}

bool http_header_get(const char *headers, const char *name, unsigned index,
                     char *out, size_t size)
{
    size_t name_length = strlen(name);
    const char *line = headers;
    unsigned seen = 0;

    if (size)
        out[0] = '\0';

    /* Die erste Zeile ist die Statuszeile - sie hat keinen Namen. */
    const char *newline = strchr(line, '\n');

    if (!newline)
        return false;
    line = newline + 1;

    while (*line) {
        if (strncasecmp(line, name, name_length) == 0 &&
            line[name_length] == ':') {
            if (seen++ == index) {
                const char *value = line + name_length + 1;

                while (*value == ' ' || *value == '\t')
                    value++;

                size_t n = 0;

                while (value[n] && value[n] != '\r' && value[n] != '\n' &&
                       n + 1 < size) {
                    out[n] = value[n];
                    n++;
                }
                if (size)
                    out[n] = '\0';
                return true;
            }
        }

        newline = strchr(line, '\n');
        if (!newline)
            break;
        line = newline + 1;
    }
    return false;
}

uint32_t http_decode_chunked(char *body, uint32_t length)
{
    uint32_t read = 0, write = 0;

    while (read < length) {
        uint32_t size = 0;
        bool digits = false;

        while (read < length && body[read] != '\r' && body[read] != '\n') {
            char c = body[read++];
            uint32_t digit;

            if (c >= '0' && c <= '9')      digit = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
            else break;                     /* Erweiterungen ignorieren */

            if (size > 0x0FFFFFFFu)
                return write;               /* unsinnig gross */
            size = size * 16 + digit;
            digits = true;
        }

        while (read < length && body[read] != '\n')
            read++;
        read++;                             /* Zeilenende ueberspringen */

        if (!digits || size == 0)
            break;
        if (read > length)
            break;
        if (size > length - read)
            size = length - read;

        memmove(body + write, body + read, size);
        write += size;
        read  += size + 2;                  /* Daten plus CRLF */
    }
    return write;
}

int http_status_code(const char *response)
{
    if (strncmp(response, "HTTP/1.", 7) != 0)
        return 0;

    const char *space = strchr(response, ' ');

    if (!space)
        return 0;
    if (space[1] < '0' || space[1] > '9' || space[2] < '0' || space[2] > '9' ||
        space[3] < '0' || space[3] > '9')
        return 0;

    return (space[1] - '0') * 100 + (space[2] - '0') * 10 + (space[3] - '0');
}

enum http_encoding http_content_encoding(const char *headers)
{
    char value[64];

    if (!http_header_get(headers, "Content-Encoding", 0, value, sizeof(value)))
        return ENC_IDENTITY;

    if (!*value || strcasecmp(value, "identity") == 0)
        return ENC_IDENTITY;
    if (strcasecmp(value, "gzip") == 0 || strcasecmp(value, "x-gzip") == 0)
        return ENC_GZIP;
    if (strcasecmp(value, "deflate") == 0)
        return ENC_DEFLATE;
    return ENC_UNKNOWN;
}
