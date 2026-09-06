/* notify.c - siehe notify.h.
 *
 * Ein Ring aus 32 Eintraegen mit einem Schreibzeiger. Gelesen wird von
 * hinten nach vorn: notify_at(0) ist die neueste Meldung. Damit muss
 * beim Eintragen nichts verschoben werden, und der Verlauf steht
 * trotzdem in der Reihenfolge da, in der man ihn lesen will.
 */

#include "notify.h"
#include "arch.h"
#include "kstring.h"
#include "rtc.h"

static struct notification ring[NOTIFY_MAX];
static size_t   next;            /* naechster Schreibplatz */
static size_t   stored;          /* wie viele belegt sind  */
static uint64_t toast_ms;        /* wann die Einblendung kam */
static bool     toast_open;

void notify_post(enum icon_id icon, const char *title, const char *text)
{
    struct notification *n = &ring[next];

    memset(n, 0, sizeof(*n));
    strlcpy(n->title, title ? title : "", sizeof(n->title));
    strlcpy(n->text, text ? text : "", sizeof(n->text));
    rtc_format_time(n->time, sizeof(n->time));
    n->icon = icon;
    n->seen = false;

    next = (next + 1) % NOTIFY_MAX;
    if (stored < NOTIFY_MAX)
        stored++;

    toast_ms   = timer_ms();
    toast_open = true;
}

size_t notify_count(void) { return stored; }

const struct notification *notify_at(size_t index)
{
    if (index >= stored)
        return NULL;

    /* next zeigt hinter die neueste; von dort aus rueckwaerts. */
    size_t at = (next + NOTIFY_MAX - 1 - index) % NOTIFY_MAX;

    return &ring[at];
}

size_t notify_unread(void)
{
    size_t n = 0;

    for (size_t i = 0; i < stored; i++) {
        const struct notification *item = notify_at(i);

        if (item && !item->seen)
            n++;
    }
    return n;
}

void notify_mark_seen(void)
{
    for (size_t i = 0; i < NOTIFY_MAX; i++)
        ring[i].seen = true;
}

void notify_clear(void)
{
    memset(ring, 0, sizeof(ring));
    next = 0;
    stored = 0;
    toast_open = false;
}

bool notify_toast_visible(void)
{
    if (!toast_open || stored == 0)
        return false;

    if (timer_ms() - toast_ms >= NOTIFY_TOAST_MS) {
        toast_open = false;
        return false;
    }
    return true;
}

const struct notification *notify_toast(void)
{
    return notify_toast_visible() ? notify_at(0) : NULL;
}

void notify_toast_dismiss(void) { toast_open = false; }
