/* formutil.h - Formulare abschicken: Felder kodieren und zusammensetzen.
 *
 * Ein Formular wird als Folge "name=wert" geschickt, getrennt durch "&",
 * mit Prozentkodierung fuer alles ausser Buchstaben, Ziffern und
 * "-._*". Ein Leerzeichen wird zum Pluszeichen. Wer hier ein Zeichen
 * vergisst, schickt der Suchmaschine ein "&" mitten im Suchwort - die
 * Anfrage "Katz & Maus" waere dann zwei Felder.
 *
 * Die Arbeit steht hier ohne Dokumentbaum und ohne Netz und wird auf dem
 * Entwicklungsrechner geprueft.
 */
#ifndef FORMUTIL_H
#define FORMUTIL_H

#include "retro.h"

/* Haengt "name=wert" (mit "&" davor, wenn schon etwas da ist) an. Name
 * und Wert werden kodiert. false, wenn es nicht mehr hineinpasst - dann
 * bleibt out unveraendert. */
bool form_append(char *out, size_t size, const char *name, const char *value);

/* Kodiert einen Text fuer eine Anfrage. Gibt die Laenge zurueck; passt es
 * nicht, kommt (size_t)-1 und out ist leer. */
size_t form_encode(char *out, size_t size, const char *text);

/* Setzt die Adresse eines GET-Formulars zusammen: Pfad der Aktion ohne
 * deren Anfrage und Marke, dann "?" und die Felder. Ohne Felder bleibt
 * es bei der Adresse. */
bool form_get_url(char *out, size_t size, const char *action,
                  const char *query);

#endif /* FORMUTIL_H */
