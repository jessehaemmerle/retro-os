/* kaefig.c - zeigt, was ein eingesperrtes Programm noch darf.
 *
 * Das Programm sperrt sich selbst ein und probiert danach der Reihe
 * nach aus, was noch geht: rechnen, ausgeben, eine Datei oeffnen, ins
 * Netz gehen, sich abspalten. Was der Kaefig verbietet, meldet einen
 * Fehler statt zu wirken - und genau das soll man sehen koennen.
 *
 * Ohne Argument nimmt es "netz". Mit "streng" beendet der Kern es beim
 * ersten Verstoss; auch das laesst sich so vorfuehren.
 */

#include "retroos.h"
#include "retroui.h"

/* -7 ist SYS_ERR_DENIED: der Kaefig war es. Jeder andere negative Wert
 * heisst, dass es aus einem gewoehnlichen Grund nicht ging - eine
 * fehlende Datei etwa oder ein Rechner, der nicht antwortet. Beides
 * auseinanderzuhalten ist der ganze Sinn dieser Vorfuehrung. */
static void probiere(const char *was, long ergebnis)
{
    print("  ");
    print(was);
    print(": ");

    if (ergebnis == -7)
        println("vom Kaefig verboten");
    else if (ergebnis < 0)
        printf("ging nicht (%d)\n", (int)ergebnis);
    else
        printf("geht (%d)\n", (int)ergebnis);
}

int main(void)
{
    char profil[32];

    if (sys_args(profil, sizeof(profil)) <= 0 || !profil[0])
        strcpy(profil, "netz");

    println("Vor dem Einsperren:");
    probiere("Datei lesen",   sys_open("/System/version.txt", 0));
    probiere("Datei anlegen", sys_open("/Temp/kaefig.txt", 1));

    if (sys_sandbox(profil) < 0) {
        printf("Das Profil \"%s\" gibt es nicht.\n", profil);
        return 1;
    }

    printf("Eingesperrt in \"%s\". Jetzt noch einmal:\n", profil);

    /* Rechnen und Ausgeben geht in jedem Profil - sonst koennte das
     * Programm sein Ergebnis nicht einmal mitteilen. */
    long summe = 0;

    for (int i = 1; i <= 100; i++)
        summe += i;
    printf("  rechnen: geht (%d)\n", (int)summe);

    probiere("Datei lesen",   sys_open("/System/version.txt", 0));
    probiere("Datei anlegen", sys_open("/Temp/kaefig.txt", 1));
    probiere("ins Netz",      net_connect("10.0.2.2", 80));
    probiere("abspalten",     sys_fork());
    probiere("eigene Nummer", sys_getpid());

    println("Fertig.");
    return 0;
}
