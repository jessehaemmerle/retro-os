/* imgview.c - Bilder ansehen.
 *
 * Das Entpacken kann das System laengst - PNG, JPEG, GIF und BMP
 * liegen fuer den Browser bereit. Was fehlte, war ein Fenster, in dem
 * man ein Bild einfach anschaut.
 *
 * Zwei Entscheidungen praegen das Ganze. Erstens wird die skalierte
 * Fassung gemerkt und nicht bei jedem Bildaufbau neu gerechnet: Ein
 * Foto auf Fenstergroesse zu bringen kostet Millionen von
 * Rechenschritten, und der Hintergrund wird bei jeder Mausbewegung neu
 * gezeichnet. Neu skaliert wird erst, wenn sich Fenster oder
 * Vergroesserung geaendert haben.
 *
 * Zweitens kennt das Fenster den Ordner, aus dem das Bild kam, und
 * blaettert mit den Pfeiltasten darin weiter. Wer sich Bilder ansieht,
 * sieht sich selten nur eines an.
 */

#include "apps.h"
#include "font.h"
#include "image.h"
#include "kstring.h"
#include "lang.h"
#include "mm.h"
#include "theme.h"
#include "vfs.h"
#include "widgets.h"

#define STATUS_H  26
#define MAX_SIBLINGS 64

struct view_state {
    struct image original;
    struct image scaled;

    /* Woraus die gemerkte Fassung entstanden ist. Stimmt eines nicht
     * mehr, wird neu gerechnet. */
    int32_t scaled_w, scaled_h;

    char    path[FS_PATH_MAX];
    char    name[FS_NAME_MAX + 1];
    size_t  bytes;

    /* 0 = einpassen, sonst Prozent. */
    uint32_t zoom;
    char     status[96];
};

static bool is_image_name(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;

    if (!dot)
        return false;
    return strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
           strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".gif") == 0 ||
           strcasecmp(dot, ".bmp") == 0;
}

/* ------------------------------------------------------------------ */

static void forget_scaled(struct view_state *st)
{
    image_free(&st->scaled);
    st->scaled_w = st->scaled_h = 0;
}

static bool load(struct view_state *st, struct fs_node *file)
{
    if (!file || file->type != FS_FILE || !fs_load(file) || !file->data ||
        file->size == 0) {
        strlcpy(st->status, tr("Die Datei laesst sich nicht lesen."),
                sizeof(st->status));
        return false;
    }

    struct image loaded;

    if (!image_decode(file->data, file->size, &loaded)) {
        ksnprintf(st->status, sizeof(st->status), tr("%s ist kein Bild."),
                  file->name);
        return false;
    }

    image_free(&st->original);
    forget_scaled(st);

    st->original = loaded;
    st->bytes = file->size;
    st->zoom = 0;
    fs_path(file, st->path, sizeof(st->path));
    strlcpy(st->name, file->name, sizeof(st->name));
    st->status[0] = '\0';
    return true;
}

/* Das naechste oder vorige Bild im selben Ordner. */
static void step(struct window *win, int delta)
{
    struct view_state *st = win->user;
    struct fs_node *file = fs_lookup(NULL, st->path);
    struct fs_node *dir = file ? file->parent : NULL;

    if (!dir)
        return;

    struct fs_node *entries[MAX_SIBLINGS];
    size_t n = fs_list(dir, entries, ARRAY_LEN(entries));

    /* Nur Bilder zaehlen mit - sonst blaettert man durch Textdateien,
     * die nichts anzuzeigen haben. */
    struct fs_node *images[MAX_SIBLINGS];
    size_t count = 0;
    size_t at = 0;

    for (size_t i = 0; i < n; i++) {
        if (entries[i]->type != FS_FILE || !is_image_name(entries[i]->name))
            continue;
        if (entries[i] == file)
            at = count;
        images[count++] = entries[i];
    }

    if (count < 2)
        return;

    size_t next = (at + count + (size_t)(delta > 0 ? 1 : count - 1)) % count;

    if (load(st, images[next])) {
        gui_set_title(win, st->name);
        gui_invalidate();
    }
}

/* ------------------------------------------------------------------ */

static struct rect canvas_rect(struct window *win)
{
    return rect_make(0, 0, gui_client_width(win),
                     gui_client_height(win) - STATUS_H);
}

/* Die Groesse, in der das Bild erscheinen soll. */
static void target_size(struct view_state *st, struct rect area,
                        int32_t *out_w, int32_t *out_h)
{
    if (st->zoom > 0) {
        *out_w = (int32_t)((int64_t)st->original.w * st->zoom / 100);
        *out_h = (int32_t)((int64_t)st->original.h * st->zoom / 100);
        *out_w = MAX(*out_w, 1);
        *out_h = MAX(*out_h, 1);
        return;
    }

    /* Einpassen: Der kleinere der beiden Faktoren gewinnt, und
     * vergroessert wird nicht - ein kleines Bild aufzublasen macht es
     * nur unscharf. */
    if (st->original.w <= area.w && st->original.h <= area.h) {
        *out_w = st->original.w;
        *out_h = st->original.h;
        return;
    }

    int64_t by_width  = (int64_t)area.w * st->original.h;
    int64_t by_height = (int64_t)area.h * st->original.w;

    if (by_width < by_height) {
        *out_w = area.w;
        *out_h = (int32_t)((int64_t)area.w * st->original.h / st->original.w);
    } else {
        *out_h = area.h;
        *out_w = (int32_t)((int64_t)area.h * st->original.w / st->original.h);
    }
    *out_w = MAX(*out_w, 1);
    *out_h = MAX(*out_h, 1);
}

