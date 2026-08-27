/* browser.c - der Webbrowser von RetroOS.
 *
 * Der Ablauf entspricht dem eines richtigen Browsers:
 *
 *   1. Die Seite wird geholt und zu einem Dokumentbaum zerlegt.
 *   2. Eingebundene Formatvorlagen, Skripte und Bilder werden
 *      nachgeladen - jedes davon ein eigener Auftrag an den
 *      Arbeits-Thread, damit die Oberflaeche bedienbar bleibt.
 *   3. Die Formatvorlagen werden gewichtet und auf den Baum gelegt.
 *   4. Die Skripte laufen und duerfen den Baum veraendern.
 *   5. Der Baum wird nach dem Kastenmodell umgebrochen und gezeichnet.
 *
 * Aendert ein Skript spaeter etwas - etwa durch einen Klick oder einen
 * Zeitgeber - werden die Schritte drei bis fuenf wiederholt.
 *
 * Vier Adressarten werden verstanden:
 *   http://...   eine Seite aus dem Netz
 *   https://...  dasselbe, verschluesselt
 *   datei:/...   eine Datei aus dem Dateisystem
 *   start:       die eingebaute Startseite
 */

#include "apps.h"
#include "clipboard.h"
#include "css.h"
#include "font.h"
#include "htmlparse.h"
#include "image.h"
#include "js.h"
#include "kstring.h"
#include "layout.h"
#include "mm.h"
#include "net.h"
#include "arch.h"
#include "thread.h"
#include "theme.h"
#include "widgets.h"

#define BR_TOOLBAR_H 34
#define BR_STATUS_H  24
#define BR_MARGIN    10
#define BR_LINE      18
#define BR_URL_MAX   255
#define BR_HISTORY   16
#define BR_RESOURCES 32
#define BR_SHEET_MAX 65536

#define COL_PAGE_BG   RGB(0xFF, 0xFF, 0xFF)
#define COL_PLACE     RGB(0x88, 0x88, 0x88)

enum br_button { BR_BACK, BR_RELOAD, BR_HOME, BR_GO, BR_COUNT };

/* ------------------------------------------------------------------ */
/* Nachgeladene Bestandteile                                           */
/* ------------------------------------------------------------------ */

enum resource_kind { RES_STYLE, RES_SCRIPT, RES_IMAGE };

enum resource_state { RES_WAITING, RES_BUSY, RES_READY, RES_FAILED };

struct resource {
    char  url[BR_URL_MAX + 1];
    enum resource_kind  kind;
    volatile int        state;

    struct image image;         /* bei RES_IMAGE */
    char        *text;          /* bei RES_STYLE und RES_SCRIPT */
    size_t       length;
};

/* Der Ladeauftrag wandert zwischen Oberflaeche und Arbeits-Thread hin und
 * her. Nur ein Feld wechselt dabei die Richtung, deshalb genuegt ein
 * Zustandswert ohne weitere Absprache. */
enum job_state { JOB_IDLE, JOB_REQUESTED, JOB_RUNNING, JOB_DONE };

struct load_job {
    char     url[BR_URL_MAX + 1];
    volatile int state;
    int32_t  resource;          /* -1 = die Seite selbst */

    struct http_response response;
    bool     ok;
};

/* ------------------------------------------------------------------ */
/* Zustand eines Fensters                                              */
/* ------------------------------------------------------------------ */

enum br_phase {
    PHASE_LEER,
    PHASE_SEITE,        /* wartet auf das Hauptdokument   */
    PHASE_BESTANDTEILE, /* wartet auf Vorlagen und Bilder */
    PHASE_FERTIG,
};

struct br_state {
    struct document    doc;
    struct stylesheet *sheet;
    struct layout      layout;
    struct js_context *js;
    bool               scripts_ran;

    struct resource resources[BR_RESOURCES];
    size_t          resource_count;

    struct load_job  job;
    struct thread   *worker;
    volatile bool    worker_quit;
    volatile bool    worker_done;

    int32_t phase;

    char url[BR_URL_MAX + 1];
    char address[BR_URL_MAX + 1];
    int32_t address_cursor;
    bool    address_focus;
    bool    address_selected;
    bool    caret_on;

    char history[BR_HISTORY][BR_URL_MAX + 1];
    int  history_len;

    char status[192];
    char security[64];

    int32_t scroll;
    int32_t layout_width;

    struct node *focused;       /* Eingabefeld unter dem Schreibzeiger */
    struct node *hovered;

    int pressed;
    bool needs_layout;
    bool needs_restyle;

    /* Der Browser darf sich selbst eine neue Adresse geben. */
    char     pending_url[BR_URL_MAX + 1];
    bool     has_pending;
};

static void browser_navigate(struct window *win, const char *url,
                             bool remember);
static void rebuild_page(struct window *win);

/* ------------------------------------------------------------------ */
/* Adressen                                                            */
/* ------------------------------------------------------------------ */

/* Setzt eine relative Adresse mit der aktuellen Seite zusammen. */
static void resolve_url(const char *base, const char *href, char *out,
                        size_t size)
{
    if (!href || !href[0]) {
        strlcpy(out, base, size);
        return;
    }

    while (*href == ' ' || *href == '\t' || *href == '\n')
        href++;

    if (strncasecmp(href, "http://", 7) == 0 ||
        strncasecmp(href, "https://", 8) == 0 ||
        strncasecmp(href, "datei:", 6) == 0 ||
        strncasecmp(href, "start:", 6) == 0) {
        strlcpy(out, href, size);
        return;
    }
    if (strncasecmp(href, "data:", 5) == 0 ||
        strncasecmp(href, "javascript:", 11) == 0 ||
        strncasecmp(href, "mailto:", 7) == 0) {
        strlcpy(out, href, size);
        return;
    }

    if (href[0] == '#') {                    /* Sprungmarke - bleibt hier */
        strlcpy(out, base, size);
        return;
    }

    if (href[0] == '/' && href[1] == '/') {
        /* "//rechner/pfad" uebernimmt das Schema der aktuellen Seite. */
        bool secure = strncasecmp(base, "https://", 8) == 0;

        ksnprintf(out, size, "%s:%s", secure ? "https" : "http", href);
        return;
    }

    /* Innerhalb des Dateisystems bleiben. */
    if (strncasecmp(base, "datei:", 6) == 0) {
        char directory[BR_URL_MAX + 1];

        strlcpy(directory, base + 6, sizeof(directory));

        char *slash = strrchr(directory, '/');

        if (slash)
            slash[1] = '\0';

        if (href[0] == '/')
            ksnprintf(out, size, "datei:%s", href);
        else
            ksnprintf(out, size, "datei:%s%s", directory, href);
        return;
    }

    char host[128], path[512];
    uint16_t port;
    bool secure = false;

    if (!url_split(base, host, sizeof(host), &port, path, sizeof(path),
                   &secure)) {
        strlcpy(out, href, size);
        return;
    }

    const char *scheme = secure ? "https" : "http";
    uint16_t standard = secure ? 443 : 80;
    char prefix[160];

    if (port == standard)
        ksnprintf(prefix, sizeof(prefix), "%s://%s", scheme, host);
    else
        ksnprintf(prefix, sizeof(prefix), "%s://%s:%u", scheme, host, port);

    if (href[0] == '/') {
        ksnprintf(out, size, "%s%s", prefix, href);
        return;
    }

    char *slash = strrchr(path, '/');

    if (slash)
        slash[1] = '\0';
    else
        strlcpy(path, "/", sizeof(path));

    ksnprintf(out, size, "%s%s%s", prefix, path, href);
}

