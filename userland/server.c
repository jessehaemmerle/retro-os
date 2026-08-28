/* server.c - ein Webserver, der in Ring 3 laeuft.
 *
 * Damit ist RetroOS nicht nur ansprechbar, sondern erreichbar: Vom
 * Wirtsrechner aus laesst sich die Ablage im Browser durchsehen.
 *
 * Fuer jede Verbindung spaltet er sich ab: Das Kind beantwortet die
 * eine Anfrage und ist danach fertig, der Elternteil nimmt sofort die
 * naechste an. Das kostet fast nichts, weil das Kind den Speicher des
 * Elternteils zunaechst nur mitbenutzt (siehe vmm_fork im Kernel), und
 * es macht den Server unempfindlich: Eine Verbindung, die haengt,
 * blockiert nur ihr eigenes Kind.
 *
 * Geht das Abspalten nicht mehr - weil schon zu viele Kinder laufen -,
 * beantwortet der Elternteil die Anfrage selbst. Lieber langsam als
 * gar nicht.
 *
 *     starte server            hoert auf Port 8080, liefert /Festplatte
 *     starte server 8000 /     anderer Port, andere Wurzel
 *
 * Beenden mit Strg+C in der Konsole.
 */

#include "retroos.h"
#include "retroui.h"

#define PORT_DEFAULT  8080
#define ROOT_DEFAULT  "/Festplatte"
#define REQUEST_MAX   2048
#define CHUNK         4096

static char root[128];

/* ------------------------------------------------------------------ */
/* Kleine Zeichenkettenhilfen - die Bibliothek der Programme ist knapp */
/* ------------------------------------------------------------------ */

static void append(char *dst, size_t room, const char *src)
{
    size_t at = strlen(dst);

    while (*src && at + 1 < room)
        dst[at++] = *src++;
    dst[at] = '\0';
}

static int ends_with(const char *text, const char *suffix)
{
    size_t a = strlen(text), b = strlen(suffix);

    if (b > a)
        return 0;
    for (size_t i = 0; i < b; i++) {
        char x = text[a - b + i], y = suffix[i];

        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y)
            return 0;
    }
    return 1;
}

/* Wandelt %20 und Konsorten zurueck. */
static void unescape(char *text)
{
    char *out = text;

    for (const char *in = text; *in; in++) {
        if (*in == '%' && in[1] && in[2]) {
            int hi = in[1], lo = in[2];

            hi = hi >= 'a' ? hi - 'a' + 10 : (hi >= 'A' ? hi - 'A' + 10 : hi - '0');
            lo = lo >= 'a' ? lo - 'a' + 10 : (lo >= 'A' ? lo - 'A' + 10 : lo - '0');
            if (hi >= 0 && hi < 16 && lo >= 0 && lo < 16) {
                *out++ = (char)(hi * 16 + lo);
                in += 2;
                continue;
            }
        }
        *out++ = *in;
    }
    *out = '\0';
}

static const char *content_type(const char *path)
{
    if (ends_with(path, ".html") || ends_with(path, ".htm")) return "text/html";
    if (ends_with(path, ".css"))  return "text/css";
    if (ends_with(path, ".js"))   return "text/javascript";
    if (ends_with(path, ".png"))  return "image/png";
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg")) return "image/jpeg";
    if (ends_with(path, ".gif"))  return "image/gif";
    if (ends_with(path, ".bmp"))  return "image/bmp";
    if (ends_with(path, ".elf"))  return "application/octet-stream";
    return "text/plain; charset=iso-8859-1";
}

/* ------------------------------------------------------------------ */
/* Antworten                                                           */
/* ------------------------------------------------------------------ */

static void send_all(int sock, const char *data, unsigned length)
{
    unsigned sent = 0;

    while (sent < length) {
        int n = net_send(sock, data + sent, length - sent);

        if (n <= 0)
            return;
        sent += (unsigned)n;
    }
}

static void send_text(int sock, const char *status, const char *type,
                      const char *body)
{
    char head[256];

    head[0] = '\0';
    append(head, sizeof(head), "HTTP/1.1 ");
    append(head, sizeof(head), status);
    append(head, sizeof(head), "\r\nContent-Type: ");
    append(head, sizeof(head), type);
    append(head, sizeof(head), "\r\nContent-Length: ");

    char number[16];
    unsigned n = (unsigned)strlen(body);
    int digits = 0;
    char tmp[16];

    if (n == 0) {
        number[0] = '0';
        number[1] = '\0';
    } else {
        while (n > 0) { tmp[digits++] = (char)('0' + n % 10); n /= 10; }
        for (int i = 0; i < digits; i++)
            number[i] = tmp[digits - 1 - i];
        number[digits] = '\0';
    }

    append(head, sizeof(head), number);
    append(head, sizeof(head), "\r\nConnection: close\r\nServer: RetroOS\r\n\r\n");

    send_all(sock, head, (unsigned)strlen(head));
    send_all(sock, body, (unsigned)strlen(body));
}

