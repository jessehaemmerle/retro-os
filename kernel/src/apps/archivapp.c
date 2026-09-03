/* archivapp.c - Archive ansehen, auspacken und anlegen.
 *
 * Das Format steht in zip.c; hier steht der Weg vom Dateibaum ins
 * Archiv und zurueck.
 *
 * Beim Packen bekommen die Eintraege Pfade mit Schraegstrichen, wie
 * ZIP es will, und immer relativ zu dem, was gepackt wurde: Wer den
 * Ordner "Dokumente" packt, findet darin "Dokumente/brief.txt" und
 * nicht "/Benutzer/anna/Dokumente/brief.txt". Ein Archiv mit
 * absoluten Pfaden ist auf einem fremden Rechner unbrauchbar.
 *
 * Beim Auspacken wird jeder Name geprueft, bevor er zu einem Pfad
 * wird. Ein Eintrag, der mit einem Schraegstrich anfaengt oder ".."
 * enthaelt, landet sonst irgendwo im System statt im Zielordner -
 * ein alter und immer noch beliebter Weg, ein Archiv als Waffe zu
 * benutzen.
 */

#include "apps.h"
#include "font.h"
#include "kstring.h"
#include "lang.h"
#include "mm.h"
#include "rtc.h"
#include "theme.h"
#include "vfs.h"
#include "widgets.h"
#include "widgets.h"
#include "zip.h"

#define ROW_H     18
#define HEADER_H  46
#define STATUS_H  26
#define MAX_SHOWN 512

struct arc_state {
    char     path[FS_PATH_MAX];
    uint8_t *data;
    size_t   length;

    struct zip_entry shown[MAX_SHOWN];
    size_t   count;
    int32_t  selection;
    int32_t  scroll;

    char     status[96];
};

/* ------------------------------------------------------------------ */
/* Packen                                                              */
/* ------------------------------------------------------------------ */

static void now_stamp(uint16_t *date, uint16_t *time)
{
    struct datetime now;

    rtc_read(&now);
    *date = zip_dos_date(now.year, now.month, now.day);
    *time = zip_dos_time(now.hour, now.minute, now.second);
}

/* Haengt einen Knoten und alles darunter an. prefix ist der Pfad im
 * Archiv, ohne fuehrenden Schraegstrich. */
static bool pack_node(struct zip_writer *w, struct fs_node *node,
                      const char *prefix, int depth)
{
    if (!node || depth > 12)
        return false;

    char name[FS_PATH_MAX];

    if (prefix[0])
        ksnprintf(name, sizeof(name), "%s/%s", prefix, node->name);
    else
        strlcpy(name, node->name, sizeof(name));

    uint16_t date, time;

    now_stamp(&date, &time);

    if (node->type == FS_DIR) {
        char with_slash[FS_PATH_MAX];

        ksnprintf(with_slash, sizeof(with_slash), "%s/", name);
        if (!zip_add(w, with_slash, NULL, 0, date, time))
            return false;

        struct fs_node *entries[128];
        size_t n = fs_list(node, entries, ARRAY_LEN(entries));

        for (size_t i = 0; i < n; i++)
            if (!pack_node(w, entries[i], name, depth + 1))
                return false;
        return true;
    }

    if (!fs_load(node))
        return false;
    return zip_add(w, name, node->data, node->size, date, time);
}

/* Packt einen Eintrag in ein Archiv daneben. */
bool archive_pack(struct fs_node *node, char *out_path, size_t out_size,
                  char *error, size_t error_size)
{
    if (!node || !node->parent) {
        strlcpy(error, tr("Das laesst sich nicht packen."), error_size);
        return false;
    }

    struct zip_writer *w = zip_begin();

    if (!w) {
        strlcpy(error, tr("Zu wenig Speicher."), error_size);
        return false;
    }

    if (!pack_node(w, node, "", 0)) {
        zip_abort(w);
        strlcpy(error, tr("Das Archiv liess sich nicht fuellen."), error_size);
        return false;
    }

    size_t length = 0;
    uint8_t *archive = zip_finish(w, &length);

    if (!archive) {
        strlcpy(error, tr("Das Archiv liess sich nicht abschliessen."),
                error_size);
        return false;
    }

    char name[FS_NAME_MAX + 1];

    ksnprintf(name, sizeof(name), "%s.zip", node->name);

    struct fs_node *file = fs_find_child(node->parent, name);

    if (!file)
        file = fs_create(node->parent, name, FS_FILE);

    bool ok = file && file->type == FS_FILE && fs_write(file, archive, length);

    kfree(archive);

    if (!ok) {
        strlcpy(error, tr("Die Datei liess sich nicht schreiben."), error_size);
        return false;
    }

    fs_path(file, out_path, out_size);
    return true;
}

