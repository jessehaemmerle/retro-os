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

/* Uebernimmt einen Modus, den jemand anders gesetzt hat. */
void fb_set_mode(uint64_t addr, uint32_t width, uint32_t height,
                 uint32_t pitch_pixels);

/* Kopiert einen Backbuffer auf den Bildschirm. Der Ausschnitt x/y/w/h
 * ist in Punkten des Backbuffers gemeint; scale sagt, wie gross ein
 * solcher Punkt auf dem Schirm wird. */
void fb_present(const uint32_t *back, uint32_t back_stride,
                uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                uint32_t scale);

#endif /* FB_H */
