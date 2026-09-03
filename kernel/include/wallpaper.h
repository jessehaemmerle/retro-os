/* wallpaper.h - ein eigenes Bild als Hintergrund.
 *
 * Die fuenf eingebauten Verlaeufe kosten nichts und sind sofort da.
 * Ein Bild ist etwas anderes: Es muss geladen, entpackt und auf die
 * Bildschirmgroesse gebracht werden, und danach liegt es als ein
 * Stueck im Speicher. Genau einmal - der Hintergrund wird bei jeder
 * Bewegung eines Fensters neu gezeichnet, und ein PNG bei jedem
 * Mausschubser aufs Neue zu entpacken waere die sicherste Art, die
 * Oberflaeche zaeh zu machen.
 *
 * Skaliert wird fuellend und mittig beschnitten: Das Bild bedeckt die
 * Flaeche ganz und behaelt sein Seitenverhaeltnis. Der Rest haengt
 * ueber den Rand hinaus und wird beim Zeichnen abgeschnitten - das
 * kostet nichts, weil ohnehin auf die Flaeche begrenzt wird.
 */
#ifndef WALLPAPER_H
#define WALLPAPER_H

#include "retro.h"
#include "gfx.h"

/* Laedt das Bild und merkt es sich. Ein leerer Pfad oder NULL nimmt
 * das Bild wieder weg; dann gilt wieder der Verlauf. */
bool wallpaper_set(const char *path);

/* Der zuletzt erfolgreich gesetzte Pfad, "" wenn keiner. */
const char *wallpaper_path(void);

/* Zeichnet das Bild in die Flaeche. false heisst: es gibt keines,
 * der Aufrufer soll seinen Verlauf malen. */
bool wallpaper_draw(struct canvas *c, struct rect area);

#endif /* WALLPAPER_H */
