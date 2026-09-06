/* searchutil.c - das Bewerten und Sortieren der Treffer.
 *
 * Getrennt von search.c, weil hier steht, was die Suche brauchbar
 * macht: dass "Uhr" die Uhr findet und nicht den Unterordner, und
 * dass ein Treffer am Wortanfang vor einem in der Mitte steht. Ohne
 * Programme, ohne Dateisystem, ohne Fenster - und darum auf dem
 * Entwicklungsrechner pruefbar.
 */

#include "search.h"
#include "kstring.h"

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Ein Zeichen, hinter dem ein neues Wort anfaengt. */
static bool boundary(char c)
{
    return c == ' ' || c == '-' || c == '_' || c == '.' || c == '/';
}

int32_t search_score(const char *text, const char *query)
{
    if (!text || !query || !*query || !*text)
        return -1;

    size_t tlen = strlen(text);
    size_t qlen = strlen(query);

    if (qlen > tlen)
        return -1;

    /* Die erste Fundstelle zaehlt: Wer "Ein" tippt, meint eher den
     * Anfang von "Einstellungen" als das "ein" in "Zeichenkette". */
    for (size_t at = 0; at + qlen <= tlen; at++) {
        size_t i = 0;

        while (i < qlen && lower(text[at + i]) == lower(query[i]))
            i++;
        if (i < qlen)
            continue;

        int32_t score = 100;

        if (at == 0)
            score += 400;
        else if (boundary(text[at - 1]))
            score += 200;

        /* Genau der Name ist besser als der Name mit Anhang. */
        if (at == 0 && qlen == tlen)
            score += 500;

        /* Kurzes vor langem: In "Uhr" steckt weniger daneben als in
         * "Unterordner". */
        score += (int32_t)(60 - MIN(tlen, (size_t)60));
        return score;
    }
    return -1;
}

void search_sort(struct search_hit *hits, size_t count)
{
    /* Einfuegesortieren: Die Liste ist kurz, und es bleibt stabil -
     * bei gleicher Punktzahl behaelt der frueher gefundene den
     * Vortritt. */
    for (size_t i = 1; i < count; i++) {
        struct search_hit key = hits[i];
        size_t k = i;

        while (k > 0 && hits[k - 1].score < key.score) {
            hits[k] = hits[k - 1];
            k--;
        }
        hits[k] = key;
    }
}