/* ------------------------------------------------------------------ */
/* Auspacken                                                           */
/* ------------------------------------------------------------------ */

/* Ist der Name aus dem Archiv harmlos? Alles andere waere ein Pfad,
 * der aus dem Zielordner hinausfuehrt. */
static bool name_is_safe(const char *name)
{
    if (!name || !name[0] || name[0] == '/' || name[0] == '\\')
        return false;

    /* Ein Doppelpunkt kaeme von einem Laufwerksbuchstaben. */
    if (strchr(name, ':') || strchr(name, '\\'))
        return false;

    for (const char *p = name; *p; p++) {
        if (p[0] != '.' || p[1] != '.')
            continue;
        /* ".." zaehlt nur als Abschnitt, nicht als Teil eines Namens
         * wie "..punkte.txt". */
        bool left = (p == name) || p[-1] == '/';
        bool right = p[2] == '\0' || p[2] == '/';

        if (left && right)
            return false;
    }
    return true;
}

/* Legt einen Pfad unterhalb von base an - Ordner fuer Ordner. */
static struct fs_node *make_path(struct fs_node *base, const char *relative,
                                 bool as_dir)
{
    char part[FS_NAME_MAX + 1];
    struct fs_node *at = base;
    const char *p = relative;

    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);

        if (len == 0 || len > FS_NAME_MAX)
            return NULL;

        memcpy(part, p, len);
        part[len] = '\0';

        bool last = !slash || !slash[1];
        bool want_dir = !last || as_dir;

        struct fs_node *next = fs_find_child(at, part);

        if (!next)
            next = fs_create(at, part, want_dir ? FS_DIR : FS_FILE);
        if (!next)
            return NULL;

        at = next;
        if (!slash)
            break;
        p = slash + 1;
    }
    return at;
}

bool archive_unpack(struct fs_node *file, char *out_path, size_t out_size,
                    char *error, size_t error_size)
{
    if (!file || file->type != FS_FILE || !fs_load(file) || !file->data) {
        strlcpy(error, tr("Die Datei laesst sich nicht lesen."), error_size);
        return false;
    }

    size_t count = 0;

    if (!zip_read(file->data, file->size, &count)) {
        strlcpy(error, tr("Das ist kein Archiv."), error_size);
        return false;
    }

    /* Ausgepackt wird in einen Ordner mit dem Namen des Archivs - so
     * verstreut ein Archiv mit vielen Dateien nicht den ganzen
     * Ordner, in dem es liegt.
     *
     * Gibt es den Namen schon, wird eine Zahl angehaengt. Das ist
     * kein Schoenheitsdienst: "Dokumente" gepackt ergibt
     * "Dokumente.zip", und das wieder ausgepackt wollte sonst genau
     * in den Ordner, aus dem es kam - der Inhalt laege danach
     * doppelt darin. */
    char base[FS_NAME_MAX + 1];
    char folder[FS_NAME_MAX + 1];

    strlcpy(base, file->name, sizeof(base));

    char *dot = strrchr(base, '.');

    if (dot && dot != base)
        *dot = '\0';

    strlcpy(folder, base, sizeof(folder));

    for (int attempt = 2; attempt < 100; attempt++) {
        if (!fs_find_child(file->parent, folder))
            break;
        ksnprintf(folder, sizeof(folder), "%s %d", base, attempt);
    }

    struct fs_node *target = fs_create(file->parent, folder, FS_DIR);

    if (!target || target->type != FS_DIR) {
        strlcpy(error, tr("Der Zielordner liess sich nicht anlegen."),
                error_size);
        return false;
    }

    size_t written = 0;
    size_t skipped = 0;

    for (size_t i = 0; i < count; i++) {
        struct zip_entry entry;

        if (!zip_entry(file->data, file->size, i, &entry))
            break;

        if (!name_is_safe(entry.name)) {
            skipped++;
            continue;
        }

        if (entry.is_dir) {
            char without[ZIP_NAME_MAX + 1];

            strlcpy(without, entry.name, sizeof(without));
            without[strlen(without) - 1] = '\0';
            make_path(target, without, true);
            continue;
        }

        size_t length = 0;
        void *content = zip_extract(file->data, file->size, &entry, &length);

        if (!content) {
            skipped++;
            continue;
        }

        struct fs_node *out = make_path(target, entry.name, false);

        if (out && out->type == FS_FILE && fs_write(out, content, length))
            written++;
        else
            skipped++;

        kfree(content);
    }

    fs_path(target, out_path, out_size);

    if (skipped)
        ksnprintf(error, error_size, tr("%u ausgepackt, %u uebergangen."),
                  (unsigned)written, (unsigned)skipped);
    else
        ksnprintf(error, error_size, tr("%u Eintraege ausgepackt."),
                  (unsigned)written);
    return written > 0 || count == 0;
}

