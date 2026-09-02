/* log.c - der Ring, in dem die Meldungen des Systems liegen.
 *
 * Der Ring wird von jedem Kern beschrieben und aus der Oberflaeche
 * gelesen, also gehoert eine Sperre darum. Sie wird mit abgeschalteten
 * Unterbrechungen genommen: Auch ein Treiber im Interrupt darf etwas
 * melden, und ohne das kaeme der Kern an sich selbst nicht vorbei.
 *
 * Gehalten wird sie nur fuer das Umkopieren eines Eintrags. Formatiert
 * wird davor - ksnprintf nimmt keine Sperre, also kann das gefahrlos
 * ausserhalb passieren.
 */

#include "log.h"

#include "arch.h"
#include "kstring.h"
#include "mm.h"
#include "rtc.h"
#include "spinlock.h"
#include "vfs.h"

#include <stdarg.h>

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

static struct log_entry  ring[LOG_ENTRIES];
static struct spinlock   lock = SPINLOCK_INIT("protokoll");
static uint32_t          written;      /* alle je geschriebenen Meldungen */

static void push(enum log_level level, const char *source, const char *text)
{
    uint64_t flags = spin_lock_irq(&lock);
    struct log_entry *e = &ring[written % LOG_ENTRIES];

    e->seq   = written + 1;
    e->ms    = timer_ms();
    e->level = (uint8_t)level;
    strlcpy(e->source, source ? source : "kern", sizeof(e->source));
    strlcpy(e->text, text, sizeof(e->text));
    written++;

    spin_unlock_irq(&lock, flags);
}

void log_write(enum log_level level, const char *source, const char *fmt, ...)
{
    char    text[LOG_TEXT_MAX + 1];
    va_list ap;

    va_start(ap, fmt);
    kvsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    push(level, source, text);
}

/* ------------------------------------------------------------------ */
/* Was kprintf schreibt                                                */
/* ------------------------------------------------------------------ */

/* Diese Funktion laeuft schon unter der Ausgabesperre von printf.c -
 * die Zeile darunter braucht also keine eigene. Erst das Ablegen im
 * Ring nimmt eine, und das ist eine andere.
 *
 * Die Dringlichkeit wird geraten, denn kprintf kennt keine: Wo "Fehler",
 * "kein" oder "nicht" steht, ist meist etwas schiefgegangen. Das ist
 * grob, aber es faerbt den Startvorgang brauchbar ein, ohne dass
 * hundert bestehende Zeilen angefasst werden mussten. */
static char   line[LOG_TEXT_MAX + 1];
static size_t line_len;

static bool contains(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);

    for (const char *p = haystack; *p; p++)
        if (strncasecmp(p, needle, n) == 0)
            return true;
    return false;
}

static void flush_line(void)
{
    if (!line_len) {
        return;
    }

    line[line_len] = '\0';

    /* Die Meldungen des Kerns sind nach dem Muster "Bereich : Text"
     * aufgebaut. Steht ein Doppelpunkt in den ersten Spalten, ist das
     * Wort davor die Herkunft - dann steht sie in der Oberflaeche in
     * einer eigenen Spalte statt mitten im Text. */
    char        source[LOG_SOURCE_MAX + 1] = "kern";
    const char *text = line;

    for (size_t i = 0; i < line_len && i <= LOG_SOURCE_MAX + 1; i++) {
        if (line[i] != ':')
            continue;

        size_t end = i;

        while (end > 0 && line[end - 1] == ' ')
            end--;
        if (end == 0 || end > LOG_SOURCE_MAX)
            break;

        memcpy(source, line, end);
        source[end] = '\0';

        text = line + i + 1;
        while (*text == ' ')
            text++;
        break;
    }

    enum log_level level = LOG_INFO;

    if (contains(line, "fehler") || contains(line, "gescheitert") ||
        contains(line, "kein") || contains(line, "nicht"))
        level = LOG_WARN;

    push(level, source, text);
    line_len = 0;
}

void log_kernel_char(char c)
{
    if (c == '\r')
        return;
    if (c == '\n') {
        flush_line();
        return;
    }
    if (line_len < LOG_TEXT_MAX) {
        line[line_len++] = c;
        return;
    }

    /* Eine ueberlange Zeile wird geteilt statt abgeschnitten - so geht
     * kein Zeichen verloren. */
    flush_line();
    line[line_len++] = c;
}

