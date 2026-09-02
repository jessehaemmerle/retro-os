/* perm.h - wer darf was mit einer Datei.
 *
 * Jeder Knoten im Dateibaum traegt einen Eigentuemer, eine Gruppe und
 * neun Rechtebits: lesen, schreiben und ausfuehren, je einmal fuer den
 * Eigentuemer, die Gruppe und alle uebrigen. Das ist das Modell, das
 * Unix seit fuenfzig Jahren benutzt, und es ist deshalb so haltbar,
 * weil es mit drei Zahlen auskommt und trotzdem fuer den Alltag reicht.
 *
 * Bei Ordnern bedeuten die Bits etwas anderes als bei Dateien:
 *
 *   r  die Eintraege aufzaehlen duerfen
 *   w  darin anlegen, umbenennen und loeschen duerfen
 *   x  hindurchgehen duerfen, also einen Namen darin nachschlagen
 *
 * Dazu kommt das Klebebit (sticky). Steht es auf einem Ordner, darf
 * dort zwar jeder ablegen, aber nur der Eigentuemer eines Eintrags ihn
 * wieder wegnehmen. Genau das braucht der Papierkorb, in den alle
 * werfen und aus dem trotzdem niemand fremde Sachen holen soll.
 *
 * root und jeder andere Verwalter gehen an den Bits vorbei.
 *
 * FAT32 kennt nichts davon. Damit die Rechte auf der Platte trotzdem
 * einen Neustart ueberstehen, liegt daneben eine Liste
 * /Festplatte/rechte.conf, die Pfad fuer Pfad festhaelt, was gelten
 * soll. Knoten im Arbeitsspeicher brauchen das nicht - sie entstehen
 * bei jedem Start neu.
 */
#ifndef PERM_H
#define PERM_H

#include "retro.h"
#include "vfs.h"

#define PERM_PATH   "/Festplatte/rechte.conf"

#define P_R 04
#define P_W 02
#define P_X 01

#define MODE_MASK    00777
#define MODE_STICKY  01000

/* Was ein neu angelegter Eintrag bekommt. */
#define MODE_FILE_DEFAULT 0644
#define MODE_DIR_DEFAULT  0755

/* Darf (uid, gid) das? want ist eine Kombination aus P_R, P_W und P_X. */
bool perm_check(const struct fs_node *node, uint32_t uid, uint32_t gid,
                uint8_t want);
/* Dasselbe fuer den, der gerade handelt. */
bool perm_may(const struct fs_node *node, uint8_t want);
/* Eigentuemer oder Verwalter - nur die duerfen Rechte aendern. */
bool perm_owns(const struct fs_node *node);
/* Darf node aus dir entfernt werden? Beachtet das Klebebit. */
bool perm_may_unlink(const struct fs_node *dir, const struct fs_node *node);

/* Setzt Rechte beziehungsweise Eigentuemer. Liefert false, wenn der
 * Handelnde das nicht darf. Beides merkt sich die Liste auf der Platte. */
bool perm_set_mode(struct fs_node *node, uint16_t mode);
bool perm_set_owner(struct fs_node *node, uint32_t uid, uint32_t gid);

/* "drwxr-x---" - zehn Zeichen und ein Abschluss. */
void perm_mode_text(uint16_t mode, uint8_t type, char out[11]);
/* Nimmt "750", "0750" oder "rwxr-x---" entgegen. */
bool perm_parse_mode(const char *text, uint16_t *out);

/* --- gespeicherte Rechte auf der Platte ---------------------------- */

void perm_store_load(void);
/* Setzt bei einem frisch von der Platte gelesenen Knoten die
 * gespeicherten Werte, falls es welche gibt. */
void perm_store_apply(struct fs_node *node);
/* Haelt die Rechte eines Knotens fest und merkt sich, dass gespeichert
 * werden muss. */
void perm_store_record(struct fs_node *node);
/* Vergisst einen Pfad wieder - beim Loeschen. */
void perm_store_forget(const struct fs_node *node);
bool perm_store_save(void);
bool perm_store_dirty(void);

/* --- Arbeiten des Systems selbst ------------------------------------ */

/* Waehrend das System seine eigenen Dateien fuehrt - Einstellungen
 * sichern, den Papierkorb pflegen, die Benutzerdatenbank schreiben -
 * gelten die Rechte nicht. Immer paarweise verwenden. */
void perm_system_begin(void);
void perm_system_end(void);
bool perm_system_active(void);

#endif /* PERM_H */
