/* winkel_test.c - prueft Sinus und Kosinus aus der Tabelle.
 *
 * Eine Tabelle laesst sich nicht gegen sich selbst pruefen. Was sich
 * pruefen laesst, sind die Beziehungen, die fuer jeden Winkel gelten
 * muessen: der Satz des Pythagoras, die Spiegelungen an den vier
 * Quadranten, und dass ausserhalb von 0 bis 360 dasselbe herauskommt
 * wie darin. Ein Vorzeichenfehler in einem Quadranten - der haeufigste
 * Fehler bei so einer Tabelle - faellt damit sofort auf.
 */

#include <stdio.h>
#include <stdlib.h>

#include "trig.h"

static int fehler;
static int geprueft;

static void pruefe(const char *was, bool bedingung)
{
    geprueft++;
    if (!bedingung) {
        printf("  FEHLER: %s\n", was);
        fehler++;
    }
}

static void test_bekannte(void)
{
    printf("Bekannte Werte\n");

    pruefe("sin 0", sin_deg(0) == 0);
    pruefe("sin 90", sin_deg(90) == TRIG_ONE);
    pruefe("sin 180", sin_deg(180) == 0);
    pruefe("sin 270", sin_deg(270) == -TRIG_ONE);
    pruefe("sin 360", sin_deg(360) == 0);

    pruefe("cos 0", cos_deg(0) == TRIG_ONE);
    pruefe("cos 90", cos_deg(90) == 0);
    pruefe("cos 180", cos_deg(180) == -TRIG_ONE);
    pruefe("cos 270", cos_deg(270) == 0);

    /* Ein halbes bei dreissig Grad - der Wert, an dem man eine
     * verrutschte Tabelle erkennt. */
    pruefe("sin 30 ist ein halbes", sin_deg(30) == 5000);
    pruefe("cos 60 ist ein halbes", cos_deg(60) == 5000);

    /* Wurzel aus zwei durch zwei bei 45 Grad. */
    pruefe("sin 45", sin_deg(45) == 7071);
    pruefe("cos 45", cos_deg(45) == 7071);
}

static void test_beziehungen(void)
{
    printf("Beziehungen\n");

    /* sin^2 + cos^2 = 1, bei jedem Winkel. Gerundet wird auf vier
     * Stellen, also darf es ein wenig danebenliegen. */
    long schlimmster = 0;

    for (int32_t d = -720; d <= 720; d++) {
        long s = sin_deg(d);
        long c = cos_deg(d);
        long sum = s * s + c * c;
        long soll = (long)TRIG_ONE * TRIG_ONE;
        long ab = sum > soll ? sum - soll : soll - sum;

        if (ab > schlimmster)
            schlimmster = ab;
    }
    geprueft++;
    if (schlimmster > 20000) {
        printf("  FEHLER: Pythagoras weicht um %ld ab\n", schlimmster);
        fehler++;
    }

    /* Punktsymmetrie und Achsensymmetrie. */
    bool spiegel_ok = true;
    bool phase_ok = true;

    for (int32_t d = 0; d <= 360; d++) {
        if (sin_deg(-d) != -sin_deg(d))
            spiegel_ok = false;
        if (cos_deg(-d) != cos_deg(d))
            spiegel_ok = false;
        if (sin_deg(d + 90) != cos_deg(d))
            phase_ok = false;
    }
    pruefe("sin ist punktsymmetrisch, cos achsensymmetrisch", spiegel_ok);
    pruefe("cos ist der verschobene sin", phase_ok);

    /* Alle 360 Grad wiederholt es sich - auch weit ausserhalb. */
    bool rund_ok = true;

    for (int32_t d = -1000; d <= 1000; d += 7)
        if (sin_deg(d) != sin_deg(d + 360) || sin_deg(d) != sin_deg(d - 720))
            rund_ok = false;
    pruefe("Der Umlauf stimmt", rund_ok);
}

static void test_quadranten(void)
{
    printf("Quadranten\n");

    /* Die Vorzeichen in den vier Vierteln - hier steckt der Fehler,
     * den man sonst erst am schiefen Zifferblatt bemerkt. */
    bool ok = true;

    for (int32_t d = 1; d < 90; d++)
        if (sin_deg(d) <= 0 || cos_deg(d) <= 0)
            ok = false;
    pruefe("Erstes Viertel: beide positiv", ok);

    ok = true;
    for (int32_t d = 91; d < 180; d++)
        if (sin_deg(d) <= 0 || cos_deg(d) >= 0)
            ok = false;
    pruefe("Zweites Viertel: sin positiv, cos negativ", ok);

    ok = true;
    for (int32_t d = 181; d < 270; d++)
        if (sin_deg(d) >= 0 || cos_deg(d) >= 0)
            ok = false;
    pruefe("Drittes Viertel: beide negativ", ok);

    ok = true;
    for (int32_t d = 271; d < 360; d++)
        if (sin_deg(d) >= 0 || cos_deg(d) <= 0)
            ok = false;
    pruefe("Viertes Viertel: sin negativ, cos positiv", ok);

    /* Und nichts faellt aus dem Bereich. */
    ok = true;
    for (int32_t d = -720; d <= 720; d++)
        if (sin_deg(d) > TRIG_ONE || sin_deg(d) < -TRIG_ONE)
            ok = false;
    pruefe("Nichts groesser als eins", ok);
}

static void test_zeiger(void)
{
    printf("Zeiger einer Uhr\n");

    /* Was die Uhr damit rechnet: Der Zeiger auf zwoelf zeigt nach
     * oben, auf drei nach rechts, auf sechs nach unten. Im Bild
     * zaehlt y nach unten, darum das Minus beim Kosinus. */
    struct { int32_t deg, dx, dy; } proben[] = {
        {   0,  0, -100 },
        {  90, 100,   0 },
        { 180,  0,  100 },
        { 270, -100,  0 },
    };

    for (size_t i = 0; i < sizeof(proben) / sizeof(proben[0]); i++) {
        int32_t x = sin_deg(proben[i].deg) * 100 / TRIG_ONE;
        int32_t y = -cos_deg(proben[i].deg) * 100 / TRIG_ONE;

        geprueft++;
        if (x != proben[i].dx || y != proben[i].dy) {
            printf("  FEHLER: %d Grad -> %d,%d statt %d,%d\n",
                   proben[i].deg, x, y, proben[i].dx, proben[i].dy);
            fehler++;
        }
    }
}

int main(void)
{
    printf("=== Winkel ===\n");

    test_bekannte();
    test_beziehungen();
    test_quadranten();
    test_zeiger();

    printf("\n%d Pruefungen, %d Fehler\n", geprueft, fehler);
    return fehler ? 1 : 0;
}
