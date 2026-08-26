/* absturz.c - greift absichtlich auf eine Adresse zu, die es nicht gibt.
 *
 * Das Programm stirbt, das System laeuft weiter - genau dafuer gibt es die
 * Trennung zwischen Ring 0 und Ring 3. Vor der Trennung haette derselbe
 * Zugriff den ganzen Rechner angehalten.
 */

#include "retroos.h"

int main(void)
{
    println("Ich schreibe jetzt an die Adresse 0. Das wird schiefgehen.");
    sys_sleep(300);

    volatile int *nirgendwo = (volatile int *)0;

    *nirgendwo = 42;

    println("Diese Zeile wird nie erscheinen.");
    return 0;
}
