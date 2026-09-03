/* display.h - Aufloesung und Vergroesserung der Oberflaeche.
 *
 * Zwei Dinge, die sich leicht verwechseln lassen.
 *
 * Die **Aufloesung** ist, was die Grafikkarte anzeigt. Sie zu aendern
 * heisst, der Karte einen anderen Modus zu setzen; das geht nur, wo es
 * die Bochs-Schnittstelle gibt (siehe vbe.h).
 *
 * Die **Vergroesserung** ist etwas ganz anderes und geht ueberall. Der
 * Backbuffer wird dabei kleiner als der Bildschirm - bei zweifacher
 * Vergroesserung halb so breit und halb so hoch -, und beim Ausgeben
 * wird jeder Punkt zu einem Quadrat. Die ganze Oberflaeche rechnet
 * weiter in Punkten dieses Backbuffers und merkt nichts davon. Genau
 * darum ist es so billig: Ein Zeichen bleibt 8x16 Punkte gross, aber
 * auf einem 4K-Schirm ist es endlich wieder zu lesen.
 *
 * Vergroessert wird ganzzahlig. Alles andere hiesse, jeden Punkt zu
 * mitteln, und aus einer gestochenen Kante wuerde Matsch - bei einer
 * Oberflaeche aus einem Pixel breiten Linien ist das kein
 * Schoenheitsfehler, sondern das Ende der Lesbarkeit.
 */
#ifndef DISPLAY_H
#define DISPLAY_H

#include "retro.h"
#include "displayutil.h"

/* Einmalig nach fb_init(). Legt den Backbuffer an und stellt die
 * Vergroesserung ein. scale 0 heisst "automatisch". */
bool display_init(uint32_t scale);

/* Was die Karte gerade anzeigt. */
int32_t display_width(void);
int32_t display_height(void);

/* Was die Oberflaeche sieht - das ist Aufloesung geteilt durch
 * Vergroesserung. */
int32_t display_logical_width(void);
int32_t display_logical_height(void);

uint32_t display_scale(void);
/* true, wenn die Vergroesserung nicht von Hand gesetzt wurde. */
bool     display_scale_is_auto(void);

/* Laesst sich die Aufloesung ueberhaupt aendern? */
bool display_can_switch(void);

/* Die Modi, die diese Karte anbietet - gefiltert nach Grafikspeicher.
 * Der laufende ist immer dabei, auch wenn er nicht in der Liste der
 * gaengigen steht. */
size_t                  display_mode_count(void);
const struct disp_mode *display_mode_at(size_t index);
size_t                  display_current_mode(void);

/* Setzt einen Modus. Bei Misserfolg bleibt alles, wie es war. */
bool display_set_mode(int32_t w, int32_t h);
/* Setzt die Vergroesserung; 0 heisst wieder automatisch. */
bool display_set_scale(uint32_t scale);

#endif /* DISPLAY_H */
