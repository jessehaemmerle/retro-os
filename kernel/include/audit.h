/* audit.h - die Pruefspur.
 *
 * Das Protokoll (log.h) sagt, was das System getan hat. Die Pruefspur
 * sagt, wer es veranlasst hat und ob er durfte. Das sind zwei
 * verschiedene Fragen, und deshalb sind es zwei verschiedene Listen:
 * Ein Protokoll darf man leeren, wenn es unuebersichtlich wird; eine
 * Pruefspur darf das gerade nicht - sonst waere sie wertlos.
 *
 * Jeder Eintrag beantwortet vier Fragen: wer (Benutzernummer), was
 * (Art), woran (Gegenstand) und mit welchem Ausgang. Mehr Felder
 * braucht es nicht, und weniger waeren zu wenig.
 *
 * Aufgeschrieben wird, was fuer die Sicherheit zaehlt - Anmeldungen,
 * abgewiesene Zugriffe, gebrauchte Verwalterrechte, Aenderungen an
 * Konten und am Paketfilter. Nicht aufgeschrieben wird der Alltag:
 * Eine Pruefspur, in der jeder Dateizugriff steht, liest niemand mehr.
 */
#ifndef AUDIT_H
#define AUDIT_H

#include "retro.h"

#define AUDIT_ENTRIES   256
#define AUDIT_OBJECT_MAX 63
#define AUDIT_PATH      "/Festplatte/pruefspur.log"

enum audit_kind {
    AUDIT_LOGIN,        /* Anmeldung                       */
    AUDIT_LOGOUT,       /* Abmeldung                       */
    AUDIT_DENIED,       /* Zugriff abgewiesen              */
    AUDIT_PRIVILEGE,    /* Verwalterrecht gebraucht        */
    AUDIT_ACCOUNT,      /* Konto angelegt, geaendert, weg  */
    AUDIT_NETWORK,      /* Paketfilter geaendert           */
    AUDIT_PROCESS,      /* Programm gestartet oder beendet */
    AUDIT_KIND_COUNT
};

struct audit_entry {
    uint32_t seq;
    uint64_t ms;
    uint8_t  kind;
    bool     ok;            /* hat es geklappt?            */
    uint32_t uid;
    char     object[AUDIT_OBJECT_MAX + 1];
};

void audit(enum audit_kind kind, bool ok, const char *fmt, ...);

size_t   audit_count(void);
bool     audit_get(size_t index, struct audit_entry *out);
uint32_t audit_lost(void);
size_t   audit_count_failed(void);

const char *audit_kind_name(enum audit_kind kind);

/* Schreibt die Spur fort - angehaengt, nicht ersetzt. Nur wer
 * CAP_LOG hat, darf sie lesen; geschrieben wird vom System. */
bool audit_save(void);
bool audit_readable(void);

#endif /* AUDIT_H */
