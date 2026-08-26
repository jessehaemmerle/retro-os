/* schreiben.c - legt eine Datei an und schreibt hinein.
 * Zeigt, dass ein Benutzerprogramm dauerhaft etwas ablegen kann. */

#include "retroos.h"

int main(void)
{
    char path[128];

    if (sys_args(path, sizeof(path)) <= 0 || !path[0])
        strcpy(path, "/Temp/notiz.txt");

    int fd = sys_open(path, 1);

    if (fd < 0) {
        printf("Konnte %s nicht anlegen.\n", path);
        return 1;
    }

    char zeile[128];
    int laenge = 0;
    const char *text = "Geschrieben von einem Programm in Ring 3.\n";

    while (text[laenge])
        laenge++;
    memcpy(zeile, text, (size_t)laenge);

    sys_write(fd, zeile, (size_t)laenge);
    sys_close(fd);

    printf("In %s geschrieben.\n", path);
    return 0;
}
