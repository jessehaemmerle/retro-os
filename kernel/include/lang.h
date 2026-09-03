/* lang.h - die Sprache der Oberflaeche.
 *
 * RetroOS ist auf Deutsch geschrieben, und zwar bis in die Bezeichner
 * hinein. Eine zweite Sprache nachtraeglich einzuziehen heisst darum
 * nicht, Nummern gegen Texte zu tauschen - es heisst, den deutschen
 * Text selbst zum Schluessel zu machen: tr("Einstellungen") liefert
 * "Settings", wenn Englisch eingestellt ist, und sonst genau das, was
 * hineinging.
 *
 * Das hat zwei Vorteile, die den einen Nachteil aufwiegen. Erstens
 * bleibt der Quelltext lesbar: Wer ihn liest, sieht den Text, den der
 * Benutzer sieht, und nicht STR_SETTINGS_TITLE. Zweitens ist ein
 * fehlender Eintrag kein Absturz und kein leeres Feld, sondern eine
 * deutsche Zeile in einem englischen Fenster - unschoen, aber
 * bedienbar. Der Nachteil ist die Suche bei jedem Aufruf; sie laeuft
 * binaer ueber eine sortierte Tabelle und kostet ein Dutzend
 * Vergleiche, waehrend das Zeichnen der Zeile hunderte Pixel kostet.
 *
 * Uebersetzt wird beim Zeichnen, nicht beim Anlegen. Nur so wechselt
 * ein offenes Fenster die Sprache mit, statt sie bis zum Neustart zu
 * behalten.
 */
#ifndef LANG_H
#define LANG_H

#include "retro.h"

enum language {
    LANG_DE,
    LANG_EN,
    LANG_COUNT
};

enum language lang_current(void);
void          lang_select(enum language lang);

/* Waehlt nach Kuerzel ("de", "en"); unbekannte aendern nichts. */
bool          lang_select_by_code(const char *code);
const char   *lang_code(enum language lang);
const char   *lang_name(enum language lang);

/* Die Belegung, die zu einer Sprache am ehesten passt. */
const char   *lang_default_keymap(enum language lang);

/* Uebersetzt einen deutschen Text. Ohne Eintrag kommt er unveraendert
 * zurueck. NULL bleibt NULL. */
const char *tr(const char *german);

/* --- fuer die Pruefungen ------------------------------------------- */

size_t      lang_entry_count(void);
const char *lang_entry_de(size_t index);
const char *lang_entry_en(size_t index);

/* Wie tr(), aber unabhaengig von der eingestellten Sprache und mit
 * NULL, wenn es keinen Eintrag gibt. */
const char *lang_lookup(const char *german);

#endif /* LANG_H */