/* ------------------------------------------------------------------ */
/* Fenster                                                             */
/* ------------------------------------------------------------------ */

static struct rect list_rect(struct window *win)
{
    return rect_make(0, HEADER_H, gui_client_width(win) - SCROLLBAR_WIDTH,
                     gui_client_height(win) - HEADER_H - STATUS_H);
}

static void reload(struct arc_state *st)
{
    st->count = 0;

    size_t count = 0;

    if (!st->data || !zip_read(st->data, st->length, &count))
        return;

    for (size_t i = 0; i < count && st->count < MAX_SHOWN; i++)
        if (zip_entry(st->data, st->length, i, &st->shown[st->count]))
            st->count++;
}

static void arc_paint(struct window *win, struct canvas *c)
{
    struct arc_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);
    struct rect area = list_rect(win);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    widget_toolbar(&local, rect_make(0, 0, local.w, HEADER_H));
    widget_icon_button(&local, rect_make(6, 8, 130, 28), ICON_DOWNLOAD,
                       tr("Auspacken"), false, st->count > 0);

    gfx_text(&local, 146, 16, st->path, COL_TEXT_DIM);

    gfx_fill(&local, area, COL_FIELD);
    gfx_bevel_thin(&local, area, false);

    int32_t rows = area.h / ROW_H;

    for (int32_t i = 0; i < rows; i++) {
        size_t index = (size_t)(st->scroll + i);

        if (index >= st->count)
            break;

        const struct zip_entry *e = &st->shown[index];
        int32_t y = area.y + i * ROW_H;
        bool selected = (int32_t)index == st->selection;

        if (selected)
            gfx_fill(&local, rect_make(area.x + 1, y, area.w - 2, ROW_H),
                     COL_SELECT);

        uint32_t colour = selected ? COL_SELECT_TEXT : COL_TEXT;

        icon_draw(&local, area.x + 4, y + 1,
                  e->is_dir ? ICON_FOLDER : ICON_FILE, 1);
        gfx_text_clipped(&local, area.x + 24, y + 2, e->name, colour,
                         area.w - 274);

        if (e->is_dir)
            continue;

        char size[24], packed[24];

        fs_format_size(size, sizeof(size), (size_t)e->size);
        fs_format_size(packed, sizeof(packed), (size_t)e->packed);
        gfx_text(&local, area.x + area.w - 240, y + 2, size, colour);
        gfx_text(&local, area.x + area.w - 150, y + 2, packed, colour);
        gfx_text(&local, area.x + area.w - 66, y + 2,
                 e->method == 8 ? tr("gepackt") : tr("roh"), colour);
    }

    widget_vscroll(&local,
                   rect_make(local.w - SCROLLBAR_WIDTH, area.y,
                             SCROLLBAR_WIDTH, area.h),
                   st->scroll, (int32_t)st->count, rows);

    char right[32];

    ksnprintf(right, sizeof(right), tr("%u Eintraege"), (unsigned)st->count);
    widget_statusbar(&local,
                     rect_make(0, gui_client_height(win) - STATUS_H,
                               gui_client_width(win), STATUS_H),
                     st->status, right);
}

