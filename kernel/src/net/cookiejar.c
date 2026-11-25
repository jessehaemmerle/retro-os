/* cookiejar.c - siehe cookiejar.h. */

#include "cookiejar.h"
#include "kstring.h"

#define MAX_LIFETIME_S (365u * 24u * 3600u)

void cookie_jar_init(struct cookie_jar *jar)
{
    memset(jar, 0, sizeof(*jar));
}

size_t cookie_jar_count(const struct cookie_jar *jar)
{
    size_t n = 0;

    for (size_t i = 0; i < COOKIE_MAX; i++)
        if (jar->items[i].used)
            n++;
    return n;
}

static void lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

/* Ist der Rechnername eine IPv4-Adresse? Dann sind Domain-Cookies
 * sinnlos - "1.2.3.4" ist kein Teil von "3.4". */
static bool is_numeric_host(const char *host)
{
    if (!*host)
        return false;
    for (const char *p = host; *p; p++)
        if (!((*p >= '0' && *p <= '9') || *p == '.' || *p == ':'))
            return false;
    return true;
}

/* Endet host auf ".domain" - oder ist er gleich domain? */
static bool domain_match(const char *host, const char *domain)
{
    size_t h = strlen(host), d = strlen(domain);

    if (h == d)
        return strcmp(host, domain) == 0;
    if (h < d + 1)
        return false;
    return host[h - d - 1] == '.' && strcmp(host + h - d, domain) == 0;
}

/* Gilt der Anfragepfad im Bereich des Cookie-Pfades? "/a" gilt fuer "/a"
 * und "/a/b", aber nicht fuer "/ab". */
static bool path_match(const char *request, const char *cookie)
{
    size_t c = strlen(cookie);
    size_t r = 0;

    while (request[r] && request[r] != '?' && request[r] != '#')
        r++;

    if (c == 0)
        return true;
    if (r < c || strncmp(request, cookie, c) != 0)
        return false;
    if (r == c || cookie[c - 1] == '/')
        return true;
    return request[c] == '/';
}

/* Der Standardpfad: das Verzeichnis der Anfrage. */
static void default_path(const char *request, char *out, size_t size)
{
    size_t n = 0;

    while (request[n] && request[n] != '?' && request[n] != '#' &&
           n + 1 < size) {
        out[n] = request[n];
        n++;
    }
    out[n] = '\0';

    char *slash = strrchr(out, '/');

    if (!slash || slash == out) {
        strlcpy(out, "/", size);
        return;
    }
    *slash = '\0';
}

static void trim(char *s)
{
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';

    size_t lead = 0;

    while (s[lead] == ' ' || s[lead] == '\t')
        lead++;
    if (lead)
        memmove(s, s + lead, strlen(s + lead) + 1);
}

static bool contains(const char *text, const char *part)
{
    size_t n = strlen(part);

    for (; *text; text++)
        if (strncmp(text, part, n) == 0)
            return true;
    return false;
}

static long to_long(const char *s)
{
    bool negative = false;
    long value = 0;

    if (*s == '-') {
        negative = true;
        s++;
    }
    for (; *s >= '0' && *s <= '9'; s++) {
        value = value * 10 + (*s - '0');
        if (value > 1000000000L)
            value = 1000000000L;
    }
    return negative ? -value : value;
}

static struct cookie *find(struct cookie_jar *jar, const char *name,
                           const char *domain, const char *path)
{
    for (size_t i = 0; i < COOKIE_MAX; i++) {
        struct cookie *c = &jar->items[i];

        if (c->used && strcmp(c->name, name) == 0 &&
            strcmp(c->domain, domain) == 0 && strcmp(c->path, path) == 0)
            return c;
    }
    return NULL;
}

static struct cookie *slot(struct cookie_jar *jar)
{
    struct cookie *oldest = NULL;

    for (size_t i = 0; i < COOKIE_MAX; i++) {
        struct cookie *c = &jar->items[i];

        if (!c->used)
            return c;
        if (!oldest || c->sequence < oldest->sequence)
            oldest = c;
    }
    return oldest;          /* voll: das aelteste weicht */
}

