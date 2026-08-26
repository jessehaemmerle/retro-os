/* image.h - Bilder aus PNG, JPEG, GIF und BMP. */
#ifndef IMAGE_H
#define IMAGE_H

#include "retro.h"
#include "gfx.h"

/* Pixel liegen als 0xAARRGGBB vor; 0xFF im Alphakanal heisst deckend. */
struct image {
    int32_t   w, h;
    uint32_t *px;
};

bool image_decode(const uint8_t *data, size_t length, struct image *out);
bool image_decode_png(const uint8_t *data, size_t length, struct image *out);
bool image_decode_jpeg(const uint8_t *data, size_t length, struct image *out);
bool image_decode_gif(const uint8_t *data, size_t length, struct image *out);
bool image_decode_bmp(const uint8_t *data, size_t length, struct image *out);

void image_free(struct image *img);

/* Erzeugt eine skalierte Kopie; bei Misserfolg bleibt out leer. */
bool image_scale(const struct image *src, int32_t w, int32_t h,
                 struct image *out);

/* Zeichnet mit Alphaueberblendung. */
void image_draw(struct canvas *c, int32_t x, int32_t y, const struct image *img);

/* Nennt das erkannte Format, etwa "PNG" - NULL wenn unbekannt. */
const char *image_format(const uint8_t *data, size_t length);

#endif /* IMAGE_H */
