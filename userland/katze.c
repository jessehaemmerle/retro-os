/* katze.c - gibt eine Datei aus. Zeigt die Dateisystem-Aufrufe. */

#include "retroos.h"

int main(void)
{
    char path[128];

    if (sys_args(path, sizeof(path)) <= 0 || !path[0]) {
        println("Aufruf: katze <datei>");
        return 1;
    }

    int fd = sys_open(path, 0);

    if (fd < 0) {
        printf("Datei nicht gefunden: %s\n", path);
        return 1;
    }

    long size = sys_filesize(fd);
    printf("--- %s (%d Byte) ---\n", path, size);

    char buffer[256];
    ssize_t n;

    while ((n = sys_read(fd, buffer, sizeof(buffer))) > 0)
        sys_write(1, buffer, (size_t)n);

    sys_close(fd);
    println("");
    return 0;
}