/* ------------------------------------------------------------------ */
/* Lesen                                                               */
/* ------------------------------------------------------------------ */

size_t log_count(void)
{
    uint32_t n = written;

    return n < LOG_ENTRIES ? n : LOG_ENTRIES;
}

uint32_t log_lost(void)
{
    return written > LOG_ENTRIES ? written - LOG_ENTRIES : 0;
}

bool log_get(size_t index, struct log_entry *out)
{
    if (!out || index >= log_count())
        return false;

    uint64_t flags = spin_lock_irq(&lock);
    uint32_t first = written > LOG_ENTRIES ? written - LOG_ENTRIES : 0;

    *out = ring[(first + index) % LOG_ENTRIES];
    spin_unlock_irq(&lock, flags);

    /* Zwischen der Pruefung und dem Kopieren kann sich der Ring gedreht
     * haben. Dann ist der Eintrag ein anderer als erwartet - aber immer
     * ein gueltiger, und das genuegt fuer eine Anzeige. */
    return true;
}

size_t log_count_level(enum log_level level)
{
    size_t n = 0;
    size_t count = log_count();
    struct log_entry e;

    for (size_t i = 0; i < count; i++)
        if (log_get(i, &e) && e.level == (uint8_t)level)
            n++;
    return n;
}

void log_clear(void)
{
    uint64_t flags = spin_lock_irq(&lock);

    written = 0;
    memset(ring, 0, sizeof(ring));
    spin_unlock_irq(&lock, flags);

    log_info("protokoll", "Protokoll geleert");
}

const char *log_level_name(enum log_level level)
{
    switch (level) {
    case LOG_DEBUG: return "Suche";
    case LOG_WARN:  return "Warnung";
    case LOG_ERROR: return "Fehler";
    default:        return "Hinweis";
    }
}

const char *log_level_short(enum log_level level)
{
    switch (level) {
    case LOG_DEBUG: return "dbg";
    case LOG_WARN:  return "WRN";
    case LOG_ERROR: return "FHL";
    default:        return "inf";
    }
}

/* ------------------------------------------------------------------ */
/* Speichern                                                           */
/* ------------------------------------------------------------------ */

bool log_save(const char *path)
{
    size_t count = log_count();
    size_t cap = count * (LOG_TEXT_MAX + 48) + 512;
    char  *text = kmalloc(cap);

    if (!text)
        return false;

    struct datetime dt;

    rtc_read(&dt);

    size_t used = 0;

    /* ksnprintf meldet, wie lang es geworden waere - dazuzaehlen liefe
     * ueber das Ende hinaus. Darum jedes Mal begrenzen. */
    #define ADD(...) do {                                        \
        if (used < cap - 1) {                                    \
            ksnprintf(text + used, cap - used, __VA_ARGS__);     \
            used += strlen(text + used);                         \
        }                                                        \
    } while (0)

    ADD("# Systemprotokoll von RetroOS\n"
        "# Gesichert am %02u.%02u.%04u um %02u:%02u:%02u\n"
        "# Die Zeit in eckigen Klammern zaehlt ab dem Einschalten.\n",
        dt.day, dt.month, dt.year, dt.hour, dt.minute, dt.second);

    if (log_lost())
        ADD("# %u aeltere Meldungen sind aus dem Ring gefallen.\n",
            (unsigned)log_lost());
    ADD("\n");

    struct log_entry e;

    for (size_t i = 0; i < count; i++) {
        if (!log_get(i, &e))
            break;
        ADD("[%5u.%03u] %s %-11s %s\n",
            (unsigned)(e.ms / 1000), (unsigned)(e.ms % 1000),
            log_level_short(e.level), e.source, e.text);
    }
    #undef ADD

    struct fs_node *file = fs_lookup(NULL, path);

    if (!file)
        file = fs_create_path(NULL, path, FS_FILE);

    bool ok = file && file->type == FS_FILE && fs_write(file, text, used);

    kfree(text);

    if (ok)
        log_info("protokoll", "in %s gesichert", path);
    else
        log_warn("protokoll", "%s liess sich nicht schreiben", path);
    return ok;
}
