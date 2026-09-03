/* config.h - Einstellungen, die einen Neustart ueberleben.
 *
 * Solange RetroOS nur vom Stick lief, war jede Einstellung nach dem
 * Ausschalten wieder weg - da lohnte sich das Merken nicht. Mit einer
 * Installation auf der Festplatte ist das anders: Was der Benutzer
 * einstellt, soll bleiben.
 *
 * Abgelegt wird es als Text unter /Festplatte/retroos.conf, ein
 * Schluessel je Zeile:
 *
 *     sprache = de
 *     tastatur = de
 *     uhr = lokal
 *     zeitzone = 60
 *     rechnername = retroos
 *     hintergrund = 2
 *     hintergrundbild = /Medien/muster.png
 *     schrift = DejaVu Sans Mono
 *
 * Textform, weil man sie dann mit dem Editor des Systems selbst
 * reparieren kann - und von einem anderen Rechner aus ebenso.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "retro.h"

#define CONFIG_PATH "/Festplatte/retroos.conf"

/* Wie die Batterieuhr zu lesen ist. Ein Rechner, auf dem auch Windows
 * laeuft, stellt sie meist auf Ortszeit; einer mit Linux auf UTC. */
enum clock_mode {
    CLOCK_LOCAL,
    CLOCK_UTC,
};

struct config {
    char            language[8];     /* "de", "en"                  */
    char            keymap[8];       /* "de", "us", "uk", "ch"      */
    enum clock_mode clock;
    int32_t         timezone;        /* Minuten gegenueber UTC      */
    char            hostname[32];
    uint32_t        background;      /* Nummer des Verlaufs         */
    char            wallpaper[64];   /* eigenes Bild, leer = keins  */
    char            font[24];        /* Name der Bildschirmschrift  */
};

/* Die aktuellen Werte. Aendern und danach config_save() rufen. */
struct config *config_current(void);

/* Setzt alles auf die Werkseinstellung. */
void config_defaults(void);

/* Liest die Datei, falls es sie gibt, und wendet sie an. Ohne Datei
 * bleibt es bei der Werkseinstellung - das ist kein Fehler. */
bool config_load(void);

/* Schreibt die Datei. Ohne eingehaengte Festplatte geht das nicht. */
bool config_save(void);

/* Uebertraegt die Werte dorthin, wo sie wirken: Sprache,
 * Tastaturbelegung, Rechnername, Hintergrund, Schriftart. */
void config_apply(void);

/* Die waehlbaren Hintergruende. */
size_t      background_count(void);
const char *background_name(size_t index);
void        background_colors(size_t index, uint32_t *top, uint32_t *bottom);

#endif /* CONFIG_H */
