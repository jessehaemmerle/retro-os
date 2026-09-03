/* wallpaper.c - das eigene Hintergrundbild laden und zeichnen. */

#include "wallpaper.h"
#include "image.h"
#include "kstring.h"
#include "log.h"
#include "mm.h"
#include "perm.h"
#include "vfs.h"

/* Das fertig skalierte Bild und der Pfad, aus dem es kam. Beides
 * gehoert zusammen: Aendert sich der Bildschirm, wird aus dem Pfad
 * neu geladen. */
static struct image scaled;
static struct image original;
static char         path_used[64];
static int32_t      scaled_for_w, scaled_for_h;

const char *wallpaper_path(void) { return path_used; }

static void forget(void)
{
    image_free(&scaled);
    image_free(&original);
    path_used[0] = '\0';
    scaled_for_w = scaled_for_h = 0;
}

/* Bringt das geladene Bild auf die Flaeche: fuellend, mittig
 * beschnitten. Der Aufrufer haelt danach ein Bild, das mindestens so
 * gross ist wie die Flaeche. */
static bool rescale(int32_t w, int32_t h)
{
    if (!original.px || w <= 0 || h <= 0)
        return false;
    if (scaled.px && scaled_for_w == w && scaled_for_h == h)
        return true;

    /* Der groessere der beiden Faktoren gewinnt, damit keine Kante
     * frei bleibt. Gerechnet wird in 64 Bit: 4096 * 4096 sprengt
     * sonst den Wertebereich. */
    int64_t by_width  = (int64_t)w * original.h;
    int64_t by_height = (int64_t)h * original.w;

    int32_t target_w, target_h;

    if (by_width > by_height) {
        target_w = w;
        target_h = (int32_t)(((int64_t)w * original.h + original.w - 1) /
                             original.w);
    } else {
        target_h = h;
        target_w = (int32_t)(((int64_t)h * original.w + original.h - 1) /
                             original.h);
    }
    target_w = MAX(target_w, w);
    target_h = MAX(target_h, h);

    struct image next;

    if (!image_scale(&original, target_w, target_h, &next))
        return false;

    image_free(&scaled);
    scaled = next;
    scaled_for_w = w;
    scaled_for_h = h;
    return true;
}

bool wallpaper_set(const char *path)
{
    if (!path || !path[0]) {
        forget();
        return true;
    }

    /* Der Hintergrund gehoert dem ganzen Rechner, das Bild kann aber
     * im Heimatverzeichnis liegen. Gelesen wird darum mit den Rechten
     * dessen, der es setzt - und das ist der Aufrufer. */
    struct fs_node *file = fs_lookup(NULL, path);

    if (!file || file->type != FS_FILE || !fs_load(file) || !file->data ||
        file->size == 0)
        return false;

    struct image loaded;

    if (!image_decode(file->data, file->size, &loaded)) {
        log_warn("hintergrund", "%s laesst sich nicht lesen", path);
        return false;
    }

    forget();
    original = loaded;
    strlcpy(path_used, path, sizeof(path_used));
    log_info("hintergrund", "%s, %d x %d", path, loaded.w, loaded.h);
    return true;
}

bool wallpaper_draw(struct canvas *c, struct rect area)
{
    if (!original.px || area.w <= 0 || area.h <= 0)
        return false;
    if (!rescale(area.w, area.h))
        return false;

    /* Was ueber die Flaeche hinaussteht, wird zur Haelfte oben und zur
     * Haelfte unten abgeschnitten - der Bildmitte traut man am
     * ehesten zu, dass dort das Wesentliche steht. */
    int32_t x = area.x - (scaled.w - area.w) / 2;
    int32_t y = area.y - (scaled.h - area.h) / 2;

    struct rect before = c->clip;

    gfx_set_clip(c, rect_intersect(before, area));
    image_draw(c, x, y, &scaled);
    gfx_set_clip(c, before);
    return true;
}
