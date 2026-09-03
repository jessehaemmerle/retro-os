/* deflate.h - Packen nach DEFLATE (RFC 1951).
 *
 * Das Gegenstueck zu inflate.h. Gepackt wird mit festen Huffman-Baeumen
 * (Blocktyp 1) und einem gierigen Suchlauf ueber ein Fenster von 32 KB.
 *
 * Warum feste Baeume und nicht eigene? Eigene Baeume bringen bei Text
 * noch einmal ein Fuenftel, kosten aber die halbe Umsetzung: zwei
 * Durchlaeufe, Haeufigkeiten zaehlen, Baeume bauen, sie selbst wieder
 * packen. Die festen Tabellen stehen in der Norm, jeder Entpacker
 * kennt sie, und der Unterschied zwischen "gar nicht gepackt" und
 * "ordentlich gepackt" ist der grosse - der zwischen "ordentlich" und
 * "sehr gut" der kleine.
 */
#ifndef DEFLATE_H
#define DEFLATE_H

#include "retro.h"

/* Packt und liefert einen mit kmalloc geholten Puffer, oder NULL.
 * Die Laenge steht danach in *out_length. */
void *deflate_raw(const uint8_t *data, size_t length, size_t *out_length);

/* Dasselbe mit zlib-Huelle: zwei Bytes davor, Adler-32 dahinter -
 * das, was PNG und ZIP-Streams erwarten. */
void *deflate_zlib(const uint8_t *data, size_t length, size_t *out_length);

/* Adler-32, wie zlib sie um die Daten legt. */
uint32_t adler32(const void *data, size_t length);

#endif /* DEFLATE_H */
