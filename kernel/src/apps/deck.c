/* deck.c - Folien halten, umsortieren, speichern.
 *
 * Das Dateiformat ist absichtlich so einfach, dass es sich von Hand
 * schreiben laesst: eine Raute beginnt eine Folie, ein Ausrufezeichen
 * nennt ihre Anordnung, ein Strich eine Zeile. Alles andere wird als
 * Zeile gelesen - so geht auch aus einer beliebigen Liste eine
 * Praesentation hervor.
 */

#include "deck.h"
#include "kstring.h"
#include "lang.h"

void deck_clear(struct deck *deck)
{
    memset(deck, 0, sizeof(*deck));
    deck->count = 1;
    deck->slides[0].layout = LAYOUT_TITLE;
}

int deck_insert(struct deck *deck, int index)
{
    if (deck->count >= DECK_SLIDES_MAX)
        return index;

    index = CLAMP(index, -1, deck->count - 1);

    for (int i = deck->count; i > index + 1; i--)
        deck->slides[i] = deck->slides[i - 1];

    deck->count++;

    struct slide *slide = &deck->slides[index + 1];

    memset(slide, 0, sizeof(*slide));
    slide->layout = LAYOUT_BULLETS;
    return index + 1;
}

void deck_remove(struct deck *deck, int index)
{
    if (index < 0 || index >= deck->count || deck->count <= 1)
        return;

    for (int i = index; i + 1 < deck->count; i++)
        deck->slides[i] = deck->slides[i + 1];
    deck->count--;
}

void deck_move(struct deck *deck, int index, int direction)
{
    int target = index + direction;

    if (index < 0 || index >= deck->count)
        return;
    if (target < 0 || target >= deck->count)
        return;

    struct slide keep = deck->slides[index];

    deck->slides[index] = deck->slides[target];
    deck->slides[target] = keep;
}

int slide_insert_line(struct slide *slide, int index)
{
    if (slide->line_count >= SLIDE_LINES_MAX)
        return index;

    index = CLAMP(index, -1, slide->line_count - 1);

    for (int i = slide->line_count; i > index + 1; i--)
        strlcpy(slide->lines[i], slide->lines[i - 1], SLIDE_TEXT_MAX);

    slide->line_count++;
    slide->lines[index + 1][0] = '\0';
    return index + 1;
}

void slide_remove_line(struct slide *slide, int index)
{
    if (index < 0 || index >= slide->line_count)
        return;

    for (int i = index; i + 1 < slide->line_count; i++)
        strlcpy(slide->lines[i], slide->lines[i + 1], SLIDE_TEXT_MAX);
    slide->line_count--;
}

/* Der Name geht in die Datei und auf den Bildschirm. Er bleibt hier
 * deutsch; wer ihn anzeigt, legt tr() darum. */
const char *deck_layout_name(uint8_t layout)
{
    switch (layout) {
    case LAYOUT_TITLE: return "Titelfolie";
    case LAYOUT_QUOTE: return "Zitat";
    default:           return "Aufzaehlung";
    }
}

/* ------------------------------------------------------------------ */
/* Dateien                                                             */
/* ------------------------------------------------------------------ */

static size_t put(char *out, size_t size, size_t at, const char *text)
{
    while (*text && at + 1 < size)
        out[at++] = *text++;
    return at;
}

size_t deck_to_text(const struct deck *deck, char *out, size_t size)
{
    size_t at = 0;

    for (int i = 0; i < deck->count; i++) {
        const struct slide *slide = &deck->slides[i];

        at = put(out, size, at, "# ");
        at = put(out, size, at, slide->title);
        at = put(out, size, at, "\n! ");
        at = put(out, size, at, deck_layout_name(slide->layout));
        at = put(out, size, at, "\n");

        for (int k = 0; k < slide->line_count; k++) {
            at = put(out, size, at, "- ");
            at = put(out, size, at, slide->lines[k]);
            at = put(out, size, at, "\n");
        }
        at = put(out, size, at, "\n");
    }

    if (size)
        out[MIN(at, size - 1)] = '\0';
    return at;
}

static uint8_t layout_from(const char *name)
{
    /* Wie bei den Aufgaben: In der Datei steht Deutsch, damit sie
     * ueberall lesbar bleibt - der englische Name wird beim Lesen
     * trotzdem verstanden. */
    if (strcasecmp(name, "Titelfolie") == 0 || strcasecmp(name, "titel") == 0 ||
        strcasecmp(name, "Title slide") == 0)
        return LAYOUT_TITLE;
    if (strcasecmp(name, "Zitat") == 0 || strcasecmp(name, "Quote") == 0)
        return LAYOUT_QUOTE;
    return LAYOUT_BULLETS;
}

void deck_from_text(struct deck *deck, const char *text, size_t length)
{
    memset(deck, 0, sizeof(*deck));

    int current = -1;
    size_t i = 0;

    while (i < length) {
        char line[SLIDE_TEXT_MAX * 2];
        size_t n = 0;

        while (i < length && text[i] != '\n') {
            if (text[i] != '\r' && n + 1 < sizeof(line))
                line[n++] = text[i];
            i++;
        }
        line[n] = '\0';
        if (i < length)
            i++;

        /* Fuehrende Leerzeichen abschneiden. */
        char *p = line;

        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            continue;

        if (*p == '#') {
            p++;
            while (*p == ' ')
                p++;

            if (deck->count >= DECK_SLIDES_MAX)
                break;

            current = deck->count++;
            memset(&deck->slides[current], 0, sizeof(struct slide));
            deck->slides[current].layout = LAYOUT_BULLETS;
            strlcpy(deck->slides[current].title, p, SLIDE_TEXT_MAX);
            continue;
        }

        if (current < 0) {
            /* Vor der ersten Raute: Der Text macht die erste Folie
             * auf, damit nichts verlorengeht. */
            current = deck->count++;
            memset(&deck->slides[current], 0, sizeof(struct slide));
            deck->slides[current].layout = LAYOUT_BULLETS;
        }

        struct slide *slide = &deck->slides[current];

        if (*p == '!') {
            p++;
            while (*p == ' ')
                p++;
            slide->layout = layout_from(p);
            continue;
        }

        if (*p == '-' || *p == '*') {
            p++;
            while (*p == ' ')
                p++;
        }

        if (slide->line_count < SLIDE_LINES_MAX)
            strlcpy(slide->lines[slide->line_count++], p, SLIDE_TEXT_MAX);
    }

    if (deck->count == 0)
        deck_clear(deck);
}
