/* hallo.c - zeigt, dass ein eigenstaendiges Programm laeuft. */

#include "retroos.h"

int main(void)
{
    char args[128];

    println("Hallo aus einem Benutzerprogramm!");
    printf("Ich laufe in Ring 3, in meinem eigenen Adressraum.\n");
    printf("Meine Nummer ist %d, das System laeuft seit %u ms.\n",
           (long)sys_getpid(), sys_uptime());

    if (sys_args(args, sizeof(args)) > 0 && args[0])
        printf("Uebergeben wurde: \"%s\"\n", args);
    else
        println("Es wurde nichts uebergeben.");

    println("Fertig.");
    return 0;
}
