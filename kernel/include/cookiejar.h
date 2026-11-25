/* cookiejar.h - Cookies, wie sie ein kleiner Browser braucht.
 *
 * Viele Seiten laden erst, wenn der Browser ein Cookie zuruecksendet,
 * das sie bei der ersten Anfrage gesetzt haben: Google schickt eine
 * Weiterleitung mit Set-Cookie, und ohne das Cookie in der naechsten
 * Anfrage kommt eine Einwilligungsseite statt der Suche. Auch
 * Anmeldungen und Warenkoerbe laufen darueber.
 *
 * Der Vorrat haelt Cookies nur im Speicher und nur bis zum Ausschalten
 * - mehr braucht es nicht. Es zaehlen Rechnername, Pfad, "Secure" und
 * die Lebensdauer; "HttpOnly" und "SameSite" betreffen Skripte und
 * fremde Seiten und sind fuer diesen Browser ohne Bedeutung.
 *
 * Die Regeln stehen hier ohne Netz und werden auf dem Entwicklungsrechner
 * mit den Kopfzeilen echter Server geprueft: Wer ein Cookie fuer die
 * falsche Domain annimmt, verraet es an fremde Seiten; wer die
 * Pfadgrenze falsch zieht, schickt "/konto" an "/kontostand".
 */
#ifndef COOKIEJAR_H
#define COOKIEJAR_H

#include "retro.h"

#define COOKIE_MAX        64
#define COOKIE_NAME_MAX   40
#define COOKIE_VALUE_MAX  200
#define COOKIE_DOMAIN_MAX 64
#define COOKIE_PATH_MAX   96

struct cookie {
    bool     used;
    char     name[COOKIE_NAME_MAX];
    char     value[COOKIE_VALUE_MAX];
    char     domain[COOKIE_DOMAIN_MAX];  /* klein, ohne fuehrenden Punkt */
    char     path[COOKIE_PATH_MAX];
    bool     secure;
    bool     host_only;                  /* ohne Domain-Angabe gesetzt */
    uint64_t expires_ms;                 /* 0 = bis zum Ausschalten    */
    uint32_t sequence;                   /* Reihenfolge des Anlegens   */
};

struct cookie_jar {
    struct cookie items[COOKIE_MAX];
    uint32_t      sequence;
};

void cookie_jar_init(struct cookie_jar *jar);
size_t cookie_jar_count(const struct cookie_jar *jar);

/* Nimmt den Wert einer Set-Cookie-Kopfzeile an. host und path sind die
 * der Anfrage, die ihn bekam. false, wenn das Cookie abgelehnt wurde
 * (fremde Domain, "Secure" ueber unverschluesselt, zu lang, ohne Namen)
 * oder es ein bestehendes loescht. */
bool cookie_jar_store(struct cookie_jar *jar, const char *host,
                      const char *path, bool secure, const char *set_cookie,
                      uint64_t now_ms);

/* Schreibt den Wert der Cookie-Kopfzeile ("a=1; b=2") fuer eine Anfrage.
 * Gibt die Laenge zurueck, 0 wenn keines passt. Es kommen nur ganze
 * Eintraege hinein. */
size_t cookie_jar_header(const struct cookie_jar *jar, const char *host,
                         const char *path, bool secure, uint64_t now_ms,
                         char *out, size_t size);

#endif /* COOKIEJAR_H */
