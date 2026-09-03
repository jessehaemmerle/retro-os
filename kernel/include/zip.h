/* zip.h - Archive lesen und schreiben.
 *
 * ZIP ist das Format, mit dem man am ehesten irgendwo ankommt: Jedes
 * Betriebssystem und jedes Telefon oeffnet es, und es braucht keinen
 * eigenen Katalog neben der Datei.
 *
 * Sein Aufbau ist rueckwaerts gedacht, und das aus gutem Grund: Am
 * Ende steht ein Verzeichnis aller Eintraege, davor die Daten. Wer ein
 * Archiv liest, sucht also zuerst das Ende - so laesst sich etwas
 * anhaengen, ohne alles umzuschreiben, und so bleibt ein Archiv
 * lesbar, dem vorne etwas vorangestellt wurde.
 *
 * Umgesetzt sind die beiden Verfahren, die praktisch alles abdecken:
 * ungepackt (0) und DEFLATE (8). Andere gibt es zwar, aber sie kommen
 * in freier Wildbahn kaum vor.
 */
#ifndef ZIP_H
#define ZIP_H

#include "retro.h"

#define ZIP_NAME_MAX 127

struct zip_entry {
    char     name[ZIP_NAME_MAX + 1];
    uint64_t size;          /* entpackt   */
    uint64_t packed;        /* im Archiv  */
    uint32_t crc;
    uint16_t method;        /* 0 = ungepackt, 8 = DEFLATE */
    uint16_t date, time;    /* im Format von MS-DOS       */
    uint64_t offset;        /* Anfang des lokalen Kopfes  */
    bool     is_dir;
};

/* --- Lesen --- */

/* Zaehlt die Eintraege. Liefert false, wenn es kein Archiv ist. */
bool zip_read(const uint8_t *data, size_t length, size_t *count);

/* Holt einen Eintrag. index zaehlt ab 0. */
bool zip_entry(const uint8_t *data, size_t length, size_t index,
               struct zip_entry *out);

/* Packt einen Eintrag aus. Der Puffer kommt von kmalloc und gehoert
 * danach dem Aufrufer. Die Pruefsumme wird dabei nachgerechnet; passt
 * sie nicht, gibt es NULL. */
void *zip_extract(const uint8_t *data, size_t length,
                  const struct zip_entry *entry, size_t *out_length);

/* --- Schreiben --- */

struct zip_writer;

struct zip_writer *zip_begin(void);
/* Haengt eine Datei an. Ein Name, der auf "/" endet, wird ein Ordner. */
bool zip_add(struct zip_writer *w, const char *name,
             const void *data, size_t length, uint16_t date, uint16_t time);
/* Schliesst ab und liefert den fertigen Puffer; der Schreiber ist
 * danach verbraucht. */
void *zip_finish(struct zip_writer *w, size_t *out_length);
/* Wegwerfen, ohne abzuschliessen. */
void zip_abort(struct zip_writer *w);

/* Datum und Uhrzeit im Format von MS-DOS - so will ZIP sie. */
uint16_t zip_dos_date(uint16_t year, uint8_t month, uint8_t day);
uint16_t zip_dos_time(uint8_t hour, uint8_t minute, uint8_t second);

#endif /* ZIP_H */