static void view_paint(struct window *win, struct canvas *c)
{
    struct view_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);
    struct rect area = canvas_rect(win);

    /* Ein Schachbrett wie in jedem Bildprogramm: Auf ihm sieht man,
     * wo ein Bild durchsichtig ist. */
    for (int32_t y = area.y; y < area.y + area.h; y += 16) {
        for (int32_t x = area.x; x < area.x + area.w; x += 16) {
            bool dark = (((x - area.x) / 16) + ((y - area.y) / 16)) & 1;

            gfx_fill(&local, rect_make(x, y, 16, 16),
                     dark ? RGB(0x50, 0x50, 0x50) : RGB(0x60, 0x60, 0x60));
        }
    }

    char right[48] = "";

    if (st->original.px) {
        int32_t w, h;

        target_size(st, area, &w, &h);

        if (!st->scaled.px || st->scaled_w != w || st->scaled_h != h) {
            image_free(&st->scaled);
            if (w == st->original.w && h == st->original.h) {
                /* In Originalgroesse wird nicht kopiert - das Bild
                 * selbst tut es auch. */
                st->scaled.px = NULL;
            } else if (!image_scale(&st->original, w, h, &st->scaled)) {
                st->scaled.px = NULL;
            }
            st->scaled_w = w;
            st->scaled_h = h;
        }

        const struct image *shown = st->scaled.px ? &st->scaled : &st->original;

        struct rect before = local.clip;

        gfx_set_clip(&local, rect_intersect(before, area));
        image_draw(&local, area.x + (area.w - shown->w) / 2,
                   area.y + (area.h - shown->h) / 2, shown);
        gfx_set_clip(&local, before);

        char size[24];

        fs_format_size(size, sizeof(size), st->bytes);
        ksnprintf(right, sizeof(right), "%dx%d, %s, %u%%",
                  (int)st->original.w, (int)st->original.h, size,
                  (unsigned)(st->zoom ? st->zoom
                                      : (unsigned)(100LL * w / st->original.w)));
    }

    widget_statusbar(&local,
                     rect_make(0, gui_client_height(win) - STATUS_H,
                               gui_client_width(win), STATUS_H),
                     st->status[0] ? st->status : st->path, right);
}

static void view_event(struct window *win, const struct gui_event *ev)
{
    struct view_state *st = win->user;

    if (ev->type == EV_RESIZED) {
        forget_scaled(st);
        return;
    }

    if (ev->type == EV_SCROLL) {
        /* Rad und Tasten aendern dasselbe. Beim ersten Dreh wird aus
         * dem Einpassen die tatsaechliche Prozentzahl - sonst spraenge
         * das Bild. */
        struct rect area = canvas_rect(win);
        int32_t w, h;

        target_size(st, area, &w, &h);
        if (st->zoom == 0 && st->original.w > 0)
            st->zoom = (uint32_t)(100LL * w / st->original.w);

        int32_t next = (int32_t)st->zoom + (ev->scroll > 0 ? 10 : -10);

        st->zoom = (uint32_t)CLAMP(next, 10, 800);
        gui_invalidate();
        return;
    }

    if (ev->type != EV_KEY_DOWN)
        return;

    switch (ev->key) {
    case KEY_RIGHT:
    case KEY_PAGEDOWN:
        step(win, +1);
        break;
    case KEY_LEFT:
    case KEY_PAGEUP:
        step(win, -1);
        break;
    case '+':
    case '-': {
        struct gui_event wheel = { .type = EV_SCROLL,
                                   .scroll = ev->key == '+' ? 1 : -1 };

        view_event(win, &wheel);
        return;
    }
    case '0':
        st->zoom = 0;              /* wieder einpassen */
        break;
    case '1':
        st->zoom = 100;            /* Originalgroesse  */
        break;
    default:
        return;
    }
    gui_invalidate();
}

static void view_close(struct window *win)
{
    struct view_state *st = win->user;

    image_free(&st->original);
    image_free(&st->scaled);
    kfree(st);
    win->user = NULL;
}

void image_open(struct fs_node *file)
{
    struct view_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    if (!load(st, file)) {
        dialog_message(tr("Bilder"), st->status);
        kfree(st);
        return;
    }

    struct window *win = gui_create_window(st->name, 120, 90, 640, 480,
                                           WF_RESIZABLE, ICON_IMAGE);
    if (!win) {
        image_free(&st->original);
        kfree(st);
        return;
    }

    win->user     = st;
    win->min_w    = 260;
    win->min_h    = 180;
    win->on_paint = view_paint;
    win->on_event = view_event;
    win->on_close = view_close;

    gui_focus_window(win);
}

void app_images(void)
{
    /* Ohne Datei wird der Ordner mit den Beispielbildern gezeigt -
     * von dort ist ein Doppelklick der kuerzeste Weg. */
    struct fs_node *dir = fs_lookup(NULL, "/Medien");

    if (dir)
        filemanager_open(dir);
    else
        app_filemanager();
}
