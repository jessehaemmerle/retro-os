/* search.c - das Sammeln der Treffer.
 *
 * Woher die Treffer kommen, steht hier; wie sie bewertet und
 * geordnet werden, in searchutil.c.
 */

#include "search.h"
#include "apps.h"
#include "icons.h"
#include "kstring.h"
#include "lang.h"
#include "vfs.h"

/* Wie viele Ordner die Dateisuche hoechstens oeffnet. Ohne Grenze
 * liefe sie ueber eine gemountete Festplatte minutenlang - und
 * niemand tippt, um dann zu warten. */
#define SCAN_MAX_NODES 2000
#define SCAN_MAX_DEPTH 6

/* --- Sammeln --------------------------------------------------------- */

static void add(struct search_hit *out, size_t *n, size_t max,
                enum search_kind kind, int icon, int32_t score,
                const char *label, const char *detail, size_t index,
                void *node)
{
    if (*n >= max)
        return;

    struct search_hit *h = &out[(*n)++];

    memset(h, 0, sizeof(*h));
    strlcpy(h->label, label, sizeof(h->label));
    strlcpy(h->detail, detail ? detail : "", sizeof(h->detail));
    h->kind  = kind;
    h->icon  = icon;
    h->score = score;
    h->index = index;
    h->node  = node;
}

/* Die Einstellungen sind kein Programm je Zeile - gesucht wird
 * trotzdem danach, denn niemand merkt sich, unter welchem Fenster die
 * Zeitzone steht. */
static const char *const settings_words[] = {
    "Sprache", "Erscheinungsbild", "Tastatur", "Aufloesung",
    "Vergroesserung", "Batterieuhr", "Zeitzone", "Hintergrund",
    "Hintergrundbild", "Schrift", "Rechnername",
};

static void scan_dir(struct fs_node *dir, const char *query,
                     struct search_hit *out, size_t *n, size_t max,
                     size_t *budget, int depth)
{
    if (!dir || depth > SCAN_MAX_DEPTH || *budget == 0)
        return;

    struct fs_node *child = dir->first_child;

    for (; child; child = child->next_sibling) {
        if (*budget == 0)
            return;
        (*budget)--;

        int32_t score = search_score(child->name, query);

        if (score >= 0 && *n < max) {
            char path[FS_PATH_MAX];

            fs_path(child, path, sizeof(path));
            /* Dateien stehen hinter Programmen: Wer "Uhr" tippt, will
             * die Uhr und nicht uhr.txt. */
            add(out, n, max, SEARCH_FILE,
                child->type == FS_DIR ? ICON_FOLDER : ICON_FILE_TEXT,
                score - 60, child->name, path, 0, child);
        }

        if (child->type == FS_DIR)
            scan_dir(child, query, out, n, max, budget, depth + 1);
    }
}

size_t search_collect(const char *query, struct search_hit *out, size_t max)
{
    size_t n = 0;

    if (!query || !*query || !out || max == 0)
        return 0;

    for (size_t i = 0; i < app_count; i++) {
        /* Gesucht wird in beiden Sprachen: Wer die Oberflaeche auf
         * Englisch stehen hat, tippt "Settings" - der Name im
         * Quelltext bleibt aber deutsch. */
        int32_t a = search_score(app_list[i].name, query);
        int32_t b = search_score(tr(app_list[i].name), query);
        int32_t score = MAX(a, b);

        if (score >= 0)
            add(out, &n, max, SEARCH_APP, app_list[i].icon, score + 40,
                app_list[i].name, "Programm", i, NULL);
    }

    for (size_t i = 0; i < ARRAY_LEN(settings_words); i++) {
        int32_t a = search_score(settings_words[i], query);
        int32_t b = search_score(tr(settings_words[i]), query);
        int32_t score = MAX(a, b);

        if (score >= 0)
            add(out, &n, max, SEARCH_SETTING, ICON_SETTINGS, score,
                settings_words[i], "Einstellungen", i, NULL);
    }

    size_t budget = SCAN_MAX_NODES;

    scan_dir(fs_root(), query, out, &n, max, &budget, 0);

    search_sort(out, n);
    return n;
}

void search_run(const struct search_hit *hit)
{
    if (!hit)
        return;

    switch (hit->kind) {
    case SEARCH_APP:
        if (hit->index < app_count && app_list[hit->index].launch)
            app_list[hit->index].launch();
        break;
    case SEARCH_SETTING:
        app_settings();
        break;
    case SEARCH_FILE:
        app_open_node(hit->node);
        break;
    }
}