/* Baut den vollen Pfad aus Wurzel und angefragtem Weg. */
static int build_path(char *out, size_t room, const char *request_path)
{
    /* Nichts ausserhalb der Wurzel herausgeben. */
    if (request_path[0] != '/')
        return 0;
    for (const char *p = request_path; *p; p++) {
        if (p[0] == '.' && p[1] == '.')
            return 0;
    }

    out[0] = '\0';
    append(out, room, root);

    /* Doppelte Schraegstriche vermeiden. */
    size_t at = strlen(out);

    if (at > 0 && out[at - 1] == '/')
        out[at - 1] = '\0';

    append(out, room, request_path);

    at = strlen(out);
    if (at > 1 && out[at - 1] == '/')
        out[at - 1] = '\0';
    return 1;
}

/* Ein Verzeichnis als Seite mit Verweisen. */
static void serve_directory(int sock, const char *path, const char *web_path)
{
    static char page[16384];

    page[0] = '\0';
    append(page, sizeof(page),
           "<!doctype html><html lang=\"de\"><head><meta charset=\"iso-8859-1\">"
           "<title>RetroOS</title><style>"
           "body{font-family:monospace;background:#12333b;color:#dfe8ea;"
           "margin:2em auto;max-width:44em}"
           "a{color:#7fd4e8}h1{font-size:1.2em}"
           "li{margin:.2em 0}</style></head><body><h1>");
    append(page, sizeof(page), web_path);
    append(page, sizeof(page), "</h1><ul>");

    if (strcmp(web_path, "/") != 0)
        append(page, sizeof(page), "<li><a href=\"..\">..</a></li>");

    char entry[160];
    unsigned index = 0;

    while (sys_readdir(path, index, entry, sizeof(entry)) > 0) {
        append(page, sizeof(page), "<li><a href=\"");
        append(page, sizeof(page), entry);
        append(page, sizeof(page), "\">");
        append(page, sizeof(page), entry);
        append(page, sizeof(page), "</a></li>");
        index++;
        if (strlen(page) > sizeof(page) - 512)
            break;
    }

    append(page, sizeof(page), "</ul><p>RetroOS</p></body></html>");
    send_text(sock, "200 OK", "text/html; charset=iso-8859-1", page);
}

