/* trash.h - der Papierkorb.
 *
 * Loeschen ist endgueltig, und das ist selten das, was gemeint war.
 * Deshalb wandert alles, was geloescht wird, zuerst nach /Papierkorb
 * und bleibt dort, bis es entweder zurueckgeholt oder der Korb
 * geleert wird.
 *
 * Damit sich ein Eintrag zurueckholen laesst, merkt sich der Korb, wo
 * er hergekommen ist. Diese Liste liegt neben den Dateien in der Datei
 * ".herkunft" - so ueberlebt sie einen Neustart, wenn der Papierkorb
 * auf einer Festplatte liegt.
 *
 * Gleichnamiges aus verschiedenen Ordnern stoert nicht: Der Korb
 * haengt eine Nummer an, wenn ein Name schon belegt ist.
 */
#ifndef TRASH_H
#define TRASH_H

#include "retro.h"
#include "vfs.h"

#define TRASH_PATH "/Papierkorb"

void trash_init(void);

/* Der Ordner selbst - fuer Dateimanager und Desktop. */
struct fs_node *trash_dir(void);
/* Liegt der Knoten im Papierkorb (oder ist er der Korb selbst)? */
bool trash_contains(const struct fs_node *node);

/* Verschiebt einen Eintrag in den Papierkorb. Liefert false, wenn er
 * geschuetzt ist oder schon drinliegt - dann hilft nur trash_purge. */
bool trash_delete(struct fs_node *node);

/* Holt einen Eintrag an seinen alten Platz zurueck. Ist der Ordner
 * verschwunden, wird er neu angelegt. */
bool trash_restore(struct fs_node *node);

/* Loescht einen einzelnen Eintrag endgueltig. */
bool trash_purge(struct fs_node *node);
/* Leert den ganzen Korb; liefert die Anzahl der geloeschten Eintraege. */
size_t trash_empty(void);

size_t trash_count(void);
size_t trash_bytes(void);
/* Woher der Eintrag stammt; leerer Text, wenn es niemand mehr weiss. */
const char *trash_origin(const struct fs_node *node);

#endif /* TRASH_H */