/* ------------------------------------------------------------------ */
/* Bestandteile verwalten                                              */
/* ------------------------------------------------------------------ */

static void release_resources(struct br_state *st)
{
    for (size_t i = 0; i < st->resource_count; i++) {
        image_free(&st->resources[i].image);
        kfree(st->resources[i].text);
        st->resources[i].text = NULL;
    }
    st->resource_count = 0;
}

static struct resource *find_resource(struct br_state *st, const char *url)
{
    for (size_t i = 0; i < st->resource_count; i++)
        if (strcmp(st->resources[i].url, url) == 0)
            return &st->resources[i];
    return NULL;
}

static struct resource *add_resource(struct br_state *st, const char *url,
                                     enum resource_kind kind)
{
    struct resource *found = find_resource(st, url);

    if (found)
        return found;
    if (st->resource_count >= BR_RESOURCES)
        return NULL;

    struct resource *r = &st->resources[st->resource_count++];

    memset(r, 0, sizeof(*r));
    strlcpy(r->url, url, sizeof(r->url));
    r->kind = kind;
    r->state = RES_WAITING;
    return r;
}

/* Der Umbruch fragt hierueber nach dem Bild zu einer Adresse. */
static struct image *lookup_image(void *context, const char *src)
{
    struct br_state *st = context;
    char full[BR_URL_MAX + 1];

    resolve_url(st->url, src, full, sizeof(full));

    struct resource *r = find_resource(st, full);

    if (!r || r->state != RES_READY || !r->image.px)
        return NULL;
    return &r->image;
}

/* ------------------------------------------------------------------ */
/* Bestandteile im Baum finden                                         */
/* ------------------------------------------------------------------ */

static void scan_node(struct br_state *st, struct node *node)
{
    if (node->kind == NODE_ELEMENT && node->name) {
        if (strcmp(node->name, "link") == 0) {
            const char *rel = node_attribute(node, "rel");
            const char *href = node_attribute(node, "href");

            if (href && rel && strcasecmp(rel, "stylesheet") == 0) {
                char full[BR_URL_MAX + 1];

                resolve_url(st->url, href, full, sizeof(full));
                if (strncasecmp(full, "data:", 5) != 0)
                    add_resource(st, full, RES_STYLE);
            }
        } else if (strcmp(node->name, "script") == 0) {
            const char *src = node_attribute(node, "src");
            const char *type = node_attribute(node, "type");
            bool usable = !type || strcasecmp(type, "text/javascript") == 0 ||
                          strcasecmp(type, "application/javascript") == 0 ||
                          strcasecmp(type, "module") == 0 || !*type;

            if (src && usable) {
                char full[BR_URL_MAX + 1];

                resolve_url(st->url, src, full, sizeof(full));
                if (strncasecmp(full, "data:", 5) != 0)
                    add_resource(st, full, RES_SCRIPT);
            }
        } else if (strcmp(node->name, "img") == 0) {
            const char *src = node_attribute(node, "src");

            if (src && *src) {
                char full[BR_URL_MAX + 1];

                resolve_url(st->url, src, full, sizeof(full));
                if (strncasecmp(full, "data:", 5) != 0)
                    add_resource(st, full, RES_IMAGE);
            }
        }
    }
    for (struct node *c = node->first; c; c = c->next)
        scan_node(st, c);
}

/* ------------------------------------------------------------------ */
/* Formatvorlagen und Skripte anwenden                                 */
/* ------------------------------------------------------------------ */

static void collect_styles(struct br_state *st)
{
    css_free(st->sheet);
    st->sheet = css_create();
    if (!st->sheet)
        return;

    char *buffer = kmalloc(BR_SHEET_MAX);

    if (!buffer)
        return;

    /* Erst die eingebundenen Dateien, dann die style-Bloecke der Seite -
     * so gewinnt bei gleichem Gewicht das Naeherliegende. */
    for (size_t i = 0; i < st->resource_count; i++) {
        struct resource *r = &st->resources[i];

        if (r->kind == RES_STYLE && r->state == RES_READY && r->text)
            css_add(st->sheet, r->text, r->length);
    }

    for (size_t i = 0; ; i++) {
        struct node *style = dom_by_tag(st->doc.root, "style", i);

        if (!style)
            break;
        dom_raw_text(style, buffer, BR_SHEET_MAX);
        css_add(st->sheet, buffer, strlen(buffer));
    }
    kfree(buffer);
}

/* Wird gerufen, wenn ein Skript den Baum veraendert hat. */
static void on_document_changed(void *context)
{
    struct br_state *st = context;

    st->needs_restyle = true;
    st->needs_layout = true;
}

static void on_script_navigate(void *context, const char *url)
{
    struct br_state *st = context;

    strlcpy(st->pending_url, url, sizeof(st->pending_url));
    st->has_pending = true;
}

static void run_scripts(struct window *win)
{
    struct br_state *st = win->user;

    if (!st->js)
        return;

    js_bind_document(st->js, &st->doc);

    char *buffer = kmalloc(BR_SHEET_MAX);

    if (!buffer)
        return;

    for (size_t i = 0; ; i++) {
        struct node *script = dom_by_tag(st->doc.root, "script", i);

        if (!script)
            break;

        const char *src = node_attribute(script, "src");

        if (src && *src) {
            char full[BR_URL_MAX + 1];

            resolve_url(st->url, src, full, sizeof(full));

            struct resource *r = find_resource(st, full);

            if (r && r->state == RES_READY && r->text)
                js_run(st->js, r->text, r->length);
            continue;
        }

        dom_raw_text(script, buffer, BR_SHEET_MAX);
        if (buffer[0])
            js_run(st->js, buffer, strlen(buffer));
    }
    kfree(buffer);

    /* Ein Skript darf sich an das Laden der Seite haengen. */
    js_dispatch_event(st->js, st->doc.root, "DOMContentLoaded");
    js_dispatch_event(st->js, st->doc.root, "load");
}

