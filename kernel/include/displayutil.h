/* displayutil.h - das Rechnen rund um Aufloesung und Skalierung.
 *
 * Getrennt vom Treiber, weil sich genau hier die Fehler verstecken:
 * beim Zerlegen von "1280x800", bei der Frage, welche Vergroesserung
 * auf einem Bildschirm noch sinnvoll ist, und dabei, ob ein Modus
 * ueberhaupt in den Grafikspeicher passt. Das laesst sich auf dem
 * Entwicklungsrechner pruefen, das Setzen des Modus nicht.
 */
#ifndef DISPLAYUTIL_H
#define DISPLAYUTIL_H

#include "retro.h"

struct disp_mode {
    int32_t w, h;
};

/* "1280x800" oder "1280x800x32" - die Farbtiefe wird gelesen und
 * verworfen, RetroOS zeichnet immer mit 32 Bit. false bei Unsinn. */
bool disp_parse_mode(const char *text, int32_t *w, int32_t *h);

/* Schreibt "1280x800". */
void disp_format_mode(char *out, size_t size, int32_t w, int32_t h);

/* Die Vergroesserung, die zu einem Bildschirm passt. Sie richtet sich
 * nach der Hoehe: Ein 8x16-Zeichen ist auf 2160 Zeilen sonst ein
 * Staubkorn. */
uint32_t disp_auto_scale(int32_t w, int32_t h);

/* Mehr als das geht nicht, ohne dass die Arbeitsflaeche zu klein wird -
 * unter 640x400 logischen Punkten passt kein Fenster mehr sinnvoll
 * hin. */
uint32_t disp_max_scale(int32_t w, int32_t h);

/* Passt der Modus in so viel Grafikspeicher? Gerechnet wird mit 32 Bit
 * je Punkt, denn anders zeichnet RetroOS nicht. */
bool disp_mode_fits(int32_t w, int32_t h, uint64_t vram_bytes);

/* Die gaengigen Modi, aufsteigend. Liefert, wie viele geschrieben
 * wurden. */
size_t disp_standard_modes(struct disp_mode *out, size_t max);

/* Schiebt ein Fenster in einen kleiner gewordenen Bildschirm zurueck.
 * Breite und Hoehe werden dabei nur verkleinert, wenn es anders nicht
 * geht - ein Fenster, das gerade noch passt, soll seine Groesse
 * behalten. */
void disp_fit_window(int32_t *x, int32_t *y, int32_t *w, int32_t *h,
                     int32_t screen_w, int32_t screen_h);

#endif /* DISPLAYUTIL_H */
