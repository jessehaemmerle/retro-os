/* http.c - HTTP/1.1-Client fuer den Browser.
 *
 * Beherrscht GET, Weiterleitungen und in Stuecken uebertragene Antworten
 * ("chunked"). Verschluesselte Verbindungen (HTTPS) sind nicht enthalten -
 * dafuer waere eine vollstaendige TLS-Umsetzung mit Zertifikatspruefung
 * noetig, die den Rahmen dieses Systems sprengt.
 */

#include "net.h"
#include "arch.h"
#include "kstring.h"
#include "mm.h"
#include "tls.h"

#define HTTP_MAX_BODY     (512 * 1024)
#define HTTP_CHUNK        8192
#define HTTP_TIMEOUT_MS   8000
#define HTTP_MAX_REDIRECT 4

bool url_split(const char *url, char *host, size_t host_size,
               uint16_t *port, char *path, size_t path_size, bool *secure)
{
    const char *p = url;
    bool https = false;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        https = true;
    }

    *port = https ? 443 : 80;
    if (secure)
        *secure = https;

    size_t n = 0;
    while (*p && *p != '/' && *p != ':' && n + 1 < host_size)
        host[n++] = *p++;
    host[n] = '\0';

    if (n == 0)
        return false;

    if (*p == ':') {
        p++;
        uint32_t value = 0;
        while (*p >= '0' && *p <= '9')
            value = value * 10 + (uint32_t)(*p++ - '0');
        if (value == 0 || value > 65535)
            return false;
        *port = (uint16_t)value;
    }

    if (*p == '\0') {
        strlcpy(path, "/", path_size);
    } else {
        strlcpy(path, p, path_size);
    }
    return true;
}

/* Sucht eine Kopfzeile und kopiert ihren Wert. */
static bool header_value(const char *headers, const char *name, char *out,
                         size_t size)
{
    size_t name_length = strlen(name);
    const char *line = headers;

    while (*line) {
        if (strncasecmp(line, name, name_length) == 0 &&
            line[name_length] == ':') {
            const char *value = line + name_length + 1;

            while (*value == ' ')
                value++;

            size_t n = 0;
            while (value[n] && value[n] != '\r' && value[n] != '\n' &&
                   n + 1 < size) {
                out[n] = value[n];
                n++;
            }
            out[n] = '\0';
            return true;
        }

        const char *newline = strchr(line, '\n');
        if (!newline)
            break;
        line = newline + 1;
    }
    return false;
}

/* Loest die Stueckelung auf: <Groesse in hex>\r\n<Daten>\r\n ... 0\r\n */
static uint32_t decode_chunked(char *body, uint32_t length)
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

            size = size * 16 + digit;
            digits = true;
        }

        while (read < length && body[read] != '\n')
            read++;
        read++;                             /* Zeilenende ueberspringen */

        if (!digits || size == 0)
            break;
        if (read + size > length)
            size = length - read;

        memmove(body + write, body + read, size);
        write += size;
        read  += size + 2;                  /* Daten plus CRLF */
    }
    return write;
}

/* Ein Kanal ist entweder eine schlichte TCP-Verbindung oder eine
 * verschluesselte darauf. Der Rest des Ablaufs bleibt derselbe. */
struct channel {
    struct tcp_socket     *socket;
    struct tls_connection *tls;
};

static int channel_send(struct channel *channel, const void *data,
                        uint32_t length)
{
    if (channel->tls)
        return tls_send(channel->tls, data, length);
    return tcp_send(channel->socket, data, length);
}

static int channel_receive(struct channel *channel, void *buffer,
                           uint32_t capacity, uint32_t timeout_ms)
{
    if (channel->tls)
        return tls_receive(channel->tls, buffer, capacity, timeout_ms);
    return tcp_receive(channel->socket, buffer, capacity, timeout_ms);
}

static bool channel_finished(struct channel *channel)
{
    if (channel->tls)
        return tls_finished(channel->tls);
    return tcp_finished(channel->socket);
}

static void channel_close(struct channel *channel)
{
    if (channel->tls)
        tls_close(channel->tls);
    if (channel->socket)
        tcp_close(channel->socket);
}

