/* js.h - ein kleiner JavaScript-Deuter fuer den Browser.
 *
 * Zahlen sind Festkommazahlen mit 16 Nachkommastellen. Der Kern wird
 * ohne Gleitkommaeinheit uebersetzt, darum waeren echte Doppelworte
 * nicht darstellbar. Fuer Seitenskripte reicht der Bereich von etwa
 * plus/minus 140 Billionen bei einer Genauigkeit von 1/65536 aus.
 *
 * Der Speicher wird nicht einzeln freigegeben, sondern in einem
 * Sammelbereich gehalten. Beim Verlassen der Seite faellt alles auf
 * einmal weg - das erspart eine Speicherbereinigung und kann keine
 * Zeigerleichen hinterlassen.
 */
#ifndef JS_H
#define JS_H

#include "retro.h"
#include "dom.h"

typedef int64_t js_num;

#define JS_FRACTION  16
#define JS_ONE       (1LL << JS_FRACTION)
#define JS_NAN       INT64_MIN
#define JS_POS_INF   INT64_MAX
#define JS_NEG_INF   (INT64_MIN + 1)

enum js_type {
    JS_UNDEFINED,
    JS_NULL,
    JS_BOOL,
    JS_NUMBER,
    JS_STRING,
    JS_OBJECT,
};

struct js_string {
    size_t length;
    char   data[];
};

struct js_object;

struct js_value {
    enum js_type type;
    union {
        bool               boolean;
        js_num             number;
        struct js_string  *string;
        struct js_object  *object;
    } as;
};

struct js_context;

/* Eine eingebaute Funktion. */
typedef struct js_value (*js_native)(struct js_context *ctx,
                                     struct js_value self,
                                     struct js_value *args, size_t count);

/* --- Umgebung --- */
struct js_context *js_create(void);
void               js_destroy(struct js_context *ctx);

/* Verbindet den Deuter mit einem Dokument; ohne Aufruf gibt es kein
 * document-Objekt. */
void js_bind_document(struct js_context *ctx, struct document *doc);

/* Wird gerufen, wenn ein Skript das Dokument veraendert hat. */
typedef void (*js_change_hook)(void *context);
void js_on_change(struct js_context *ctx, js_change_hook hook, void *context);

/* Wird gerufen, wenn ein Skript eine neue Adresse ansteuert. */
typedef void (*js_navigate_hook)(void *context, const char *url);
void js_on_navigate(struct js_context *ctx, js_navigate_hook hook,
                    void *context);

/* --- Ausfuehren --- */
/* Fuehrt Quelltext aus. Bei einem Fehler steht die Meldung in
 * js_error(); der Rueckgabewert ist dann false. */
bool js_run(struct js_context *ctx, const char *source, size_t length);

/* Ruft eine Ereignisbehandlung mit "this" auf dem Knoten auf. */
bool js_run_handler(struct js_context *ctx, const char *source,
                    struct node *self);

const char *js_error(const struct js_context *ctx);

/* Sammelt die Ausgaben von console.log fuer die Anzeige. */
const char *js_console(const struct js_context *ctx);
void        js_console_clear(struct js_context *ctx);

/* Loest die Ereignisbehandlungen eines Knotens aus, etwa "click". */
bool js_dispatch_event(struct js_context *ctx, struct node *node,
                       const char *type);

/* Zaehlt die noch offenen Zeitgeber und fuehrt faellige aus. */
bool js_run_timers(struct js_context *ctx, uint64_t now_ms);

#endif /* JS_H */
