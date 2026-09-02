/* log.h - das Systemprotokoll.
 *
 * Bisher gingen alle Meldungen des Kerns auf die serielle Schnittstelle
 * und waren damit nur zu sehen, wenn jemand ein Kabel angeschlossen
 * hatte oder RetroOS in einer virtuellen Maschine lief. Auf einem
 * richtigen Rechner war der ganze Startvorgang danach verloren - und
 * gerade dort will man wissen, warum die Platte nicht gefunden wurde.
 *
 * Deshalb liegen die Meldungen jetzt zusaetzlich in einem Ring im
 * Arbeitsspeicher: die letzten paar hundert, mit Zeitpunkt, Dringlichkeit
 * und der Stelle, von der sie kommen. Ein Ring und keine wachsende
 * Liste, weil ein Protokoll, das den Speicher auffrisst, schlimmer ist
 * als eines, das die aeltesten Zeilen vergisst.
 *
 * Es gibt zwei Wege hinein. Alles, was kprintf schreibt, landet
 * zeilenweise als Meldung des Kerns darin - so ist der Startvorgang
 * vollstaendig da, ohne dass eine einzige bestehende Zeile geaendert
 * werden musste. Wer mehr sagen will, ruft log_write() und gibt die
 * Dringlichkeit und die Herkunft gleich mit.
 */
#ifndef LOG_H
#define LOG_H

#include "retro.h"

#define LOG_ENTRIES     512
#define LOG_TEXT_MAX    119
#define LOG_SOURCE_MAX  11

#define LOG_PATH_DEFAULT "/Festplatte/protokoll.txt"

enum log_level {
    LOG_DEBUG,      /* nur beim Suchen interessant        */
    LOG_INFO,       /* der gewoehnliche Lauf der Dinge    */
    LOG_WARN,       /* ging gut aus, sollte aber auffallen */
    LOG_ERROR,      /* etwas hat nicht geklappt           */
    LOG_LEVELS
};

struct log_entry {
    uint32_t seq;                        /* laufende Nummer seit dem Start */
    uint64_t ms;                         /* Millisekunden seit dem Start   */
    uint8_t  level;
    char     source[LOG_SOURCE_MAX + 1];
    char     text[LOG_TEXT_MAX + 1];
};

void log_write(enum log_level level, const char *source, const char *fmt, ...);

#define log_debug(src, ...) log_write(LOG_DEBUG, (src), __VA_ARGS__)
#define log_info(src, ...)  log_write(LOG_INFO,  (src), __VA_ARGS__)
#define log_warn(src, ...)  log_write(LOG_WARN,  (src), __VA_ARGS__)
#define log_error(src, ...) log_write(LOG_ERROR, (src), __VA_ARGS__)

/* Wie viele Meldungen liegen gerade im Ring? Index 0 ist die aelteste
 * noch vorhandene. */
size_t log_count(void);
/* Kopiert eine Meldung heraus. false, wenn es die Nummer nicht gibt.
 * Kopiert wird unter der Sperre - der Ring darf sich dabei drehen. */
bool   log_get(size_t index, struct log_entry *out);
/* Wie viele Meldungen der Ring seit dem Start hat fallen lassen. */
uint32_t log_lost(void);
/* Wie viele der noch vorhandenen haben diese Dringlichkeit? */
size_t log_count_level(enum log_level level);

void log_clear(void);
/* Schreibt das Protokoll als Text. Liefert false, wenn die Datei nicht
 * angelegt werden konnte. */
bool log_save(const char *path);

const char *log_level_name(enum log_level level);
const char *log_level_short(enum log_level level);

/* Nimmt die Zeichen entgegen, die kprintf auf die serielle Schnittstelle
 * schreibt, und macht daraus zeilenweise Meldungen. Wird nur von
 * printf.c gerufen. */
void log_kernel_char(char c);

#endif /* LOG_H */