static bool fetch_once(const char *url, struct http_response *out,
                       char *redirect, size_t redirect_size)
{
    char host[128], path[512];
    uint16_t port;
    bool secure = false;

    redirect[0] = '\0';

    if (!url_split(url, host, sizeof(host), &port, path, sizeof(path),
                   &secure)) {
        strlcpy(out->error, "Die Adresse ist unvollstaendig.",
                sizeof(out->error));
        return false;
    }

    ip_addr_t address;
    if (!dns_resolve(host, &address)) {
        ksnprintf(out->error, sizeof(out->error),
                  "%s laesst sich nicht aufloesen.", host);
        return false;
    }

    struct channel channel = { NULL, NULL };

    channel.socket = tcp_connect(address, port, 5000);
    if (!channel.socket) {
        ksnprintf(out->error, sizeof(out->error),
                  "Keine Verbindung zu %s:%u.", host, port);
        return false;
    }

    if (secure) {
        char reason[96];

        strlcpy(reason, "unbekannter Fehler", sizeof(reason));
        channel.tls = tls_connect(channel.socket, host, reason, sizeof(reason));

        if (!channel.tls) {
            tcp_close(channel.socket);
            ksnprintf(out->error, sizeof(out->error), "%s", reason);
            return false;
        }
        strlcpy(out->security, tls_description(channel.tls),
                sizeof(out->security));
    }

    char request[768];
    int request_length = ksnprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: RetroOS/1.0\r\n"
        "Accept: text/html,text/plain\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n", path, host);

    if (channel_send(&channel, request, (uint32_t)request_length) < 0) {
        channel_close(&channel);
        strlcpy(out->error, "Die Anfrage konnte nicht gesendet werden.",
                sizeof(out->error));
        return false;
    }

    /* Antwort einsammeln, bis die Gegenseite schliesst. */
    uint32_t capacity = HTTP_CHUNK * 4;
    char    *buffer = kmalloc(capacity);
    uint32_t length = 0;

    if (!buffer) {
        channel_close(&channel);
        strlcpy(out->error, "Zu wenig Speicher.", sizeof(out->error));
        return false;
    }

    uint64_t deadline = timer_ms() + HTTP_TIMEOUT_MS;

    for (;;) {
        if (length + HTTP_CHUNK > capacity) {
            if (capacity >= HTTP_MAX_BODY)
                break;

            uint32_t bigger = capacity * 2;
            char *grown = krealloc(buffer, bigger);

            if (!grown)
                break;
            buffer = grown;
            capacity = bigger;
        }

        int n = channel_receive(&channel, buffer + length, HTTP_CHUNK, 700);
        if (n > 0)
            length += (uint32_t)n;

        if (channel_finished(&channel) && n <= 0)
            break;
        if (timer_ms() > deadline)
            break;
    }

    channel_close(&channel);

    if (length == 0) {
        kfree(buffer);
        strlcpy(out->error, "Der Server hat nichts geantwortet.",
                sizeof(out->error));
        return false;
    }

    buffer[MIN(length, capacity - 1)] = '\0';

    /* Statuszeile */
    int status = 0;
    if (strncmp(buffer, "HTTP/1.", 7) == 0) {
        const char *space = strchr(buffer, ' ');

        if (space) {
            status = (int)((space[1] - '0') * 100 + (space[2] - '0') * 10 +
                           (space[3] - '0'));
        }
    }

    /* Kopf und Rumpf trennen */
    char *separator = NULL;
    for (uint32_t i = 0; i + 3 < length; i++) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
            separator = buffer + i;
            break;
        }
    }

    if (!separator) {
        kfree(buffer);
        strlcpy(out->error, "Die Antwort ist unverstaendlich.",
                sizeof(out->error));
        return false;
    }

    *separator = '\0';
    char    *body = separator + 4;
    uint32_t body_length = length - (uint32_t)(body - buffer);

    if (status >= 300 && status < 400) {
        char location[512];

        if (header_value(buffer, "Location", location, sizeof(location))) {
            strlcpy(redirect, location, redirect_size);
            kfree(buffer);
            return false;
        }
    }

    char encoding[64];
    if (header_value(buffer, "Transfer-Encoding", encoding, sizeof(encoding)) &&
        strcasecmp(encoding, "chunked") == 0)
        body_length = decode_chunked(body, body_length);

    if (!header_value(buffer, "Content-Type", out->content_type,
                      sizeof(out->content_type)))
        strlcpy(out->content_type, "text/html", sizeof(out->content_type));

    /* Rumpf in einen eigenen Puffer umziehen, damit der Kopf freigegeben
     * werden kann. */
    char *result = kmalloc(body_length + 1);
    if (!result) {
        kfree(buffer);
        strlcpy(out->error, "Zu wenig Speicher fuer die Seite.",
                sizeof(out->error));
        return false;
    }

    memcpy(result, body, body_length);
    result[body_length] = '\0';
    kfree(buffer);

    out->status      = status;
    out->body        = result;
    out->body_length = body_length;
    return true;
}

bool http_get(const char *url, struct http_response *out)
{
    char current[512];
    char redirect[512];

    memset(out, 0, sizeof(*out));
    strlcpy(current, url, sizeof(current));

    for (int hop = 0; hop < HTTP_MAX_REDIRECT; hop++) {
        if (fetch_once(current, out, redirect, sizeof(redirect)))
            return true;

        if (!redirect[0])
            return false;

        /* Eine Weiterleitung ohne Rechnername bezieht sich auf denselben. */
        if (redirect[0] == '/') {
            char host[128], path[512];
            uint16_t port;
            bool secure = false;

            if (!url_split(current, host, sizeof(host), &port, path,
                           sizeof(path), &secure))
                return false;

            const char *scheme = secure ? "https" : "http";
            uint16_t standard = secure ? 443 : 80;
            char combined[512];

            if (port == standard)
                ksnprintf(combined, sizeof(combined), "%s://%s%s", scheme,
                          host, redirect);
            else
                ksnprintf(combined, sizeof(combined), "%s://%s:%u%s", scheme,
                          host, port, redirect);
            strlcpy(current, combined, sizeof(current));
        } else {
            strlcpy(current, redirect, sizeof(current));
        }
    }

    strlcpy(out->error, "Zu viele Weiterleitungen.", sizeof(out->error));
    return false;
}

void http_response_free(struct http_response *response)
{
    if (response && response->body) {
        kfree(response->body);
        response->body = NULL;
        response->body_length = 0;
    }
}
