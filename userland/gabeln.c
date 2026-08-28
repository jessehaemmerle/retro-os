/* gabeln.c - zeigt, was beim Abspalten eines Prozesses geschieht.
 *
 * Der Aufruf sys_fork() kehrt zweimal zurueck: einmal im Elternteil
 * mit der Nummer des Kindes, einmal im Kind mit 0. Ab da laufen beide
 * dasselbe Programm, aber jeder fuer sich.
 *
 * Was sie sich anfangs teilen, ist der Speicher - Seite fuer Seite,
 * ohne dass etwas kopiert wuerde. Erst der erste Schreibzugriff loest
 * die Verdopplung aus. Genau das zeigt der Zaehler unten: Beide
 * beginnen beim selben Wert und sehen danach nur noch ihre eigene
 * Fassung davon.
 *
 *     starte gabeln        drei Kinder, jedes zaehlt fuer sich
 *     starte gabeln 6      sechs Kinder
 */

#include "retroos.h"
#include "retroui.h"

#define KINDER_STANDARD 3
#define KINDER_MAX      8

/* Absichtlich global: Die Seite, auf der das hier liegt, wird beim
 * Abspalten geteilt - bis jemand hineinschreibt. */
static int zaehler = 1000;
static char notiz[64] = "unberuehrt";

int main(void)
{
    char args[64];
    int kinder = KINDER_STANDARD;

    if (sys_args(args, sizeof(args)) > 0 && args[0] >= '0' && args[0] <= '9') {
        kinder = atoi(args);
        if (kinder < 1)
            kinder = 1;
        if (kinder > KINDER_MAX)
            kinder = KINDER_MAX;
    }

    printf("Elternteil ist Nummer %d, Zaehler steht auf %d.\n",
           sys_getpid(), zaehler);

    for (int i = 0; i < kinder; i++) {
        int pid = sys_fork();

        if (pid < 0) {
            printf("Abspalten ging nicht mehr.\n");
            break;
        }

        if (pid == 0) {
            /* Ab hier ist das das Kind. Der erste Schreibzugriff auf
             * den Zaehler gibt ihm seine eigene Seite. */
            zaehler += (i + 1) * 10;
            strcpy(notiz, "vom Kind beschrieben");

            printf("  Kind %d (Nummer %d): Zaehler %d, Notiz \"%s\"\n",
                   i + 1, sys_getpid(), zaehler, notiz);

            sys_sleep(50 * (unsigned)(i + 1));
            printf("  Kind %d ist fertig.\n", i + 1);
            sys_exit(i + 1);
        }

        printf("Kind %d hat die Nummer %d.\n", i + 1, pid);
    }

    /* Auf alle Kinder warten und ihren Ausgang einsammeln. */
    for (;;) {
        int code = 0;
        int pid = sys_wait(0, &code, 2000);

        if (pid < 0)
            break;              /* keine Kinder mehr */
        if (pid == 0) {
            printf("Ein Kind laesst sich Zeit.\n");
            continue;
        }
        printf("Nummer %d ist beendet, Ergebnis %d.\n", pid, code);
    }

    printf("Elternteil sieht immer noch Zaehler %d und Notiz \"%s\".\n",
           zaehler, notiz);
    return 0;
}
