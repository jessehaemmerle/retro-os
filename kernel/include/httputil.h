/* httputil.h - das Rechnen hinter dem HTTP-Client, ohne Netz.
 *
 * Adressen zerlegen und zusammensetzen, Kopfzeilen finden, Stuecke
 * ("chunked") zusammenfuegen. Das sind die Stellen, an denen eine Seite
 * "nicht laedt", obwohl der Server alles richtig geschickt hat: eine
 * Weiterleitung auf eine relative Adresse, die niemand zusammensetzt,
 * eine Stueckelung, die an einer Hexzahl scheitert, eine Kopfzeile, die
 * in Gross- statt Kleinschreibung kommt.
 *
 * Getrennt vom Client, damit sich das auf dem Entwicklungsrechner mit
 * den Antworten echter Server pruefen laesst.
 */
#ifndef HTTPUTIL_H
#define HTTPUTIL_H

#include "retro.h"

/* Zerlegt eine Adresse. Ohne Schema gilt http. secure meldet https; ohne
 * Portangabe gilt 80 bzw. 443. false bei leerem Rechnernamen oder
 * unsinnigem Port. Pfad samt Anfrage bleibt zusammen ("/a?b=c"). */
bool url_split(const char *url, char *host, size_t host_size,
               uint16_t *port, char *path, size_t path_size, bool *secure);

/* Setzt eine Adresse relativ zu einer Basis zusammen (RFC 3986,
 * vereinfacht): absolute Adressen, "//rechner/pfad", "/pfad", "?anfrage",
 * "#marke" und Pfade mit "." und "..". Die Marke (#...) wird nicht
 * uebernommen - sie gehoert nicht in eine Anfrage. Gilt nur fuer http
 * und https; alles andere wird unveraendert zurueckgegeben. */
void url_resolve(const char *base, const char *ref, char *out, size_t size);

/* Sucht in den Kopfzeilen (eine je Zeile, getrennt durch \n oder \r\n,
 * ohne die Statuszeile zu beachten) die index-te mit diesem Namen -
 * gleich in welcher Schreibweise - und kopiert ihren Wert ohne fuehrende
 * Leerzeichen. false, wenn es keine gibt. */
bool http_header_get(const char *headers, const char *name, unsigned index,
                     char *out, size_t size);

/* Loest die Stueckelung auf. Gibt die neue Laenge zurueck. Eine
 * kaputte Stueckelung endet am Fehler; was bis dahin stand, bleibt. */
uint32_t http_decode_chunked(char *body, uint32_t length);

/* Der Statuscode der ersten Zeile ("HTTP/1.1 301 ..."), sonst 0. */
int http_status_code(const char *response);

/* Wie der Rumpf kodiert ist. */
enum http_encoding { ENC_IDENTITY, ENC_GZIP, ENC_DEFLATE, ENC_UNKNOWN };
enum http_encoding http_content_encoding(const char *headers);

#endif /* HTTPUTIL_H */