static void do_unpack(struct window *win)
{
    struct arc_state *st = win->user;
    struct fs_node *file = fs_lookup(NULL, st->path);
    char target[FS_PATH_MAX];
    char message[96];

    if (!file) {
        strlcpy(st->status, tr("Das Archiv ist nicht mehr da."),
                sizeof(st->status));
        return;
    }

    if (archive_unpack(file, target, sizeof(target), message, sizeof(message)))
        ksnprintf(st->status, sizeof(st->status), "%s - %s", message, target);
    else
        strlcpy(st->status, message, sizeof(st->status));
}

static void arc_event(struct window *win, const struct gui_event *ev)
{
    struct arc_state *st = win->user;
    struct rect area = list_rect(win);
    int32_t rows = area.h / ROW_H;

    switch (ev->type) {
    case EV_MOUSE_DOWN:
        if (rect_contains(rect_make(6, 8, 130, 28), ev->x, ev->y)) {
            do_unpack(win);
            gui_invalidate();
            return;
        }
        if (rect_contains(area, ev->x, ev->y)) {
            int32_t index = st->scroll + (ev->y - area.y) / ROW_H;

            if (index >= 0 && index < (int32_t)st->count)
                st->selection = index;
            gui_invalidate();
        }
        return;
    case EV_SCROLL:
        st->scroll = CLAMP(st->scroll - ev->scroll * 3, 0,
                           MAX((int32_t)st->count - rows, 0));
        gui_invalidate();
        return;
    case EV_KEY_DOWN:
        if (ev->key == KEY_DOWN && st->selection + 1 < (int32_t)st->count)
            st->selection++;
        else if (ev->key == KEY_UP && st->selection > 0)
            st->selection--;
        else
            return;

        if (st->selection < st->scroll)
            st->scroll = st->selection;
        else if (st->selection >= st->scroll + rows)
            st->scroll = st->selection - rows + 1;
        gui_invalidate();
        return;
    default:
        return;
    }
}

static void arc_close(struct window *win)
{
    struct arc_state *st = win->user;

    kfree(st->data);
    kfree(st);
    win->user = NULL;
}

void archive_open(struct fs_node *file)
{
    if (!file || file->type != FS_FILE || !fs_load(file) || !file->data) {
        dialog_message(tr("Archiv"), tr("Die Datei laesst sich nicht lesen."));
        return;
    }

    size_t count = 0;

    if (!zip_read(file->data, file->size, &count)) {
        dialog_message(tr("Archiv"), tr("Das ist kein Archiv."));
        return;
    }

    struct arc_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    /* Der Inhalt wird kopiert: Der Dateibaum darf sich unter dem
     * Fenster weiterbewegen, und ein Zeiger auf fs-Daten waere nach
     * dem naechsten Schreiben irgendwo. */
    st->data = kmalloc(file->size);
    if (!st->data) {
        kfree(st);
        return;
    }
    memcpy(st->data, file->data, file->size);
    st->length = file->size;
    st->selection = -1;
    fs_path(file, st->path, sizeof(st->path));
    reload(st);

    struct window *win = gui_create_window(file->name, 140, 100, 620, 400,
                                           WF_RESIZABLE, ICON_ARCHIVE);
    if (!win) {
        kfree(st->data);
        kfree(st);
        return;
    }

    win->user     = st;
    win->min_w    = 420;
    win->min_h    = 220;
    win->on_paint = arc_paint;
    win->on_event = arc_event;
    win->on_close = arc_close;

    gui_focus_window(win);
}

void app_archive(void)
{
    /* Ohne Archiv gibt es nichts zu zeigen - der Dateimanager ist der
     * Weg dorthin. */
    app_filemanager();
}
