/* tasks.c - Aufgaben halten, sortieren und als Text ablegen.
 *
 * Die Liste ist ein festes Feld und keine verkettete Kette. Bei
 * hoechstens vierundsechzig Eintraegen kostet das Durchlaufen nichts,
 * und dafuer gibt es keinen Zeiger, der ins Leere zeigen koennte,
 * waehrend das Fenster gerade zeichnet.
 */

#include "tasks.h"
#include "kstring.h"
#include "lang.h"

void tasks_clear(struct tasklist *list)
{
    if (!list)
        return;
    memset(list, 0, sizeof(*list));
    list->next_id = 1;
}

struct task *tasks_add(struct tasklist *list, const char *text)
{
    if (!list || !text)
        return NULL;

    /* Fuehrende und nachlaufende Leerzeichen weg - eine Aufgabe, die
     * nur aus Leerraum besteht, ist keine. */
    while (*text == ' ' || *text == '\t')
        text++;
    if (!*text)
        return NULL;

    if (!list->next_id)
        list->next_id = 1;

    for (size_t i = 0; i < TASK_MAX; i++) {
        if (list->items[i].used)
            continue;

        struct task *t = &list->items[i];

        memset(t, 0, sizeof(*t));
        t->used = true;
        t->id   = list->next_id++;
        t->prio = TP_MID;
        strlcpy(t->text, text, sizeof(t->text));

        size_t len = strlen(t->text);

        while (len > 0 && (t->text[len - 1] == ' ' || t->text[len - 1] == '\t'))
            t->text[--len] = '\0';
        return t;
    }
    return NULL;
}

struct task *tasks_by_id(struct tasklist *list, uint32_t id)
{
    if (!list)
        return NULL;

    for (size_t i = 0; i < TASK_MAX; i++)
        if (list->items[i].used && list->items[i].id == id)
            return &list->items[i];
    return NULL;
}

bool tasks_remove(struct tasklist *list, uint32_t id)
{
    struct task *t = tasks_by_id(list, id);

    if (!t)
        return false;
    memset(t, 0, sizeof(*t));
    return true;
}

size_t tasks_purge_done(struct tasklist *list)
{
    size_t n = 0;

    if (!list)
        return 0;

    for (size_t i = 0; i < TASK_MAX; i++) {
        if (!list->items[i].used || !list->items[i].done)
            continue;
        memset(&list->items[i], 0, sizeof(list->items[i]));
        n++;
    }
    return n;
}

