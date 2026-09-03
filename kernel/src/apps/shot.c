/* shot.c - Bildschirmfotos.
 *
 * Aufgenommen wird der Backbuffer, also genau das, was die Oberflaeche
 * zusammengesetzt hat - und nicht der Framebuffer. Bei zweifacher
 * Vergroesserung ist das der Unterschied zwischen einem Bild in der
 * Groesse der Arbeitsflaeche und einem, in dem jeder Punkt vierfach
 * dasteht. Gemeint ist das erste.
 *
 * Aufgenommen wird auch nicht sofort. Wer das Foto aus dem Startmenue
 * anstoesst, haette sonst das offene Menue darauf. Stattdessen wird
 * vorgemerkt, und die Oberflaeche loest im naechsten fertigen Bild
 * aus - dann steht darauf, was der Benutzer sieht.
 */

#include "apps.h"
#include "gfx.h"
#include "image.h"
#include "kstring.h"
#include "lang.h"
#include "log.h"
#include "mm.h"
#include "rtc.h"
#include "user.h"
#include "vfs.h"

static bool pending;

void screenshot_request(void)
{
    pending = true;
    gui_invalidate();
}

bool screenshot_pending(void)
{
    return pending;
}

/* Wo die Bilder hinsollen: in den eigenen Ordner, wenn es einen gibt,
 * sonst nach /Medien. */
static struct fs_node *target_dir(void)
{
    char home[FS_PATH_MAX];

    user_home_file(tr("Bilder"), "/Medien", home, sizeof(home));

    struct fs_node *dir = fs_lookup(NULL, home);

    if (dir && dir->type == FS_DIR)
        return dir;

    dir = fs_create_path(NULL, home, FS_DIR);
    if (dir && dir->type == FS_DIR)
        return dir;

    return fs_lookup(NULL, "/Medien");
}

bool screenshot_take(char *path_out, size_t path_size,
                     char *error, size_t error_size)
{
    pending = false;

    struct canvas *screen = gfx_screen();

    if (!screen || !screen->px) {
        strlcpy(error, tr("Es gibt kein Bild zum Speichern."), error_size);
        return false;
    }

    uint8_t *png = NULL;
    size_t   size = 0;

    if (!png_encode(screen->px, screen->w, screen->h, screen->stride,
                    &png, &size)) {
        strlcpy(error, tr("Zu wenig Speicher fuer das Bild."), error_size);
        return false;
    }

    struct fs_node *dir = target_dir();

    if (!dir) {
        kfree(png);
        strlcpy(error, tr("Es gibt keinen Ordner fuer Bilder."), error_size);
        return false;
    }

    /* Der Zeitpunkt im Namen: So liegen die Aufnahmen von selbst in
     * der richtigen Reihenfolge, und zwei kurz hintereinander
     * ueberschreiben sich nicht. */
    struct datetime now;
    char name[48];

    rtc_read(&now);
    ksnprintf(name, sizeof(name), "Bild-%04u%02u%02u-%02u%02u%02u.png",
              (unsigned)now.year, (unsigned)now.month, (unsigned)now.day,
              (unsigned)now.hour, (unsigned)now.minute, (unsigned)now.second);

    struct fs_node *file = fs_create(dir, name, FS_FILE);

    if (!file) {
        kfree(png);
        strlcpy(error, tr("Die Datei liess sich nicht anlegen."), error_size);
        return false;
    }

    bool ok = fs_write(file, png, size);

    kfree(png);

    if (!ok) {
        strlcpy(error, tr("Die Datei liess sich nicht schreiben."), error_size);
        return false;
    }

    fs_path(file, path_out, path_size);
    log_info("bildschirmfoto", "%s, %u Bytes", path_out, (unsigned)size);
    return true;
}

/* Der Weg ueber das Startmenue: vormerken und die Oberflaeche machen
 * lassen. */
void app_screenshot(void)
{
    screenshot_request();
}