bool cookie_jar_store(struct cookie_jar *jar, const char *host,
                      const char *path, bool secure, const char *set_cookie,
                      uint64_t now_ms)
{
    char line[COOKIE_NAME_MAX + COOKIE_VALUE_MAX + 256];

    if (strlen(set_cookie) >= sizeof(line))
        return false;
    strlcpy(line, set_cookie, sizeof(line));

    /* Name=Wert ist alles bis zum ersten Semikolon. */
    char *attributes = strchr(line, ';');

    if (attributes)
        *attributes++ = '\0';

    char *equals = strchr(line, '=');

    if (!equals)
        return false;
    *equals++ = '\0';

    char name[COOKIE_NAME_MAX + 1];
    char value[COOKIE_VALUE_MAX + 1];

    trim(line);
    trim(equals);
    if (!*line || strlen(line) >= COOKIE_NAME_MAX ||
        strlen(equals) >= COOKIE_VALUE_MAX)
        return false;
    strlcpy(name, line, sizeof(name));
    strlcpy(value, equals, sizeof(value));

    char req_host[COOKIE_DOMAIN_MAX];

    strlcpy(req_host, host, sizeof(req_host));
    lower(req_host);

    char domain[COOKIE_DOMAIN_MAX];
    char cpath[COOKIE_PATH_MAX];
    bool host_only = true;
    bool cookie_secure = false;
    bool expired = false;
    uint64_t expires_ms = 0;

    strlcpy(domain, req_host, sizeof(domain));
    cpath[0] = '\0';

    while (attributes && *attributes) {
        char *next = strchr(attributes, ';');

        if (next)
            *next++ = '\0';

        char *val = strchr(attributes, '=');

        if (val)
            *val++ = '\0';
        trim(attributes);
        if (val)
            trim(val);

        if (strcasecmp(attributes, "Secure") == 0) {
            cookie_secure = true;
        } else if (strcasecmp(attributes, "Domain") == 0 && val && *val) {
            char d[COOKIE_DOMAIN_MAX];

            if (*val == '.')
                val++;
            if (strlen(val) >= sizeof(d))
                return false;
            strlcpy(d, val, sizeof(d));
            lower(d);

            /* Nur ein Oberbegriff des eigenen Rechners - und nicht "com". */
            if (!domain_match(req_host, d))
                return false;
            if (!is_numeric_host(req_host) && !strchr(d, '.') &&
                strcmp(d, req_host) != 0)
                return false;
            /* Bei einer IP-Adresse gilt "Domain" nur, wenn es genau sie
             * selbst ist - und dann bleibt es ein Cookie fuer diesen einen
             * Rechner. */
            if (is_numeric_host(req_host)) {
                if (strcmp(d, req_host) != 0)
                    return false;
            } else {
                strlcpy(domain, d, sizeof(domain));
                host_only = false;
            }
        } else if (strcasecmp(attributes, "Path") == 0 && val && val[0] == '/') {
            if (strlen(val) >= sizeof(cpath))
                return false;
            strlcpy(cpath, val, sizeof(cpath));
        } else if (strcasecmp(attributes, "Max-Age") == 0 && val) {
            long age = to_long(val);

            if (age <= 0)
                expired = true;
            else
                expires_ms = now_ms +
                             (uint64_t)MIN((unsigned long)age, MAX_LIFETIME_S) * 1000u;
        } else if (strcasecmp(attributes, "Expires") == 0 && val) {
            /* Eine Uhr haben wir hier nicht; ein Datum aus der
             * Vergangenheit ist aber leicht zu erkennen: 1970, und das ist
             * die uebliche Art, ein Cookie zu loeschen. */
            if (contains(val, "1970") || contains(val, " 70 "))
                expired = true;
        }
        attributes = next;
    }

    /* "Secure" von einer unverschluesselten Seite geht nicht an. */
    if (cookie_secure && !secure)
        return false;

    if (!cpath[0])
        default_path(path, cpath, sizeof(cpath));

    struct cookie *existing = find(jar, name, domain, cpath);

    if (expired) {
        if (existing)
            existing->used = false;
        return false;
    }

    struct cookie *c = existing ? existing : slot(jar);

    memset(c, 0, sizeof(*c));
    c->used = true;
    strlcpy(c->name, name, sizeof(c->name));
    strlcpy(c->value, value, sizeof(c->value));
    strlcpy(c->domain, domain, sizeof(c->domain));
    strlcpy(c->path, cpath, sizeof(c->path));
    c->secure = cookie_secure;
    c->host_only = host_only;
    c->expires_ms = expires_ms;
    c->sequence = ++jar->sequence;
    return true;
}

size_t cookie_jar_header(const struct cookie_jar *jar, const char *host,
                         const char *path, bool secure, uint64_t now_ms,
                         char *out, size_t size)
{
    char req_host[COOKIE_DOMAIN_MAX];
    const struct cookie *chosen[COOKIE_MAX];
    size_t count = 0;

    if (size)
        out[0] = '\0';

    strlcpy(req_host, host, sizeof(req_host));
    lower(req_host);

    for (size_t i = 0; i < COOKIE_MAX; i++) {
        const struct cookie *c = &jar->items[i];

        if (!c->used)
            continue;
        if (c->expires_ms && now_ms >= c->expires_ms)
            continue;
        if (c->secure && !secure)
            continue;
        if (c->host_only ? strcmp(req_host, c->domain) != 0
                         : !domain_match(req_host, c->domain))
            continue;
        if (!path_match(path, c->path))
            continue;
        chosen[count++] = c;
    }

    /* Laengere Pfade zuerst, bei gleicher Laenge das aeltere. */
    for (size_t a = 1; a < count; a++) {
        const struct cookie *key = chosen[a];
        size_t b = a;

        while (b > 0) {
            const struct cookie *p = chosen[b - 1];
            size_t kl = strlen(key->path), pl = strlen(p->path);

            if (pl > kl || (pl == kl && p->sequence < key->sequence))
                break;
            chosen[b] = p;
            b--;
        }
        chosen[b] = key;
    }

    size_t n = 0;

    for (size_t i = 0; i < count; i++) {
        size_t need = strlen(chosen[i]->name) + 1 + strlen(chosen[i]->value) +
                      (n ? 2 : 0);

        if (n + need + 1 > size)
            break;
        if (n) {
            out[n++] = ';';
            out[n++] = ' ';
        }
        n += (size_t)ksnprintf(out + n, size - n, "%s=%s", chosen[i]->name,
                               chosen[i]->value);
    }
    return n;
}
