/* liste.c - zeigt den Inhalt eines Ordners. */

#include "retroos.h"

int main(void)
{
    char path[128];

    if (sys_args(path, sizeof(path)) <= 0 || !path[0])
        strcpy(path, "/");

    printf("Inhalt von %s:\n", path);

    char name[96];
    int total = 0;

    for (unsigned i = 0; i < 256; i++) {
        int count = sys_readdir(path, i, name, sizeof(name));

        if (count <= 0)
            break;
        total = count;
        printf("  %s\n", name);
        if ((int)i + 1 >= count)
            break;
    }

    if (total == 0)
        println("  (leer oder nicht gefunden)");
    return 0;
}
