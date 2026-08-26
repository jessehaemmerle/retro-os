/* zaehler.c - zaehlt langsam und zeigt dabei, dass alles andere
 * weiterlaeuft: die Oberflaeche, das Netzwerk und weitere Programme. */

#include "retroos.h"

int main(void)
{
    char args[64];
    int limit = 10;

    if (sys_args(args, sizeof(args)) > 0 && args[0]) {
        int value = atoi(args);

        if (value > 0 && value <= 100)
            limit = value;
    }

    printf("Zaehle bis %d, eine Zahl je halbe Sekunde.\n", (long)limit);

    for (int i = 1; i <= limit; i++) {
        printf("  %3d von %d\n", (long)i, (long)limit);
        sys_sleep(500);
    }

    println("Durchgezaehlt.");
    return 0;
}