/* ------------------------------------------------------------------ */
/* Umbruch                                                             */
/* ------------------------------------------------------------------ */

static struct rect page_rect(struct window *win);

static void clear_boxes(struct node *node)
{
    node->has_box = false;
    for (struct node *c = node->first; c; c = c->next)
        clear_boxes(c);
}

static void relayout(struct window *win)
{
    struct br_state *st = win->user;
    struct rect area = page_rect(win);
    int32_t width = MAX(area.w - 2 * BR_MARGIN - 4, 80);

    if (st->needs_restyle) {
        collect_styles(st);
        css_apply(st->sheet, st->doc.root, 16, width, area.h);
        st->needs_restyle = false;
    }

    clear_boxes(st->doc.root);
    layout_run(&st->layout, st->doc.body, width, lookup_image, st);
    st->layout_width = width;
    st->needs_layout = false;
    st->scroll = MIN(st->scroll,
                     MAX(st->layout.height - area.h + 2 * BR_MARGIN, 0));
}

static void rebuild_page(struct window *win)
{
    struct br_state *st = win->user;

    st->needs_restyle = true;
    if (!st->scripts_ran) {
        struct rect area = page_rect(win);

        collect_styles(st);
        css_apply(st->sheet, st->doc.root, 16,
                  MAX(area.w - 2 * BR_MARGIN - 4, 80), area.h);
        st->needs_restyle = false;
        run_scripts(win);
        st->scripts_ran = true;
    }
    st->needs_restyle = true;
    relayout(win);
    gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Seiten laden                                                        */
/* ------------------------------------------------------------------ */

static const char start_page[] =
    "<html><head><title>RetroOS</title><style>"
    "body { background: #fdfdf8; color: #202020; font-family: sans-serif;"
    "       margin: 18px }"
    "h1 { color: #204878; border-bottom: 2px solid #6890c0;"
    "     padding-bottom: 6px }"
    "h2 { color: #305888 }"
    ".kasten { background: #eef2f8; border: 1px solid #b8c8dc;"
    "          padding: 10px 14px; margin: 12px 0 }"
    ".hinweis { color: #606060; font-size: 14px }"
    "code { background: #e8e8e0; padding: 1px 4px }"
    "</style></head><body>"
    "<h1>RetroOS-Browser</h1>"
    "<p>Ein Browser mit Dokumentbaum, Formatvorlagen, Bildern und "
    "JavaScript. Gib oben eine Adresse ein oder folge einem Verweis.</p>"
    "<div class=\"kasten\">"
    "<h2>Aus dem Dateisystem</h2>"
    "<ul>"
    "<li><a href=\"datei:/Dokumente/beispiel.html\">Beispielseite</a></li>"
    "<li><a href=\"datei:/Dokumente/pruefung.html\">Selbsttest der "
    "Darstellung</a></li>"
    "<li><a href=\"datei:/Dokumente/willkommen.txt\">Willkommenstext</a></li>"
    "<li><a href=\"datei:/System/version.txt\">Systemversion</a></li>"
    "</ul>"
    "</div>"
    "<h2>Was der Browser kann</h2>"
    "<ul>"
    "<li>HTTPS mit TLS 1.3, X25519 und AES-GCM oder ChaCha20-Poly1305, "
    "mit Pruefung der Zertifikatskette</li>"
    "<li>Formatvorlagen mit Kaskade, Kastenmodell, Farben, Schriftgroessen "
    "und schwebenden Kaesten</li>"
    "<li>Bilder in PNG, JPEG, GIF und BMP</li>"
    "<li>JavaScript mit Zugriff auf den Dokumentbaum, Ereignissen und "
    "Zeitgebern</li>"
    "</ul>"
    "<p class=\"hinweis\">Adressen ohne Vorsatz werden als "
    "<code>http://</code> gelesen.</p>"
    "</body></html>";

static void reset_document(struct br_state *st)
{
    layout_free(&st->layout);
    document_free(&st->doc);
    release_resources(st);
    if (st->js) {
        js_destroy(st->js);
        st->js = NULL;
    }
    st->js = js_create();
    if (st->js) {
        js_on_change(st->js, on_document_changed, st);
        js_on_navigate(st->js, on_script_navigate, st);
    }
    st->scripts_ran = false;
    st->focused = NULL;
    document_init(&st->doc);
}

static void load_start_page(struct br_state *st)
{
    reset_document(st);
    html_build(&st->doc, start_page, sizeof(start_page) - 1);
    strlcpy(st->status, "Startseite", sizeof(st->status));
    st->security[0] = '\0';
}

static bool load_local(struct br_state *st, const char *path)
{
    struct fs_node *node = fs_lookup(fs_root(), path);

    if (!node || node->type != FS_FILE) {
        ksnprintf(st->status, sizeof(st->status), "Datei nicht gefunden: %s",
                  path);
        return false;
    }
    if (!fs_load(node)) {
        strlcpy(st->status, "Die Datei laesst sich nicht lesen.",
                sizeof(st->status));
        return false;
    }

    reset_document(st);

    const char *dot = strrchr(node->name, '.');
    bool is_html = dot && (strcasecmp(dot, ".html") == 0 ||
                           strcasecmp(dot, ".htm") == 0);

    if (is_html)
        html_build(&st->doc, (const char *)node->data, node->size);
    else
        html_build_plain(&st->doc, (const char *)node->data, node->size);

    ksnprintf(st->status, sizeof(st->status), "%s - %u Byte", node->name,
              (unsigned)node->size);
    st->security[0] = '\0';
    return true;
}

/* Laedt einen Bestandteil aus dem Dateisystem. */
static bool load_local_resource(struct resource *r, const char *path)
{
    struct fs_node *node = fs_lookup(fs_root(), path);

    if (!node || node->type != FS_FILE || !fs_load(node))
        return false;

    if (r->kind == RES_IMAGE)
        return image_decode(node->data, node->size, &r->image);

    r->text = kmalloc(node->size + 1);
    if (!r->text)
        return false;
    memcpy(r->text, node->data, node->size);
    r->text[node->size] = '\0';
    r->length = node->size;
    return true;
}

/* Wertet das Ergebnis des Arbeits-Threads fuer die Seite aus. */
static bool finish_http(struct br_state *st)
{
    struct http_response *response = &st->job.response;

    if (!st->job.ok) {
        strlcpy(st->status, response->error[0]
                    ? response->error
                    : "Die Seite konnte nicht geladen werden.",
                sizeof(st->status));
        return false;
    }

    reset_document(st);

    if (strncasecmp(response->content_type, "text/plain", 10) == 0 ||
        strncasecmp(response->content_type, "application/json", 16) == 0)
        html_build_plain(&st->doc, response->body, response->body_length);
    else
        html_build(&st->doc, response->body, response->body_length);

    strlcpy(st->security, response->security[0] ? response->security
                                                : "unverschluesselt",
            sizeof(st->security));
    ksnprintf(st->status, sizeof(st->status), "%d - %u Byte%s - %s - %s",
              response->status, (unsigned)response->body_length,
              response->truncated ? " (unvollstaendig)" : "",
              response->content_type, st->security);

    http_response_free(response);
    return true;
}

/* Wertet das Ergebnis fuer einen nachgeladenen Bestandteil aus. */
static void finish_resource(struct br_state *st, struct resource *r)
{
    struct http_response *response = &st->job.response;

    if (!st->job.ok) {
        r->state = RES_FAILED;
        return;
    }

    if (r->kind == RES_IMAGE) {
        r->state = image_decode((const uint8_t *)response->body,
                                response->body_length, &r->image)
                   ? RES_READY : RES_FAILED;
    } else {
        r->text = kmalloc(response->body_length + 1);
        if (r->text) {
            memcpy(r->text, response->body, response->body_length);
            r->text[response->body_length] = '\0';
            r->length = response->body_length;
            r->state = RES_READY;
        } else {
            r->state = RES_FAILED;
        }
    }
    http_response_free(response);
}

/* Laeuft im Arbeits-Thread: holt, waehrend die Oberflaeche weiterlaeuft. */
static void browser_worker(void *argument)
{
    struct br_state *st = argument;

    while (!st->worker_quit) {
        if (st->job.state != JOB_REQUESTED) {
            wait_on(&st->job, NULL, 200);
            continue;
        }

        st->job.state = JOB_RUNNING;
        st->job.ok = http_get(st->job.url, &st->job.response);
        st->job.state = JOB_DONE;
    }

    st->worker_done = true;
}

/* Gibt den naechsten offenen Bestandteil in Auftrag. */
static bool post_next_resource(struct window *win)
{
    struct br_state *st = win->user;

    if (st->job.state != JOB_IDLE)
        return true;

    for (size_t i = 0; i < st->resource_count; i++) {
        struct resource *r = &st->resources[i];

        if (r->state != RES_WAITING)
            continue;

        /* Aus dem Dateisystem geht es ohne Umweg. */
        if (strncasecmp(r->url, "datei:", 6) == 0) {
            r->state = load_local_resource(r, r->url + 6) ? RES_READY
                                                          : RES_FAILED;
            continue;
        }
        if (strncasecmp(r->url, "http", 4) != 0 || !net_ready() ||
            !st->worker) {
            r->state = RES_FAILED;
            continue;
        }

        r->state = RES_BUSY;
        strlcpy(st->job.url, r->url, sizeof(st->job.url));
        st->job.resource = (int32_t)i;
        st->job.state = JOB_REQUESTED;
        wake_one(&st->job);

        size_t done = 0;

        for (size_t k = 0; k < st->resource_count; k++)
            if (st->resources[k].state == RES_READY ||
                st->resources[k].state == RES_FAILED)
                done++;
        ksnprintf(st->status, sizeof(st->status),
                  "Lade Bestandteil %u von %u ...",
                  (unsigned)(done + 1), (unsigned)st->resource_count);
        return true;
    }
    return false;
}

static void document_ready(struct window *win)
{
    struct br_state *st = win->user;
    char title[WIN_TITLE_MAX + 1];

    st->phase = PHASE_FERTIG;
    rebuild_page(win);

    ksnprintf(title, sizeof(title), "Browser - %s",
              st->doc.title[0] ? st->doc.title : st->url);
    gui_set_title(win, title);

    if (st->security[0])
        ksnprintf(st->status, sizeof(st->status), "%s%s%s",
                  st->doc.title[0] ? st->doc.title : st->url,
                  " - ", st->security);
    gui_invalidate();
}

static void after_document(struct window *win)
{
    struct br_state *st = win->user;

    scan_node(st, st->doc.root);

    st->phase = PHASE_BESTANDTEILE;
    if (!post_next_resource(win))
        document_ready(win);
    else
        gui_invalidate();
}

static void do_load(struct window *win, const char *url)
{
    struct br_state *st = win->user;

    st->scroll = 0;

    if (strncasecmp(url, "start:", 6) == 0)
        load_start_page(st);
    else if (strncasecmp(url, "datei:", 6) == 0) {
        if (!load_local(st, url + 6)) {
            st->phase = PHASE_FERTIG;
            gui_invalidate();
            return;
        }
    } else if (!finish_http(st)) {
        st->phase = PHASE_FERTIG;
        gui_invalidate();
        return;
    }

    after_document(win);
}

static void browser_navigate(struct window *win, const char *url, bool remember)
{
    struct br_state *st = win->user;
    char full[BR_URL_MAX + 1];

    if (strncasecmp(url, "javascript:", 11) == 0) {
        if (st->js)
            js_run(st->js, url + 11, strlen(url + 11));
        rebuild_page(win);
        return;
    }
    if (strncasecmp(url, "mailto:", 7) == 0) {
        ksnprintf(st->status, sizeof(st->status),
                  "Nachrichten kann RetroOS nicht verschicken: %s", url + 7);
        gui_invalidate();
        return;
    }

    /* Ohne Vorsatz ist http:// gemeint. */
    if (strncasecmp(url, "http://", 7) != 0 &&
        strncasecmp(url, "https://", 8) != 0 &&
        strncasecmp(url, "datei:", 6) != 0 &&
        strncasecmp(url, "start:", 6) != 0)
        ksnprintf(full, sizeof(full), "http://%s", url);
    else
        strlcpy(full, url, sizeof(full));

    if (remember && st->url[0] && st->history_len < BR_HISTORY)
        strlcpy(st->history[st->history_len++], st->url, BR_URL_MAX + 1);

    strlcpy(st->url, full, sizeof(st->url));
    strlcpy(st->address, full, sizeof(st->address));
    st->address_cursor = (int32_t)strlen(st->address);
    st->address_focus = false;

    bool from_net = strncasecmp(full, "http://", 7) == 0 ||
                    strncasecmp(full, "https://", 8) == 0;

    if (!from_net) {
        do_load(win, full);
        return;
    }

    if (!net_ready()) {
        strlcpy(st->status, "Keine Netzwerkverbindung.", sizeof(st->status));
        st->phase = PHASE_FERTIG;
        gui_invalidate();
        return;
    }

    if (!st->worker)
        st->worker = thread_create("browser", browser_worker, st, PRIO_NORMAL);

    if (!st->worker) {
        strlcpy(st->status, "Kein freier Arbeits-Thread.", sizeof(st->status));
        gui_invalidate();
        return;
    }

    strlcpy(st->job.url, full, sizeof(st->job.url));
    st->job.resource = -1;
    st->job.state = JOB_REQUESTED;
    wake_one(&st->job);

    strlcpy(st->status, "Lade ...", sizeof(st->status));
    st->phase = PHASE_SEITE;
    gui_invalidate();
}

/* ------------------------------------------------------------------ */
/* Aufteilung des Fensters                                             */
/* ------------------------------------------------------------------ */

static struct rect button_rect(int index)
{
    return rect_make(4 + index * 32, 4, 28, BR_TOOLBAR_H - 8);
}

static struct rect address_rect(struct window *win)
{
    int32_t left = 4 + 3 * 32 + 6;

    return rect_make(left, 5, gui_client_width(win) - left - 56,
                     BR_TOOLBAR_H - 10);
}

static struct rect go_rect(struct window *win)
{
    return rect_make(gui_client_width(win) - 48, 5, 44, BR_TOOLBAR_H - 10);
}

static struct rect page_rect(struct window *win)
{
    return rect_make(0, BR_TOOLBAR_H,
                     gui_client_width(win) - SCROLLBAR_WIDTH,
                     gui_client_height(win) - BR_TOOLBAR_H - BR_STATUS_H);
}

/* ------------------------------------------------------------------ */
/* Zeichnen der Seite                                                  */
/* ------------------------------------------------------------------ */

/* Sucht vom Knoten aufwaerts einen Verweis. */
static struct node *enclosing_link(struct node *node)
{
    for (struct node *n = node; n; n = n->parent)
        if (n->kind == NODE_ELEMENT && n->name &&
            strcmp(n->name, "a") == 0 && node_attribute(n, "href"))
            return n;
    return NULL;
}

static void paint_border(struct canvas *c, const struct fragment *f)
{
    struct rect r = f->rect;

    if (f->border[0] > 0)
        gfx_fill(c, rect_make(r.x, r.y, r.w, f->border[0]),
                 f->border_color[0]);
    if (f->border[2] > 0)
        gfx_fill(c, rect_make(r.x, r.y + r.h - f->border[2], r.w,
                              f->border[2]), f->border_color[2]);
    if (f->border[3] > 0)
        gfx_fill(c, rect_make(r.x, r.y, f->border[3], r.h),
                 f->border_color[3]);
    if (f->border[1] > 0)
        gfx_fill(c, rect_make(r.x + r.w - f->border[1], r.y, f->border[1],
                              r.h), f->border_color[1]);
}

static void paint_fragment(struct canvas *c, struct br_state *st,
                           const struct fragment *f, int32_t ox, int32_t oy)
{
    struct rect r = f->rect;

    r.x += ox;
    r.y += oy;

    switch (f->kind) {
    case FRAG_BOX:
        if (f->has_background && r.w > 0 && r.h > 0)
            gfx_fill(c, r, f->background);
        if (f->border[0] || f->border[1] || f->border[2] || f->border[3]) {
            struct fragment shifted = *f;

            shifted.rect = r;
            paint_border(c, &shifted);
        }
        break;

    case FRAG_TEXT: {
        if (!f->text)
            break;

        uint32_t color = f->color;

        gfx_text_sized(c, r.x, r.y, f->text, color, f->font_size, f->bold,
                       f->italic, f->tracking);
        if (f->underline)
            gfx_hline(c, r.x, r.y + f->font_size, r.w, color);
        if (f->strike)
            gfx_hline(c, r.x, r.y + f->font_size / 2, r.w, color);
        break;
    }

    case FRAG_IMAGE:
        if (f->image) {
            struct image scaled;

            if (f->image->w == r.w && f->image->h == r.h) {
                image_draw(c, r.x, r.y, f->image);
            } else if (image_scale(f->image, r.w, r.h, &scaled)) {
                image_draw(c, r.x, r.y, &scaled);
                image_free(&scaled);
            }
        } else {
            gfx_frame(c, r, COL_PLACE);
            gfx_text_clipped(c, r.x + 4, r.y + MAX(r.h / 2 - 8, 2),
                             f->text ? f->text : "Bild", COL_PLACE,
                             MAX(r.w - 8, 8));
        }
        break;

    case FRAG_BULLET:
        gfx_fill(c, r, f->color);
        break;

    case FRAG_RULE:
        gfx_fill(c, rect_make(r.x, r.y, r.w, MAX(r.h, 1)), f->color);
        break;

    case FRAG_FIELD: {
        gfx_fill(c, r, RGB(0xFF, 0xFF, 0xFF));
        gfx_bevel_thin(c, r, false);

        bool active = st->focused && st->focused == f->node;
        const char *text = f->node && f->node->value ? f->node->value
                                                     : f->text;

        if (text)
            gfx_text_clipped(c, r.x + 4, r.y + (r.h - FONT_HEIGHT) / 2, text,
                             f->node && f->node->value ? COL_TEXT : COL_PLACE,
                             r.w - 8);
        if (active) {
            int32_t caret = r.x + 4 +
                            (f->node->value
                             ? gfx_text_width(f->node->value) : 0);

            if (st->caret_on && caret < r.x + r.w - 2)
                gfx_vline(c, caret, r.y + 3, r.h - 6, COL_TEXT);
        }
        break;
    }

    case FRAG_BUTTON:
        widget_button(c, r, f->text ? f->text : "", false, true);
        break;

    case FRAG_CHECKBOX:
        gfx_fill(c, r, RGB(0xFF, 0xFF, 0xFF));
        gfx_bevel_thin(c, r, false);
        if (f->node && f->node->checked) {
            gfx_line(c, r.x + 3, r.y + r.h / 2, r.x + r.w / 2,
                     r.y + r.h - 4, COL_TEXT);
            gfx_line(c, r.x + r.w / 2, r.y + r.h - 4, r.x + r.w - 3,
                     r.y + 3, COL_TEXT);
        }
        break;
    }
}

static void paint_page(struct window *win, struct canvas *c)
{
    struct br_state *st = win->user;
    struct rect area = page_rect(win);
    uint32_t background = COL_PAGE_BG;

    if (st->doc.body && st->doc.body->style.has_background)
        background = st->doc.body->style.background;
    else if (st->doc.html && st->doc.html->style.has_background)
        background = st->doc.html->style.background;

    gfx_fill(c, area, background);
    gfx_bevel_thin(c, area, false);

    struct canvas page = *c;

    gfx_set_clip(&page, rect_intersect(c->clip,
                                       rect_make(area.x + 2, area.y + 2,
                                                 area.w - 4, area.h - 4)));

    if (st->phase == PHASE_SEITE || st->phase == PHASE_BESTANDTEILE) {
        gfx_text_sized(&page, area.x + BR_MARGIN, area.y + BR_MARGIN,
                       st->phase == PHASE_SEITE ? "Lade ..."
                                                : "Lade Bestandteile ...",
                       COL_TEXT_DIM, 24, true, false, 0);
        if (st->layout.count == 0)
            return;
    } else if (st->layout.count == 0) {
        gfx_text(&page, area.x + BR_MARGIN, area.y + BR_MARGIN,
                 "Keine Seite geladen.", COL_TEXT_DIM);
        return;
    }

    int32_t ox = area.x + BR_MARGIN;
    int32_t oy = area.y + BR_MARGIN - st->scroll;
    int32_t top = area.y - 64;
    int32_t bottom = area.y + area.h + 64;

    for (size_t i = 0; i < st->layout.count; i++) {
        const struct fragment *f = &st->layout.items[i];
        int32_t y = f->rect.y + oy;

        if (y + f->rect.h < top || y > bottom)
            continue;
        paint_fragment(&page, st, f, ox, oy);
    }
}

static void br_paint(struct window *win, struct canvas *c)
{
    struct br_state *st = win->user;
    struct canvas local = gui_client_canvas(win, c);
    struct rect area = page_rect(win);

    gfx_fill(&local, rect_make(0, 0, local.w, local.h), COL_FACE);

    /* Werkzeugleiste */
    widget_toolbar(&local, rect_make(0, 0, local.w, BR_TOOLBAR_H));
    widget_icon_button(&local, button_rect(BR_BACK), ICON_BACK, NULL,
                       st->pressed == BR_BACK, st->history_len > 0);
    widget_icon_button(&local, button_rect(BR_RELOAD), ICON_RELOAD, NULL,
                       st->pressed == BR_RELOAD, true);
    widget_icon_button(&local, button_rect(BR_HOME), ICON_HOME, NULL,
                       st->pressed == BR_HOME, true);

    struct rect address = address_rect(win);

    if (st->address_focus && st->address_selected) {
        widget_field(&local, address, "", -1, true);
        gfx_fill(&local, rect_make(address.x + 3, address.y + 3,
                                   MIN(gfx_text_width(st->address),
                                       address.w - 6), address.h - 6),
                 COL_SELECT);
        gfx_text(&local, address.x + 3,
                 address.y + (address.h - FONT_HEIGHT) / 2, st->address,
                 COL_SELECT_TEXT);
    } else {
        widget_field(&local, address, st->address,
                     st->address_focus && st->caret_on ? st->address_cursor
                                                       : -1,
                     st->address_focus);
    }
    widget_button(&local, go_rect(win), "Los", st->pressed == BR_GO, true);

    paint_page(win, &local);

    int32_t visible = area.h;

    widget_vscroll(&local, rect_make(area.x + area.w, area.y,
                                     SCROLLBAR_WIDTH, area.h),
                   st->scroll / BR_LINE,
                   MAX(st->layout.height + 2 * BR_MARGIN, visible) / BR_LINE,
                   visible / BR_LINE);

    /* Statuszeile */
    char right[64];

    ksnprintf(right, sizeof(right), "%u Stuecke",
              (unsigned)st->layout.count);
    widget_statusbar(&local, rect_make(0, local.h - BR_STATUS_H,
                                       local.w, BR_STATUS_H),
                     st->status, right);
}

/* ------------------------------------------------------------------ */
/* Ereignisse                                                          */
/* ------------------------------------------------------------------ */

static int32_t max_scroll(struct window *win)
{
    struct br_state *st = win->user;

    return MAX(st->layout.height + 2 * BR_MARGIN - page_rect(win).h, 0);
}

static void br_action(struct window *win, int action)
{
    struct br_state *st = win->user;

    switch (action) {
    case BR_BACK:
        if (st->history_len > 0) {
            char previous[BR_URL_MAX + 1];

            strlcpy(previous, st->history[--st->history_len],
                    sizeof(previous));
            browser_navigate(win, previous, false);
        }
        break;
    case BR_RELOAD:
        browser_navigate(win, st->url, false);
        break;
    case BR_HOME:
        browser_navigate(win, "start:", true);
        break;
    case BR_GO:
        browser_navigate(win, st->address, true);
        break;
    }
}

/* Legt den Text der angezeigten Seite in die Zwischenablage. Ohne
 * Auswahl im Dokument ist das die naheliegende Bedeutung von Strg+C:
 * die ganze Seite. */
static void copy_page_text(struct window *win)
{
    struct br_state *st = win->user;
    size_t room = 64 * 1024;
    char *buffer = kmalloc(room);

    if (!buffer)
        return;

    buffer[0] = '\0';
    if (st->doc.root)
        dom_text_content(st->doc.root, buffer, room);

    clipboard_set(buffer, strlen(buffer));
    kfree(buffer);

    ksnprintf(st->status, sizeof(st->status), "Seitentext kopiert (%u Zeichen)",
              (unsigned)strlen(clipboard_get(NULL) ? clipboard_get(NULL) : ""));
    gui_invalidate();
}

static void br_address_key(struct window *win, const struct gui_event *ev)
{
    struct br_state *st = win->user;
    size_t length = strlen(st->address);

    if (ev->mods & MOD_CTRL) {
        char c = (ev->ascii >= 'A' && ev->ascii <= 'Z')
                 ? (char)(ev->ascii + 32) : ev->ascii;

        /* Strg+A markiert alles - das naechste Zeichen ersetzt sie. */
        if (c == 'a') {
            st->address_selected = true;
            gui_invalidate();
            return;
        }
        if (c == 'c') {
            clipboard_set(st->address, length);
            return;
        }
        if (c == 'v') {
            size_t bytes = 0;
            const char *text = clipboard_get(&bytes);

            if (st->address_selected) {
                st->address[0] = '\0';
                st->address_cursor = 0;
                st->address_selected = false;
                length = 0;
            }
            for (size_t i = 0; text && i < bytes &&
                               length + 1 < sizeof(st->address); i++) {
                if (text[i] < 32 || (unsigned char)text[i] == 127)
                    continue;
                memmove(&st->address[st->address_cursor + 1],
                        &st->address[st->address_cursor],
                        length - st->address_cursor + 1);
                st->address[st->address_cursor++] = text[i];
                length++;
            }
            gui_invalidate();
            return;
        }
        return;
    }

    if (st->address_selected && ev->ascii >= 32 &&
        (unsigned char)ev->ascii < 127) {
        st->address[0] = '\0';
        st->address_cursor = 0;
        st->address_selected = false;
        length = 0;
    } else if (ev->ascii || ev->key) {
        st->address_selected = false;
    }

    if (ev->key == KEY_ESCAPE) {
        st->address_focus = false;
        strlcpy(st->address, st->url, sizeof(st->address));
        gui_invalidate();
        return;
    }
    if (ev->key == KEY_ENTER) {
        st->address_focus = false;
        browser_navigate(win, st->address, true);
        return;
    }
    if (ev->key == KEY_BACKSPACE) {
        if (st->address_cursor > 0) {
            memmove(st->address + st->address_cursor - 1,
                    st->address + st->address_cursor,
                    length - st->address_cursor + 1);
            st->address_cursor--;
        }
        gui_invalidate();
        return;
    }
    if (ev->key == KEY_DELETE) {
        if ((size_t)st->address_cursor < length)
            memmove(st->address + st->address_cursor,
                    st->address + st->address_cursor + 1,
                    length - st->address_cursor);
        gui_invalidate();
        return;
    }
    if (ev->key == KEY_LEFT) {
        st->address_cursor = MAX(st->address_cursor - 1, 0);
        gui_invalidate();
        return;
    }
    if (ev->key == KEY_RIGHT) {
        st->address_cursor = MIN(st->address_cursor + 1, (int32_t)length);
        gui_invalidate();
        return;
    }
    if (ev->key == KEY_HOME) {
        st->address_cursor = 0;
        gui_invalidate();
        return;
    }
    if (ev->key == KEY_END) {
        st->address_cursor = (int32_t)length;
        gui_invalidate();
        return;
    }

    if (ev->ascii >= 32 && (unsigned char)ev->ascii != 127 &&
        length + 1 < BR_URL_MAX) {
        memmove(st->address + st->address_cursor + 1,
                st->address + st->address_cursor,
                length - st->address_cursor + 1);
        st->address[st->address_cursor++] = ev->ascii;
        gui_invalidate();
    }
}

/* Text in ein Eingabefeld tippen. */
static void field_key(struct window *win, const struct gui_event *ev)
{
    struct br_state *st = win->user;
    struct node *node = st->focused;

    if (!node)
        return;

    size_t length = node->value ? strlen(node->value) : 0;

    if (ev->key == KEY_ESCAPE) {
        st->focused = NULL;
        gui_invalidate();
        return;
    }
    if (ev->key == KEY_BACKSPACE) {
        if (length > 0)
            node->value[length - 1] = '\0';
    } else if (ev->key == KEY_ENTER) {
        if (st->js)
            js_dispatch_event(st->js, node, "change");
        st->focused = NULL;
    } else if (ev->ascii >= 32 && (unsigned char)ev->ascii != 127) {
        char *bigger = kmalloc(length + 2);

        if (bigger) {
            if (node->value)
                memcpy(bigger, node->value, length);
            bigger[length] = ev->ascii;
            bigger[length + 1] = '\0';
            kfree(node->value);
            node->value = bigger;
        }
    } else {
        return;
    }

    if (st->js)
        js_dispatch_event(st->js, node, "input");
    st->needs_layout = true;
    gui_invalidate();
}

/* Wandelt Fensterkoordinaten in Dokumentkoordinaten. */
static bool to_document(struct window *win, int32_t x, int32_t y,
                        int32_t *dx, int32_t *dy)
{
    struct br_state *st = win->user;
    struct rect area = page_rect(win);

    if (!rect_contains(area, x, y))
        return false;
    *dx = x - (area.x + BR_MARGIN);
    *dy = y - (area.y + BR_MARGIN) + st->scroll;
    return true;
}

static void click_node(struct window *win, struct node *node)
{
    struct br_state *st = win->user;

    if (!node)
        return;

    /* Erst die Behandlung durch das Skript - sie kann die Seite umbauen. */
    bool handled = false;

    if (st->js) {
        for (struct node *n = node; n; n = n->parent) {
            if (js_dispatch_event(st->js, n, "click"))
                handled = true;
            if (n->kind == NODE_ELEMENT && n->name &&
                strcmp(n->name, "body") == 0)
                break;
        }
    }

    /* Ankreuzfelder und Eingabefelder. */
    if (node->kind == NODE_ELEMENT && node->name) {
        const char *type = node_attribute(node, "type");

        if (strcmp(node->name, "input") == 0 && type &&
            (strcasecmp(type, "checkbox") == 0 ||
             strcasecmp(type, "radio") == 0)) {
            node->checked = !node->checked;
            if (st->js)
                js_dispatch_event(st->js, node, "change");
            st->needs_layout = true;
        } else if (strcmp(node->name, "input") == 0 ||
                   strcmp(node->name, "textarea") == 0) {
            st->focused = node;
        }
    }

    if (st->has_pending) {
        char target[BR_URL_MAX + 1];

        st->has_pending = false;
        resolve_url(st->url, st->pending_url, target, sizeof(target));
        browser_navigate(win, target, true);
        return;
    }

    if (handled) {
        /* Das Skript hat entschieden. Nur neu aufbauen. */
        rebuild_page(win);
        return;
    }

    struct node *link = enclosing_link(node);

    if (link) {
        const char *href = node_attribute(link, "href");
        char target[BR_URL_MAX + 1];

        resolve_url(st->url, href, target, sizeof(target));
        browser_navigate(win, target, true);
        return;
    }

    if (st->needs_layout || st->needs_restyle) {
        relayout(win);
        gui_invalidate();
    }
}

static void br_event(struct window *win, const struct gui_event *ev)
{
    struct br_state *st = win->user;

    switch (ev->type) {
    case EV_MOUSE_DOWN: {
        if (ev->y < BR_TOOLBAR_H) {
            for (int i = 0; i < BR_HOME + 1; i++) {
                if (rect_contains(button_rect(i), ev->x, ev->y)) {
                    st->pressed = i;
                    gui_invalidate();
                    return;
                }
            }
            if (rect_contains(go_rect(win), ev->x, ev->y)) {
                st->pressed = BR_GO;
                gui_invalidate();
                return;
            }

            bool hit = rect_contains(address_rect(win), ev->x, ev->y);

            if (hit && !st->address_focus)
                st->address_selected = true;   /* wie im echten Browser */
            st->address_focus = hit;
            gui_invalidate();
            return;
        }

        struct rect scroll = rect_make(page_rect(win).x + page_rect(win).w,
                                       page_rect(win).y, SCROLLBAR_WIDTH,
                                       page_rect(win).h);

        if (rect_contains(scroll, ev->x, ev->y)) {
            int32_t step = page_rect(win).h / 2;

            if (ev->y < scroll.y + SCROLLBAR_WIDTH)
                st->scroll = MAX(st->scroll - BR_LINE * 3, 0);
            else if (ev->y >= scroll.y + scroll.h - SCROLLBAR_WIDTH)
                st->scroll = MIN(st->scroll + BR_LINE * 3, max_scroll(win));
            else
                st->scroll = CLAMP(((ev->y - scroll.y) *
                                    (st->layout.height + 2 * BR_MARGIN)) /
                                   MAX(scroll.h, 1) - step, 0,
                                   max_scroll(win));
            gui_invalidate();
            return;
        }

        st->address_focus = false;
        st->focused = NULL;

        int32_t dx, dy;

        if (to_document(win, ev->x, ev->y, &dx, &dy))
            click_node(win, layout_hit(&st->layout, dx, dy));
        gui_invalidate();
        break;
    }

    case EV_MOUSE_UP: {
        int pressed = st->pressed;

        st->pressed = -1;
        if (pressed == BR_GO && rect_contains(go_rect(win), ev->x, ev->y))
            br_action(win, BR_GO);
        else if (pressed >= 0 && pressed <= BR_HOME &&
                 rect_contains(button_rect(pressed), ev->x, ev->y))
            br_action(win, pressed);
        gui_invalidate();
        break;
    }

    case EV_SCROLL:
        st->scroll = CLAMP(st->scroll - ev->scroll * BR_LINE * 3, 0,
                           max_scroll(win));
        gui_invalidate();
        break;

    case EV_KEY_DOWN:
        if (st->address_focus) {
            br_address_key(win, ev);
            return;
        }
        if (st->focused) {
            field_key(win, ev);
            return;
        }
        if ((ev->mods & MOD_CTRL) &&
            (ev->ascii == 'c' || ev->ascii == 'C')) {
            copy_page_text(win);
            return;
        }
        switch (ev->key) {
        case KEY_DOWN:
            st->scroll = MIN(st->scroll + BR_LINE, max_scroll(win));
            break;
        case KEY_UP:
            st->scroll = MAX(st->scroll - BR_LINE, 0);
            break;
        case KEY_PAGEDOWN:
            st->scroll = MIN(st->scroll + page_rect(win).h - BR_LINE,
                             max_scroll(win));
            break;
        case KEY_PAGEUP:
            st->scroll = MAX(st->scroll - page_rect(win).h + BR_LINE, 0);
            break;
        case KEY_HOME:
            st->scroll = 0;
            break;
        case KEY_END:
            st->scroll = max_scroll(win);
            break;
        case KEY_F5:
            br_action(win, BR_RELOAD);
            return;
        case KEY_BACKSPACE:
            br_action(win, BR_BACK);
            return;
        default:
            return;
        }
        gui_invalidate();
        break;

    case EV_TICK:
        st->caret_on = !st->caret_on;

        /* Ist der Arbeits-Thread fertig, wird jetzt ausgewertet. */
        if (st->job.state == JOB_DONE) {
            int32_t index = st->job.resource;

            st->job.state = JOB_IDLE;

            if (index < 0) {
                do_load(win, st->job.url);
            } else if ((size_t)index < st->resource_count) {
                finish_resource(st, &st->resources[index]);
                if (!post_next_resource(win))
                    document_ready(win);
                gui_invalidate();
            }
            return;
        }

        /* Zeitgeber der Skripte bedienen. */
        if (st->js && st->phase == PHASE_FERTIG) {
            if (js_run_timers(st->js, timer_ms())) {
                if (st->has_pending) {
                    char target[BR_URL_MAX + 1];

                    st->has_pending = false;
                    resolve_url(st->url, st->pending_url, target,
                                sizeof(target));
                    browser_navigate(win, target, true);
                    return;
                }
                if (st->needs_layout || st->needs_restyle) {
                    relayout(win);
                    gui_invalidate();
                }
            }
        }

        if (st->phase == PHASE_SEITE || st->phase == PHASE_BESTANDTEILE)
            gui_invalidate();
        if (st->address_focus || st->focused)
            gui_invalidate();
        break;

    case EV_RESIZED: {
        struct rect area = page_rect(win);
        int32_t width = MAX(area.w - 2 * BR_MARGIN - 4, 80);

        if (width != st->layout_width && st->phase == PHASE_FERTIG)
            relayout(win);
        st->scroll = MIN(st->scroll, max_scroll(win));
        gui_invalidate();
        break;
    }

    default:
        break;
    }
}

static void br_close(struct window *win)
{
    struct br_state *st = win->user;

    if (!st)
        return;

    /* Der Arbeits-Thread benutzt st - erst wenn er beendet ist, darf der
     * Speicher weg. */
    if (st->worker) {
        st->worker_quit = true;
        wake_one(&st->job);

        for (int i = 0; i < 200 && !st->worker_done; i++)
            thread_sleep(10);
    }

    if (st->job.state == JOB_DONE && st->job.ok)
        http_response_free(&st->job.response);

    layout_free(&st->layout);
    css_free(st->sheet);
    js_destroy(st->js);
    release_resources(st);
    document_free(&st->doc);
    kfree(st);
    win->user = NULL;
}

/* Oeffnet eine Adresse - in einem vorhandenen Fenster, wenn eines offen
 * ist. */
void browser_open(const char *url)
{
    struct window *win = gui_find_by_paint(br_paint);

    if (win) {
        gui_focus_window(win);
        browser_navigate(win, url, true);
        return;
    }

    app_browser();

    win = gui_find_by_paint(br_paint);
    if (win)
        browser_navigate(win, url, true);
}

void app_browser(void)
{
    struct br_state *st = kzalloc(sizeof(*st));

    if (!st)
        return;

    static int32_t cascade;
    int32_t offset = (cascade++ % 4) * 26;

    struct window *win = gui_create_window("Browser", 120 + offset,
                                           50 + offset, 820, 600,
                                           WF_RESIZABLE, ICON_BROWSER);

    if (!win) {
        kfree(st);
        return;
    }

    st->pressed = -1;
    st->caret_on = true;
    st->job.resource = -1;
    document_init(&st->doc);

    win->user     = st;
    win->on_paint = br_paint;
    win->on_event = br_event;
    win->on_close = br_close;
    win->min_w    = 460;
    win->min_h    = 300;

    gui_focus_window(win);
    browser_navigate(win, "start:", false);
}
