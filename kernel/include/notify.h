/* notify.h - Benachrichtigungen.
 *
 * Bis hierher meldete sich das System mit Rueckfragefenstern: Ein
 * Bildschirmfoto war fertig, und mitten auf dem Schirm stand ein
 * Dialog, den jemand wegklicken musste. Das ist fuer eine Nachricht
 * zu viel - eine Nachricht will gesehen, nicht beantwortet werden.
 *
 * Stattdessen: eine Einblendung rechts unten, die von selbst wieder
 * geht, und ein Verlauf, in dem alles stehen bleibt. Wer nicht
 * hinsieht, verpasst nichts; wer beschaeftigt ist, wird nicht
 * unterbrochen.
 *
 * Der Ring fasst 32 Meldungen. Die aelteste faellt heraus, wenn die
 * dreiunddreissigste kommt - ein Verlauf, der Speicher frisst, waere
 * schlechter als einer, der vergisst.
 */
#ifndef NOTIFY_H
#define NOTIFY_H

#include "icons.h"

#define NOTIFY_MAX      32
#define NOTIFY_TITLE    40
#define NOTIFY_TEXT     96
/* So lange steht die Einblendung, danach verschwindet sie von
 * selbst. */
#define NOTIFY_TOAST_MS 5000

struct notification {
    char         title[NOTIFY_TITLE];
    char         text[NOTIFY_TEXT];
    char         time[8];        /* "HH:MM" */
    enum icon_id icon;
    bool         seen;           /* im Verlauf angesehen */
};

/* Traegt eine Meldung ein und blendet sie ein. Titel und Text duerfen
 * deutsch bleiben - uebersetzt wird beim Zeichnen. */
void notify_post(enum icon_id icon, const char *title, const char *text);

/* Der Verlauf, neueste zuerst. */
size_t                     notify_count(void);
const struct notification *notify_at(size_t index);
size_t                     notify_unread(void);
void                       notify_mark_seen(void);
void                       notify_clear(void);

/* Die Einblendung: Sie zeigt die neueste Meldung, solange sie jung
 * genug ist. */
bool notify_toast_visible(void);
const struct notification *notify_toast(void);
/* Schliesst die Einblendung von Hand (Klick darauf). */
void notify_toast_dismiss(void);

#endif /* NOTIFY_H */
