/* trig.h - Sinus und Kosinus in ganzen Zahlen.
 *
 * Der Kern schaltet die Recheneinheit fuer Gleitkommazahlen gar nicht
 * erst ein - sie zu benutzen hiesse, ihren Zustand bei jedem
 * Threadwechsel mitzuschleppen, und dafuer gibt es hier nichts zu
 * rechnen. Fuer die Zeiger einer Uhr braucht es trotzdem einen Sinus.
 *
 * Also eine Tabelle: 91 Werte fuer 0 bis 90 Grad, mit 10000
 * vervielfacht. Alles darueber ergibt sich aus den Spiegelungen. Ein
 * Grad Schrittweite reicht - der Sekundenzeiger einer Uhr springt
 * ohnehin, und auf einem Zifferblatt von 200 Punkten ist ein Grad
 * weniger als zwei Punkte.
 */
#ifndef TRIG_H
#define TRIG_H

#include "retro.h"

#define TRIG_ONE 10000

/* Winkel in Grad, auch negativ und ueber 360 hinaus. */
int32_t sin_deg(int32_t degrees);
int32_t cos_deg(int32_t degrees);

#endif /* TRIG_H */
