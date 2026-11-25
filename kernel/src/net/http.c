/* http.c - HTTP/1.1-Client fuer den Browser.
 *
 * Beherrscht GET und POST, Weiterleitungen, in Stuecken uebertragene
 * Antworten ("chunked"), gzip und deflate sowie Cookies. HTTPS laeuft
 * ueber tls.c. Das Rechnen darum herum - Adressen, Kopfzeilen,
 * Stueckelung, Cookies - steht in httputil.c und cookiejar.c und ist auf
 * dem Entwicklungsrechner geprueft.
 */

#include "net.h"
#include "arch.h"
#include "kstring.h"
#include "mm.h"
#include "tls.h"
#include "httputil.h"
#include "cookiejar.h"
#include "inflate.h"
#include "spinlock.h"

#define HTTP_MAX_BODY     (512 * 1024)
#define HTTP_CHUNK        8192
/* Die Wartezeit gilt fuer den Stillstand, nicht fuer die ganze
 * Uebertragung: Solange Daten ankommen, wird weiter eingesammelt. Erst
 * wenn eine Weile gar nichts mehr kommt - oder die harte Grenze
 * erreicht ist - wird abgebrochen. */
#define HTTP_TIMEOUT_MS   8000
#define HTTP_TOTAL_MS     60000
#define HTTP_MAX_REDIRECT 6
/* Entpackt darf eine Antwort hoechstens so gross sein - sonst wird aus
 * einer kleinen Antwort ein Speicherfresser. */
#define HTTP_MAX_INFLATED (4 * 1024 * 1024)
#define HTTP_REQUEST_MAX  4096

/* Alle Fenster teilen sich einen Vorrat an Cookies; jedes hat aber ihren
 * eigenen Arbeits-Thread. */
static struct cookie_jar jar;
static struct spinlock   jar_lock;
static bool              jar_ready;

