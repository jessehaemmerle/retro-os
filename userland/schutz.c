/* schutz.c - prueft, ob die Ausfuehrsperre wirklich greift.
 *
 * Das Programm legt eine Maschinenanweisung in ein Feld auf dem Stapel
 * und springt hinein. Auf einem System ohne Ausfuehrsperre gelaenge das
 * - und genau darauf bauen die meisten Angriffe: fremde Daten in einen
 * Puffer schreiben und dann anspringen.
 *
 * Hier soll es scheitern. Das Programm stirbt, das System laeuft weiter.
 */

#include "retroos.h"

int main(void)
{
    println("Ich lege eine Anweisung auf den Stapel und springe hinein.");
    println("Mit Ausfuehrsperre muss das schiefgehen.");
    sys_sleep(300);

    /* 0xC3 ist "ret" - die harmloseste Anweisung, die es gibt. Es geht
     * nicht darum, was dort steht, sondern wo es steht. */
    volatile unsigned char code[16];

    code[0] = 0xC3;

    void (*sprung)(void) = (void (*)(void))(unsigned long)code;

    sprung();

    println("Der Sprung ging durch - die Sperre fehlt.");
    return 1;
}
