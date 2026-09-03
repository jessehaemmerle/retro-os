/* lang.c - Umschalten und Nachschlagen.
 *
 * Die Tabelle selbst steht in lang_data.c und wird aus
 * data/sprache-en.txt erzeugt - der Uebersetzer soll eine Textdatei
 * bearbeiten und keinen C-Quelltext.
 */

#include "lang.h"
#include "kstring.h"

extern const struct lang_entry {
    const char *de;
    const char *en;
} lang_table[];
extern const size_t lang_table_count;

static enum language current = LANG_DE;

enum language lang_current(void) { return current; }

void lang_select(enum language lang)
{
    if (lang < LANG_COUNT)
        current = lang;
}

const char *lang_code(enum language lang)
{
    return lang == LANG_EN ? "en" : "de";
}

const char *lang_name(enum language lang)
{
    /* Jede Sprache nennt sich selbst so, wie sie heisst - wer Englisch
     * sucht, sucht "English" und nicht "Englisch". */
    return lang == LANG_EN ? "English" : "Deutsch";
}

const char *lang_default_keymap(enum language lang)
{
    return lang == LANG_EN ? "us" : "de";
}

bool lang_select_by_code(const char *code)
{
    if (!code)
        return false;
    for (enum language l = 0; l < LANG_COUNT; l++) {
        if (strcasecmp(lang_code(l), code) == 0) {
            current = l;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */

const char *lang_lookup(const char *german)
{
    if (!german || !german[0])
        return NULL;

    size_t low = 0;
    size_t high = lang_table_count;

    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int cmp = strcmp(german, lang_table[mid].de);

        if (cmp == 0)
            return lang_table[mid].en;
        if (cmp < 0)
            high = mid;
        else
            low = mid + 1;
    }
    return NULL;
}

const char *tr(const char *german)
{
    if (current == LANG_DE || !german)
        return german;

    const char *found = lang_lookup(german);

    return found ? found : german;
}

size_t lang_entry_count(void) { return lang_table_count; }

const char *lang_entry_de(size_t index)
{
    return index < lang_table_count ? lang_table[index].de : NULL;
}

const char *lang_entry_en(size_t index)
{
    return index < lang_table_count ? lang_table[index].en : NULL;
}