static void jar_use(void)
{
    if (!jar_ready) {
        cookie_jar_init(&jar);
        spin_init(&jar_lock, "cookies");
        jar_ready = true;
    }
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

/* Eine Anfrage. Kommt eine Weiterleitung zurueck, steht ihr Ziel in
 * redirect und redirect_status nennt den Code; sonst ist redirect leer. */
static bool fetch_once(const char *method, const char *url, const char *body_in,
                       uint32_t body_in_length, const char *content_type_in,
                       struct http_response *out, char *redirect,
                       size_t redirect_size, int *redirect_status)
{
    char host[128], path[512];
    uint16_t port;
    bool secure = false;

    redirect[0] = '\0';
    *redirect_status = 0;

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

    /* Die Anfrage - gross genug fuer Cookies. */
    char *request = kmalloc(HTTP_REQUEST_MAX);

    if (!request) {
        channel_close(&channel);
        strlcpy(out->error, "Zu wenig Speicher.", sizeof(out->error));
        return false;
    }

    char cookies[1536];

    jar_use();
    spin_lock(&jar_lock);
    cookie_jar_header(&jar, host, path, secure, timer_ms(), cookies,
                      sizeof(cookies));
    spin_unlock(&jar_lock);

    int request_length = ksnprintf(request, HTTP_REQUEST_MAX,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: RetroOS/1.0\r\n"
        "Accept: text/html,text/plain,*/*;q=0.5\r\n"
        "Accept-Encoding: gzip, deflate\r\n"
        "Connection: close\r\n",
        method, path, host);

    if (cookies[0])
        request_length += ksnprintf(request + request_length,
                                    HTTP_REQUEST_MAX - (size_t)request_length,
                                    "Cookie: %s\r\n", cookies);

    if (body_in) {
        request_length += ksnprintf(request + request_length,
                                    HTTP_REQUEST_MAX - (size_t)request_length,
                                    "Content-Type: %s\r\n"
                                    "Content-Length: %u\r\n",
                                    content_type_in
                                        ? content_type_in
                                        : "application/x-www-form-urlencoded",
                                    (unsigned)body_in_length);
    }
    request_length += ksnprintf(request + request_length,
                                HTTP_REQUEST_MAX - (size_t)request_length,
                                "\r\n");

    if (request_length <= 0 || (size_t)request_length >= HTTP_REQUEST_MAX ||
        (body_in && (size_t)request_length + body_in_length >= HTTP_REQUEST_MAX)) {
        kfree(request);
        channel_close(&channel);
        strlcpy(out->error, "Die Anfrage ist zu lang.", sizeof(out->error));
        return false;
    }

    if (body_in) {
        memcpy(request + request_length, body_in, body_in_length);
        request_length += (int)body_in_length;
    }

    int sent = channel_send(&channel, request, (uint32_t)request_length);

    kfree(request);
    if (sent < 0) {
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

    uint64_t idle_deadline = timer_ms() + HTTP_TIMEOUT_MS;
    uint64_t hard_deadline = timer_ms() + HTTP_TOTAL_MS;
    bool     cut_short = false;

    for (;;) {
        if (length + HTTP_CHUNK > capacity) {
            if (capacity >= HTTP_MAX_BODY) {
                cut_short = true;
                break;
            }

            uint32_t bigger = capacity * 2;
            char *grown = krealloc(buffer, bigger);

            if (!grown) {
                cut_short = true;
                break;
            }
            buffer = grown;
            capacity = bigger;
        }

        int n = channel_receive(&channel, buffer + length, HTTP_CHUNK, 700);
        if (n > 0) {
            length += (uint32_t)n;
            idle_deadline = timer_ms() + HTTP_TIMEOUT_MS;
        }

        if (channel_finished(&channel) && n <= 0)
            break;
        if (timer_ms() > idle_deadline || timer_ms() > hard_deadline) {
            cut_short = true;
            break;
        }
    }

    channel_close(&channel);

    if (length == 0) {
        kfree(buffer);
        strlcpy(out->error, "Der Server hat nichts geantwortet.",
                sizeof(out->error));
        return false;
    }

    buffer[MIN(length, capacity - 1)] = '\0';

    int status = http_status_code(buffer);

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

    /* Cookies merken - auch die einer Weiterleitung. Sie kommen als
     * mehrere gleichnamige Kopfzeilen. */
    {
        char line[COOKIE_NAME_MAX + COOKIE_VALUE_MAX + 256];

        spin_lock(&jar_lock);
        for (unsigned i = 0; i < 32 &&
             http_header_get(buffer, "Set-Cookie", i, line, sizeof(line)); i++)
            cookie_jar_store(&jar, host, path, secure, line, timer_ms());
        spin_unlock(&jar_lock);
    }

    if (status >= 300 && status < 400) {
        char location[512];

        if (http_header_get(buffer, "Location", 0, location, sizeof(location))) {
            strlcpy(redirect, location, redirect_size);
            *redirect_status = status;
            kfree(buffer);
            return false;
        }
    }

    char encoding[64];
    bool chunked = http_header_get(buffer, "Transfer-Encoding", 0, encoding,
                                   sizeof(encoding)) &&
                   strcasecmp(encoding, "chunked") == 0;

    if (chunked)
        body_length = http_decode_chunked(body, body_length);

    if (!http_header_get(buffer, "Content-Type", 0, out->content_type,
                         sizeof(out->content_type)))
        strlcpy(out->content_type, "text/html", sizeof(out->content_type));

    /* Sagt der Kopf, wie lang der Rumpf sein sollte, wird nachgezaehlt.
     * Eine abgeschnittene Seite als vollstaendig auszugeben waere die
     * unangenehmere Art von Fehler: Man sieht sie nicht. (Gilt fuer die
     * uebertragene, also die gepackte Laenge.) */
    char announced[32];

    if (!chunked &&
        http_header_get(buffer, "Content-Length", 0, announced,
                        sizeof(announced))) {
        uint32_t expected = 0;

        for (const char *d = announced; *d >= '0' && *d <= '9'; d++)
            expected = expected * 10 + (uint32_t)(*d - '0');

        if (expected > body_length)
            cut_short = true;
    }

    /* Gepackt? Dann entpacken - mit Grenze. */
    enum http_encoding coding = http_content_encoding(buffer);
    char *unpacked = NULL;

    if (coding == ENC_GZIP || coding == ENC_DEFLATE) {
        size_t inflated_length = 0;

        unpacked = inflate_auto_limited((const uint8_t *)body, body_length,
                                        &inflated_length, HTTP_MAX_INFLATED);
        if (!unpacked) {
            /* Eine abgeschnittene Antwort entpackt sich nicht - das ist
             * dann kein Fehler des Servers. */
            kfree(buffer);
            strlcpy(out->error, cut_short
                        ? "Die Antwort wurde unterbrochen."
                        : "Die Antwort laesst sich nicht entpacken.",
                    sizeof(out->error));
            return false;
        }
        body = unpacked;
        body_length = (uint32_t)inflated_length;
    } else if (coding == ENC_UNKNOWN) {
        kfree(buffer);
        strlcpy(out->error, "Unbekannte Kodierung der Antwort.",
                sizeof(out->error));
        return false;
    }

    /* Rumpf in einen eigenen Puffer umziehen, damit der Kopf freigegeben
     * werden kann. */
    char *result = kmalloc(body_length + 1);
    if (!result) {
        kfree(unpacked);
        kfree(buffer);
        strlcpy(out->error, "Zu wenig Speicher fuer die Seite.",
                sizeof(out->error));
        return false;
    }

    memcpy(result, body, body_length);
    result[body_length] = '\0';
    kfree(unpacked);
    kfree(buffer);

    out->status      = status;
    out->body        = result;
    out->body_length = body_length;
    out->truncated   = cut_short;
    return true;
}

bool http_request(const char *method, const char *url, const char *body,
                  uint32_t body_length, const char *content_type,
                  struct http_response *out)
{
    char current[512];
    char redirect[512];
    char verb[8];

    memset(out, 0, sizeof(*out));
    strlcpy(current, url, sizeof(current));
    strlcpy(verb, method, sizeof(verb));

    for (int hop = 0; hop < HTTP_MAX_REDIRECT; hop++) {
        int status = 0;

        if (fetch_once(verb, current, body, body_length, content_type, out,
                       redirect, sizeof(redirect), &status)) {
            strlcpy(out->final_url, current, sizeof(out->final_url));
            return true;
        }

        if (!redirect[0])
            return false;

        /* 301, 302 und 303 machen aus einem POST ein GET ohne Rumpf; 307
         * und 308 behalten beides. */
        if (status == 301 || status == 302 || status == 303) {
            if (strcmp(verb, "GET") != 0) {
                strlcpy(verb, "GET", sizeof(verb));
                body = NULL;
                body_length = 0;
            }
        }

        char next[512];

        url_resolve(current, redirect, next, sizeof(next));
        strlcpy(current, next, sizeof(current));
    }

    strlcpy(out->error, "Zu viele Weiterleitungen.", sizeof(out->error));
    return false;
}

bool http_get(const char *url, struct http_response *out)
{
    return http_request("GET", url, NULL, 0, NULL, out);
}

bool http_post(const char *url, const char *body, uint32_t body_length,
               const char *content_type, struct http_response *out)
{
    return http_request("POST", url, body ? body : "", body_length,
                        content_type, out);
}

void http_response_free(struct http_response *response)
{
    if (response && response->body) {
        kfree(response->body);
        response->body = NULL;
        response->body_length = 0;
    }
}
