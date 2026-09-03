/* fb.c - Zugriff auf den vom Bootloader eingerichteten Framebuffer.
 *
 * RetroOS zeichnet nie direkt hierhin, sondern immer in einen Backbuffer
 * (siehe gfx.c). Erst fb_present() schiebt fertige Bildbereiche auf den
 * Schirm - so entsteht kein Flackern.
 */

#include "fb.h"
#include "kstring.h"
#include "boot.h"

struct framebuffer g_fb;

bool fb_init(void)
{
    const struct boot_info *bi = boot_info();

    if (!bi->fb_addr || bi->fb_bpp != 32)
        return false;

    g_fb.pixels = (uint32_t *)bi->fb_addr;
    g_fb.width  = (uint32_t)bi->fb_width;
    g_fb.height = (uint32_t)bi->fb_height;
    g_fb.pitch  = (uint32_t)(bi->fb_pitch / 4);
    g_fb.bpp    = bi->fb_bpp;

    return true;
}

void fb_set_mode(uint64_t addr, uint32_t width, uint32_t height,
                 uint32_t pitch_pixels)
{
    g_fb.pixels = (uint32_t *)addr;
    g_fb.width  = width;
    g_fb.height = height;
    g_fb.pitch  = pitch_pixels ? pitch_pixels : width;
    g_fb.bpp    = 32;
}

void fb_present(const uint32_t *back, uint32_t back_stride,
                uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                uint32_t scale)
{
    if (scale < 1)
        scale = 1;

    uint32_t px = x * scale;
    uint32_t py = y * scale;

    if (px >= g_fb.width || py >= g_fb.height)
        return;

    /* Der Ausschnitt wird am Rand des Schirms abgeschnitten - und zwar
     * in ganzen Backbuffer-Punkten, damit kein halbes Quadrat
     * uebrigbleibt. */
    if (px + w * scale > g_fb.width)
        w = (g_fb.width - px) / scale;
    if (py + h * scale > g_fb.height)
        h = (g_fb.height - py) / scale;

    if (scale == 1) {
        for (uint32_t row = 0; row < h; row++)
            memcpy(&g_fb.pixels[(py + row) * g_fb.pitch + px],
                   &back[(y + row) * back_stride + x],
                   (size_t)w * 4);
        return;
    }

    /* Vergroessert: Die erste Zeile wird Punkt fuer Punkt aufgeblasen,
     * die restlichen scale-1 sind dann nur noch eine Kopie davon. Das
     * ist der ganze Trick - memcpy ist um ein Vielfaches schneller als
     * eine Schleife ueber einzelne Punkte. */
    for (uint32_t row = 0; row < h; row++) {
        const uint32_t *src = &back[(y + row) * back_stride + x];
        uint32_t *first = &g_fb.pixels[(py + row * scale) * g_fb.pitch + px];

        for (uint32_t i = 0; i < w; i++) {
            uint32_t value = src[i];

            for (uint32_t k = 0; k < scale; k++)
                first[i * scale + k] = value;
        }

        for (uint32_t k = 1; k < scale; k++)
            memcpy(&g_fb.pixels[(py + row * scale + k) * g_fb.pitch + px],
                   first, (size_t)w * scale * 4);
    }
}
