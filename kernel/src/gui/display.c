/* display.c - siehe display.h. */

#include "display.h"
#include "fb.h"
#include "gfx.h"
#include "gui.h"
#include "input.h"
#include "kstring.h"
#include "log.h"
#include "mm.h"
#include "vbe.h"

#define MAX_MODES 16

static struct disp_mode modes[MAX_MODES];
static size_t   mode_count;
static size_t   mode_now;

static uint32_t scale = 1;
static bool     scale_auto = true;

int32_t display_width(void)  { return (int32_t)g_fb.width; }
int32_t display_height(void) { return (int32_t)g_fb.height; }

int32_t display_logical_width(void)  { return (int32_t)(g_fb.width / scale); }
int32_t display_logical_height(void) { return (int32_t)(g_fb.height / scale); }

uint32_t display_scale(void)      { return scale; }
bool     display_scale_is_auto(void) { return scale_auto; }
bool     display_can_switch(void) { return vbe_available(); }

size_t display_mode_count(void) { return mode_count; }
size_t display_current_mode(void) { return mode_now; }

const struct disp_mode *display_mode_at(size_t index)
{
    return index < mode_count ? &modes[index] : NULL;
}

/* ------------------------------------------------------------------ */

/* Baut die Liste der waehlbaren Modi. Aufgenommen wird, was die Karte
 * im Speicher halten kann; der laufende Modus kommt in jedem Fall
 * hinein, auch wenn er nicht zu den gaengigen gehoert - sonst stuende
 * die Liste ohne den Eintrag da, auf dem man gerade sitzt. */
static void collect_modes(void)
{
    struct disp_mode all[MAX_MODES];
    size_t n = disp_standard_modes(all, ARRAY_LEN(all));
    uint64_t vram = vbe_vram_bytes();

    mode_count = 0;
    mode_now = 0;

    for (size_t i = 0; i < n && mode_count < MAX_MODES; i++) {
        if (vram && !disp_mode_fits(all[i].w, all[i].h, vram))
            continue;
        modes[mode_count++] = all[i];
    }

    int32_t w = display_width();
    int32_t h = display_height();

    for (size_t i = 0; i < mode_count; i++) {
        if (modes[i].w == w && modes[i].h == h) {
            mode_now = i;
            return;
        }
    }

    if (mode_count < MAX_MODES) {
        modes[mode_count] = (struct disp_mode){ w, h };
        mode_now = mode_count++;
    }
}

/* Nach einer Aenderung stimmt nichts mehr, was sich die Oberflaeche
 * ueber die Bildschirmgroesse gemerkt hat. */
static void relayout(void)
{
    int32_t w = display_logical_width();
    int32_t h = display_logical_height();

    mouse_set_limits(w, h);
    gui_screen_resized(w, h);
}

bool display_init(uint32_t wish)
{
    vbe_init();
    collect_modes();

    scale_auto = (wish == 0);
    scale = scale_auto ? disp_auto_scale(display_width(), display_height())
                       : wish;

    uint32_t possible = disp_max_scale(display_width(), display_height());

    if (scale < 1)
        scale = 1;
    if (scale > possible)
        scale = possible;

    if (!gfx_init(display_logical_width(), display_logical_height(), scale))
        return false;

    log_info("bildschirm", "%dx%d, %ux vergroessert, Flaeche %dx%d",
             display_width(), display_height(), (unsigned)scale,
             display_logical_width(), display_logical_height());
    return true;
}

bool display_set_scale(uint32_t wish)
{
    bool as_auto = (wish == 0);
    uint32_t next = as_auto ? disp_auto_scale(display_width(), display_height())
                            : wish;
    uint32_t possible = disp_max_scale(display_width(), display_height());

    if (next < 1 || next > possible)
        return false;
    if (next == scale && as_auto == scale_auto)
        return true;

    uint32_t before = scale;

    scale = next;
    if (!gfx_init(display_logical_width(), display_logical_height(), scale)) {
        scale = before;
        gfx_init(display_logical_width(), display_logical_height(), scale);
        return false;
    }

    scale_auto = as_auto;
    relayout();
    return true;
}

bool display_set_mode(int32_t w, int32_t h)
{
    if (!vbe_available())
        return false;
    if (w == display_width() && h == display_height())
        return true;

    uint64_t pitch = 0;

    if (!vbe_set_mode((uint32_t)w, (uint32_t)h, &pitch))
        return false;

    /* Die Karte zeigt jetzt etwas anderes an - der Framebuffer muss
     * das wissen, bevor irgendjemand wieder hineinschreibt.
     *
     * Der lineare Speicher bleibt dabei, wo er ist: Ein Moduswechsel
     * ueber diese Schnittstelle verschiebt ihn nicht, er fuellt ihn nur
     * anders. Die Adresse, die der Bootloader geliefert hat, gilt also
     * weiter - und sie ist die einzige, die schon abgebildet ist. Die
     * physische aus dem BAR waere hier ein Zeiger ins Leere. */
    fb_set_mode((uint64_t)(uintptr_t)g_fb.pixels,
                (uint32_t)w, (uint32_t)h, (uint32_t)(pitch / 4));

    if (scale_auto)
        scale = disp_auto_scale(w, h);

    uint32_t possible = disp_max_scale(w, h);

    if (scale > possible)
        scale = possible;

    if (!gfx_init(display_logical_width(), display_logical_height(), scale))
        return false;

    collect_modes();
    relayout();

    log_info("bildschirm", "jetzt %dx%d, %ux vergroessert", w, h,
             (unsigned)scale);
    return true;
}
