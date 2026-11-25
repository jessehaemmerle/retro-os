/* inflate.h - Entpacken nach DEFLATE (RFC 1951) samt zlib- und gzip-Huelle. */
#ifndef INFLATE_H
#define INFLATE_H

#include "retro.h"

/* Alle drei Funktionen liefern einen mit kmalloc geholten Puffer oder NULL.
 * Die entpackte Laenge steht danach in *out_length. */
void *inflate_raw(const uint8_t *data, size_t length, size_t *out_length);
void *inflate_zlib(const uint8_t *data, size_t length, size_t *out_length);
void *inflate_gzip(const uint8_t *data, size_t length, size_t *out_length);

/* Erkennt zlib- und gzip-Huellen selbst. */
void *inflate_auto(const uint8_t *data, size_t length, size_t *out_length);

/* Wie inflate_auto, aber die Ausgabe darf hoechstens limit Byte lang
 * werden - sonst kommt NULL. Fuer Daten aus dem Netz: Eine kleine
 * Antwort kann sich zu Gigabyte entpacken. */
void *inflate_auto_limited(const uint8_t *data, size_t length,
                           size_t *out_length, size_t limit);

#endif /* INFLATE_H */
