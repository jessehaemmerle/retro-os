/* roehre.c - zeigt Roehren und geteilten Speicher.
 *
 * Das Programm spaltet sich ab. Das Kind schreibt eine Reihe von
 * Zeilen in die Roehre und legt nebenbei eine Zahl in den geteilten
 * Bereich; der Elternteil liest mit, bis das Kind seine Seite
 * schliesst, und sieht danach die Zahl, ohne dass sie je durch den Kern
 * gelaufen waere.
 *
 * Beide Wege stehen absichtlich nebeneinander: Die Roehre traegt die
 * Nachricht, der geteilte Bereich die Menge. So sieht man den
 * Unterschied - der eine kopiert und ordnet, der andere kopiert nicht
 * und ordnet auch nicht.
 */

#include "retroos.h"
#include "retroui.h"

#define RUNDEN 8

struct ablage {
    volatile int  fertig;
    volatile long summe;
    char          gruss[32];
};

int main(void)
{
    int paar[2];

    if (sys_pipe(paar) < 0) {
        println("Die Roehre liess sich nicht anlegen.");
        return 1;
    }

    int bereich = sys_shm_open("roehre-demo", sizeof(struct ablage), 1);

    if (bereich < 0) {
        println("Der geteilte Bereich liess sich nicht anlegen.");
        return 1;
    }

    struct ablage *gemeinsam = sys_shm_map(bereich);

    if (!gemeinsam) {
        println("Der Bereich liess sich nicht einblenden.");
        return 1;
    }

    memset(gemeinsam, 0, sizeof(*gemeinsam));

    int kind = sys_fork();

    if (kind < 0) {
        println("Das Abspalten ist gescheitert.");
        return 1;
    }

    if (kind == 0) {
        /* Das Kind blendet den Bereich neu ein: Geerbt wird er nicht,
         * sonst waere er nach dem ersten Schreiben zwei getrennte
         * Bereiche - und niemand haette es gemerkt. */
        struct ablage *meins = sys_shm_map(bereich);

        sys_close(paar[0]);          /* das Lesende braucht es nicht */

        long summe = 0;

        for (int i = 1; i <= RUNDEN; i++) {
            char zeile[32];
            int  n = 0;

            zeile[n++] = 'R'; zeile[n++] = 'u'; zeile[n++] = 'n';
            zeile[n++] = 'd'; zeile[n++] = 'e'; zeile[n++] = ' ';
            zeile[n++] = (char)('0' + i);
            zeile[n++] = '\n';

            sys_write(paar[1], zeile, n);
            summe += (long)i * i;
            meins->summe = summe;
            sys_sleep(30);
        }

        strcpy(meins->gruss, "Gruss aus dem Kind");
        meins->fertig = 1;

        sys_close(paar[1]);          /* jetzt merkt der Leser das Ende */
        sys_exit(0);
    }

    /* Der Elternteil liest, bis nichts mehr kommt. Sein eigenes
     * Schreibende muss vorher zu sein - sonst waere immer noch ein
     * Schreiber da, und die Roehre endete nie. */
    sys_close(paar[1]);

    println("Aus der Roehre:");

    char puffer[128];
    int  gelesen;
    long gesamt = 0;

    while ((gelesen = sys_read(paar[0], puffer, sizeof(puffer))) > 0) {
        sys_write(1, puffer, gelesen);
        gesamt += gelesen;
    }

    sys_close(paar[0]);

    int code = 0;

    sys_wait(kind, &code, 3000);

    printf("\nGelesen: %d Bytes\n", (int)gesamt);
    printf("Aus dem geteilten Bereich: Summe %d, fertig %d\n",
           (int)gemeinsam->summe, gemeinsam->fertig);
    println(gemeinsam->gruss);

    /* Die Summe der Quadratzahlen von 1 bis 8 ist 204. Steht sie da,
     * hat das Kind wirklich in dieselben Seitenrahmen geschrieben. */
    println(gemeinsam->summe == 204 && gemeinsam->fertig == 1
            ? "Der Bereich ist wirklich geteilt."
            : "Da stimmt etwas nicht.");

    sys_shm_unlink("roehre-demo");
    return 0;
}