size_t tasks_count(const struct tasklist *list, bool only_open)
{
    size_t n = 0;

    if (!list)
        return 0;

    for (size_t i = 0; i < TASK_MAX; i++) {
        if (!list->items[i].used)
            continue;
        if (only_open && list->items[i].done)
            continue;
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Sortieren                                                           */
/* ------------------------------------------------------------------ */

/* Ein Termin als eine Zahl, damit sich Daten vergleichen lassen, ohne
 * drei Felder einzeln zu pruefen. Ohne Termin die groesste Zahl: Was
 * keinen Termin hat, draengt am wenigsten. */
static uint32_t due_key(const struct task *t)
{
    if (!t->year)
        return 0xFFFFFFFFu;
    return (uint32_t)t->year * 10000u + (uint32_t)t->month * 100u + t->day;
}

static bool before(const struct task *a, const struct task *b)
{
    if (a->done != b->done)
        return !a->done;

    uint32_t da = due_key(a), db = due_key(b);

    if (da != db)
        return da < db;
    if (a->prio != b->prio)
        return a->prio > b->prio;
    return strcasecmp(a->text, b->text) < 0;
}

size_t tasks_sorted(struct tasklist *list, struct task **out, size_t max,
                    bool hide_done)
{
    size_t n = 0;

    if (!list || !out)
        return 0;

    for (size_t i = 0; i < TASK_MAX && n < max; i++) {
        if (!list->items[i].used)
            continue;
        if (hide_done && list->items[i].done)
            continue;
        out[n++] = &list->items[i];
    }

    /* Einfaches Einfuegesortieren - bei dieser Laenge ist alles andere
     * nur mehr Code. */
    for (size_t i = 1; i < n; i++) {
        struct task *key = out[i];
        size_t k = i;

        while (k > 0 && before(key, out[k - 1])) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = key;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Termine                                                             */
/* ------------------------------------------------------------------ */

static const uint8_t days_in_month[13] = {
    0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static bool leap_year(uint16_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static bool date_valid(uint16_t year, uint8_t month, uint8_t day)
{
    if (year < 1970 || year > 2999 || month < 1 || month > 12 || day < 1)
        return false;

    uint8_t last = days_in_month[month];

    if (month == 2 && leap_year(year))
        last = 29;
    return day <= last;
}

static const char *read_number(const char *text, uint32_t *out, size_t max_digits)
{
    uint32_t value = 0;
    size_t   digits = 0;

    while (*text >= '0' && *text <= '9' && digits < max_digits) {
        value = value * 10 + (uint32_t)(*text++ - '0');
        digits++;
    }
    if (!digits)
        return NULL;
    *out = value;
    return text;
}

bool tasks_parse_date(const char *text, uint16_t *year, uint8_t *month,
                      uint8_t *day)
{
    if (!text || !year || !month || !day)
        return false;

    while (*text == ' ')
        text++;

    /* Ein Strich oder gar nichts loescht den Termin. */
    if (!*text || (text[0] == '-' && !text[1])) {
        *year = 0;
        *month = 0;
        *day = 0;
        return true;
    }

    uint32_t a = 0, b = 0, c = 0;
    const char *p = read_number(text, &a, 4);

    if (!p)
        return false;

    char sep = *p;

    if (sep != '.' && sep != '-' && sep != '/')
        return false;

    p = read_number(p + 1, &b, 2);
    if (!p || *p != sep)
        return false;

    p = read_number(p + 1, &c, 4);
    if (!p)
        return false;

    while (*p == ' ')
        p++;
    if (*p)
        return false;

    uint16_t y;
    uint8_t  m, d;

    /* "2026-09-15" faengt mit dem Jahr an, "15.09.2026" hoert damit auf.
     * Woran man das erkennt: Ein Jahr hat vier Stellen. */
    if (a > 31) {
        y = (uint16_t)a; m = (uint8_t)b; d = (uint8_t)c;
    } else {
        d = (uint8_t)a; m = (uint8_t)b; y = (uint16_t)c;
    }

    if (!date_valid(y, m, d))
        return false;

    *year = y;
    *month = m;
    *day = d;
    return true;
}

void tasks_format_date(const struct task *t, char *out, size_t size)
{
    if (!t || !t->year) {
        strlcpy(out, "-", size);
        return;
    }
    ksnprintf(out, size, "%02u.%02u.%04u", t->day, t->month, t->year);
}

bool tasks_overdue(const struct task *t, uint16_t year, uint8_t month,
                   uint8_t day)
{
    if (!t || !t->year || t->done)
        return false;

    uint32_t due = (uint32_t)t->year * 10000u + (uint32_t)t->month * 100u +
                   t->day;
    uint32_t now = (uint32_t)year * 10000u + (uint32_t)month * 100u + day;

    return due < now;
}

/* ------------------------------------------------------------------ */
/* Wichtigkeit                                                         */
/* ------------------------------------------------------------------ */

const char *task_prio_name(uint8_t prio)
{
    switch (prio) {
    case TP_HIGH: return "hoch";
    case TP_LOW:  return "niedrig";
    default:      return "mittel";
    }
}

bool task_prio_parse(const char *text, uint8_t *out)
{
    if (!text || !out)
        return false;

    /* Deutsch steht in der Datei, Englisch tippt der Benutzer, wenn
     * die Oberflaeche englisch ist - beides muss ankommen. Die Datei
     * selbst bleibt deutsch: Eine Aufgabenliste, deren Format von der
     * eingestellten Sprache abhaengt, waere auf dem naechsten Rechner
     * nicht mehr lesbar. */
    if (strcasecmp(text, "hoch") == 0 || strcasecmp(text, "high") == 0)
        { *out = TP_HIGH; return true; }
    if (strcasecmp(text, "mittel") == 0 || strcasecmp(text, "medium") == 0)
        { *out = TP_MID;  return true; }
    if (strcasecmp(text, "niedrig") == 0 || strcasecmp(text, "low") == 0)
        { *out = TP_LOW;  return true; }
    return false;
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

size_t tasks_to_text(const struct tasklist *list, char *out, size_t size)
{
    size_t used = 0;

    if (!out || !size)
        return 0;

    out[0] = '\0';
    if (!list)
        return 0;

    #define ADD(...) do {                                    \
        if (used < size - 1) {                               \
            ksnprintf(out + used, size - used, __VA_ARGS__); \
            used += strlen(out + used);                      \
        }                                                    \
    } while (0)

    ADD("# Aufgaben von RetroOS\n"
        "# [x] erledigt, [ ] offen; dann Wichtigkeit, Termin und Text.\n"
        "# Diese Datei darf von Hand geaendert werden.\n\n");

    /* Geschrieben wird in der Reihenfolge der Liste, nicht sortiert -
     * so bleibt die Datei von einem Speichern zum naechsten stabil und
     * ein Vergleich zeigt nur, was sich wirklich geaendert hat. */
    for (size_t i = 0; i < TASK_MAX; i++) {
        const struct task *t = &list->items[i];
        char date[16];

        if (!t->used)
            continue;

        tasks_format_date(t, date, sizeof(date));
        ADD("[%c] %-8s %-11s %s\n", t->done ? 'x' : ' ',
            task_prio_name(t->prio), date, t->text);
    }
    #undef ADD

    return used;
}

/* Ein Wort bis zum naechsten Leerzeichen, in einen kleinen Puffer. */
static const char *word(const char *text, char *out, size_t size)
{
    size_t n = 0;

    while (*text == ' ' || *text == '\t')
        text++;
    while (*text && *text != ' ' && *text != '\t' && n + 1 < size)
        out[n++] = *text++;
    out[n] = '\0';
    while (*text && *text != ' ' && *text != '\t')
        text++;
    return text;
}

static void read_line(struct tasklist *list, const char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    if (!*line || *line == '#')
        return;

    bool done = false;

    /* Ohne Kasten davor gilt die ganze Zeile als offene Aufgabe - so
     * laesst sich eine Liste auch einfach hintippen. */
    if (line[0] == '[' && line[1] && line[2] == ']') {
        done = line[1] == 'x' || line[1] == 'X';
        line += 3;
    } else {
        struct task *t = tasks_add(list, line);

        if (t)
            t->prio = TP_MID;
        return;
    }

    char buffer[24];
    uint8_t prio = TP_MID;
    const char *rest = word(line, buffer, sizeof(buffer));

    if (!task_prio_parse(buffer, &prio)) {
        /* Kein Wort fuer die Wichtigkeit - dann war es schon der Text. */
        rest = line;
        prio = TP_MID;
    }

    uint16_t year = 0;
    uint8_t  month = 0, day = 0;
    const char *after = word(rest, buffer, sizeof(buffer));

    if (buffer[0] && tasks_parse_date(buffer, &year, &month, &day))
        rest = after;

    while (*rest == ' ' || *rest == '\t')
        rest++;

    struct task *t = tasks_add(list, rest);

    if (!t)
        return;
    t->done  = done;
    t->prio  = prio;
    t->year  = year;
    t->month = month;
    t->day   = day;
}

void tasks_from_text(struct tasklist *list, const char *text)
{
    if (!list)
        return;

    tasks_clear(list);
    if (!text)
        return;

    char line[TASK_TEXT_MAX + 64];

    while (*text) {
        size_t n = 0;

        while (*text && *text != '\n' && n + 1 < sizeof(line))
            line[n++] = *text++;
        line[n] = '\0';

        /* Reste einer ueberlangen Zeile ueberspringen. */
        while (*text && *text != '\n')
            text++;
        if (*text == '\n')
            text++;

        size_t len = strlen(line);

        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' '))
            line[--len] = '\0';

        read_line(list, line);
    }
}
