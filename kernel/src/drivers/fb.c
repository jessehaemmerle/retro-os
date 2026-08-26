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

void fb_present(const uint32_t *back, uint32_t x, uint32_t y,
                uint32_t w, uint32_t h)
{
    if (x >= g_fb.width || y >= g_fb.height)
        return;

    if (x + w > g_fb.width)
        w = g_fb.width - x;
    if (y + h > g_fb.height)
        h = g_fb.height - y;

    for (uint32_t row = 0; row < h; row++) {
        memcpy(&g_fb.pixels[(y + row) * g_fb.pitch + x],
               &back[(y + row) * g_fb.width + x],
               (size_t)w * 4);
    }
}
