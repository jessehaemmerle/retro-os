/* abrufen.c - holt eine Seite aus dem Netz, aus Ring 3 heraus.
 *
 * Der Netzstapel gehoert dem Kernel; das Programm bekommt nur eine
 * Verbindung und schiebt Bytes hindurch. HTTP selbst ist so einfach,
 * dass die paar Zeilen hier reichen.
 *
 *     starte abrufen example.com
 */

#include "retroos.h"
#include "retroui.h"

static void put(const char *s) { print(s); }

/* Haengt an - die Bibliothek der Programme kennt kein strcat. */
static void strcat_into(char *dst, const char *src)
{
    size_t at = strlen(dst);

    while (*src)
        dst[at++] = *src++;
    dst[at] = '\0';
}

int main(void)
{
    char args[128];

    if (sys_args(args, sizeof(args)) <= 0 || !args[0]) {
        put("Aufruf: abrufen <rechner> [pfad]\n");
        return 1;
    }

    /* Erstes Wort ist der Rechner, ein zweites waere der Pfad. */
    char host[96];
    char path[96];
    int i = 0, k = 0;

    while (args[i] == ' ')
        i++;
    while (args[i] && args[i] != ' ' && k < (int)sizeof(host) - 1)
        host[k++] = args[i++];
    host[k] = '\0';

    while (args[i] == ' ')
        i++;
    if (args[i]) {
        k = 0;
        while (args[i] && args[i] != ' ' && k < (int)sizeof(path) - 1)
            path[k++] = args[i++];
        path[k] = '\0';
    } else {
        strcpy(path, "/");
    }

    put("Verbinde mit ");
    put(host);
    put(" ...\n");

    int sock = net_connect(host, 80);

    if (sock < 0) {
        put("Die Verbindung kam nicht zustande.\n");
        return 1;
    }

    char request[320];

    request[0] = '\0';
    strcat_into(request, "GET ");
    strcat_into(request, path);
    strcat_into(request, " HTTP/1.1\r\nHost: ");
    strcat_into(request, host);
    strcat_into(request, "\r\nConnection: close\r\nUser-Agent: RetroOS\r\n\r\n");

    int len = (int)strlen(request);

    if (net_send(sock, request, (unsigned)len) < 0) {
        put("Die Anfrage ging nicht hinaus.\n");
        net_close(sock);
        return 1;
    }

    char buffer[1024];
    int total = 0;
    int idle = 0;

    for (;;) {
        int got = net_recv(sock, buffer, sizeof(buffer) - 1, 500);

        if (got > 0) {
            buffer[got] = '\0';
            /* Nur die ersten zwei Kilobyte anzeigen - die Konsole ist
             * nicht der richtige Ort fuer eine ganze Seite. */
            if (total < 1200)
                put(buffer);
            total += got;
            idle = 0;
        } else if (++idle > 8) {
            break;
        }
    }

    net_close(sock);

    put("\n--- ");
    char count[16];
    int n = total, digits = 0;
    char tmp[16];

    if (n == 0) {
        count[0] = '0';
        count[1] = '\0';
    } else {
        while (n > 0) { tmp[digits++] = (char)('0' + n % 10); n /= 10; }
        for (int d = 0; d < digits; d++)
            count[d] = tmp[digits - 1 - d];
        count[digits] = '\0';
    }
    put(count);
    put(" Byte empfangen ---\n");

    /* Der Konsole einen Augenblick lassen, die letzten Zeilen
     * abzuholen - sonst endet das Programm schneller, als sie liest. */
    sys_sleep(150);
    return 0;
}
