/* fb.h - der lineare Framebuffer, den der Bootloader bereitstellt. */
#ifndef FB_H
#define FB_H

#include "retro.h"

struct framebuffer {
    uint32_t *pixels;   /* 32 Bit pro Pixel, XRGB                */
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;    /* Pixel pro Zeile (nicht Bytes!)        */
    uint32_t  bpp;
};

extern struct framebuffer g_fb;

/* Initialisiert den Framebuffer aus den Bootloader-Daten. */
bool fb_init(void);

/* Kopiert einen Backbuffer zeilenweise auf den Bildschirm. */
void fb_present(const uint32_t *back, uint32_t x, uint32_t y,
                uint32_t w, uint32_t h);

#endif /* FB_H */