static void serve_file(int sock, const char *path, int fd)
{
    long size = sys_filesize(fd);

    if (size < 0)
        size = 0;

    char head[256];
    char number[16], tmp[16];
    unsigned n = (unsigned)size;
    int digits = 0;

    if (n == 0) {
        number[0] = '0';
        number[1] = '\0';
    } else {
        while (n > 0) { tmp[digits++] = (char)('0' + n % 10); n /= 10; }
        for (int i = 0; i < digits; i++)
            number[i] = tmp[digits - 1 - i];
        number[digits] = '\0';
    }

    head[0] = '\0';
    append(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: ");
    append(head, sizeof(head), content_type(path));
    append(head, sizeof(head), "\r\nContent-Length: ");
    append(head, sizeof(head), number);
    append(head, sizeof(head), "\r\nConnection: close\r\nServer: RetroOS\r\n\r\n");
    send_all(sock, head, (unsigned)strlen(head));

    static char buffer[CHUNK];
    long left = size;

    while (left > 0) {
        long want = left < CHUNK ? left : CHUNK;
        long got = sys_read(fd, buffer, (size_t)want);

        if (got <= 0)
            break;
        send_all(sock, buffer, (unsigned)got);
        left -= got;
    }
}

/* ------------------------------------------------------------------ */
/* Eine Anfrage                                                        */
/* ------------------------------------------------------------------ */

static void handle(int sock)
{
    static char request[REQUEST_MAX];
    int used = 0;

    /* Der Kopf endet an der Leerzeile. Mehr brauchen wir nicht - der
     * Server nimmt ohnehin nur GET entgegen. */
    while (used < REQUEST_MAX - 1) {
        int n = net_recv(sock, request + used, (unsigned)(REQUEST_MAX - 1 - used),
                         2000);

        if (n <= 0)
            break;
        used += n;
        request[used] = '\0';

        int done = 0;

        for (int i = 3; i < used; i++) {
            if (request[i - 3] == '\r' && request[i - 2] == '\n' &&
                request[i - 1] == '\r' && request[i] == '\n') {
                done = 1;
                break;
            }
        }
        if (done)
            break;
    }

    if (used <= 0)
        return;
    request[used] = '\0';

    if (request[0] != 'G' || request[1] != 'E' || request[2] != 'T') {
        send_text(sock, "405 Method Not Allowed", "text/plain",
                  "Nur GET.\n");
        return;
    }

    /* Den Weg aus der ersten Zeile herausschneiden. */
    char web_path[160];
    int at = 4;
    int k = 0;

    while (request[at] && request[at] != ' ' && request[at] != '\r' &&
           k < (int)sizeof(web_path) - 1)
        web_path[k++] = request[at++];
    web_path[k] = '\0';

    /* Was hinter einem Fragezeichen steht, geht uns nichts an. */
    for (int i = 0; web_path[i]; i++) {
        if (web_path[i] == '?') {
            web_path[i] = '\0';
            break;
        }
    }

    unescape(web_path);
    if (!web_path[0])
        strcpy(web_path, "/");

    printf("  %s\n", web_path);

    char path[256];

    if (!build_path(path, sizeof(path), web_path)) {
        send_text(sock, "403 Forbidden", "text/plain", "Nicht erlaubt.\n");
        return;
    }

    /* Ein Verzeichnis laesst sich nicht oeffnen - das unterscheidet
     * hier Datei von Ordner. */
    int fd = sys_open(path, 0);

    if (fd >= 0) {
        serve_file(sock, path, fd);
        sys_close(fd);
        return;
    }

    char probe[160];

    if (sys_readdir(path, 0, probe, sizeof(probe)) >= 0) {
        serve_directory(sock, path, web_path);
        return;
    }

    send_text(sock, "404 Not Found", "text/plain",
              "Das gibt es hier nicht.\n");
}

/* ------------------------------------------------------------------ */

int main(void)
{
    char args[128];
    int port = PORT_DEFAULT;

    strcpy(root, ROOT_DEFAULT);

    if (sys_args(args, sizeof(args)) > 0 && args[0]) {
        int i = 0;

        while (args[i] == ' ')
            i++;

        if (args[i] >= '0' && args[i] <= '9') {
            port = atoi(&args[i]);
            while (args[i] && args[i] != ' ')
                i++;
        }
        while (args[i] == ' ')
            i++;
        if (args[i]) {
            int k = 0;

            while (args[i] && args[i] != ' ' && k < (int)sizeof(root) - 1)
                root[k++] = args[i++];
            root[k] = '\0';
        }
    }

    /* Ohne formatierte Festplatte gibt es /Festplatte nicht. Dann ist
     * die Ablage im Arbeitsspeicher die naechstbeste Wurzel. */
    char probe[160];

    if (sys_readdir(root, 0, probe, sizeof(probe)) < 0)
        strcpy(root, "/");

    int listener = net_listen(port);

    if (listener < 0) {
        printf("Port %d laesst sich nicht belegen.\n", port);
        return 1;
    }

    printf("Webserver auf Port %d, Wurzel %s\n", port, root);
    printf("Beenden mit Strg+C.\n");

    for (;;) {
        int sock = net_accept(listener, 1000);

        if (sock < 0) {
            /* In der Sekunde kam keine. Gute Gelegenheit, fertige
             * Kinder einzusammeln. */
            while (sys_wait(0, 0, 0) > 0)
                ;
            continue;
        }

        int kind = sys_fork();

        if (kind == 0) {
            /* Das Kind braucht den Zuhoerer nicht - es hat schon, was
             * es beantworten soll. */
            net_close(listener);
            handle(sock);
            net_close(sock);
            sys_exit(0);
        }

        /* Kein Platz mehr fuer ein Kind - dann eben selbst. */
        if (kind < 0)
            handle(sock);

        /* Der Elternteil gibt seine Fassung der Verbindung ab; das
         * Kind haelt sie weiter. */
        net_close(sock);

        /* Was schon fertig ist, gleich einsammeln. */
        while (sys_wait(0, 0, 0) > 0)
            ;
    }
}
