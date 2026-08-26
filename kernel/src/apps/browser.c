/* browser.c - der Webbrowser von RetroOS.
 *
 * Er holt Seiten per HTTP, laesst sie von html.c zerlegen und setzt daraus
 * ein Bild: Ueberschriften groesser, Verweise blau und unterstrichen,
 * Aufzaehlungen mit Punkt. Der Umbruch geschieht beim Zeichnen, also passt
 * sich die Seite sofort an die Fenstergroesse an.
 *
 * Drei Adressarten werden verstanden:
 *   http://...   eine Seite aus dem Netz
 *   datei:/...   eine Datei aus dem Dateisystem
 *   start:       die eingebaute Startseite
 *
 * Geladen wird, waehrend die Oberflaeche steht. Das ist der Preis dafuer,
 * dass RetroOS ohne Nebenlaeufigkeit auskommt; der Zustand "Lade ..." wird
 * deshalb vor dem Beginn gezeichnet.
 */

#include "apps.h"
#include "font.h"
#include "html.h"
#include "kstring.h"
#include "mm.h"
#include "net.h"
#include "thread.h"
#include "theme.h"
#include "widgets.h"

#define BR_TOOLBAR_H 34
#define BR_STATUS_H  24
#define BR_MARGIN    12
#define BR_LINE      18
#define BR_MAX_LINKS 256
#define BR_URL_MAX   255
#define BR_HISTORY   16

#define COL_PAGE_BG   RGB(0xFC, 0xFC, 0xF8)
#define COL_LINK      RGB(0x18, 0x40, 0xC8)
#define COL_HEADING   RGB(0x10, 0x28, 0x60)
#define COL_PLACE     RGB(0x80, 0x80, 0x80)

enum br_button { BR_BACK, BR_RELOAD, BR_HOME, BR_GO, BR_COUNT };

struct link_area {
    struct rect rect;
    const char *href;
};

/* Der Ladeauftrag wandert zwischen Oberflaeche und Arbeits-Thread hin und
 * her. Nur ein Feld wechselt dabei die Richtung, deshalb genuegt ein
 * Zustandswert ohne weitere Absprache. */
enum job_state {
    JOB_IDLE,
    JOB_REQUESTED,
    JOB_RUNNING,
    JOB_DONE,
};

struct load_job {
    char     url[BR_URL_MAX + 1];
    volatile int state;

    struct http_response response;
    bool     ok;
};

struct br_state {
    struct html_doc doc;

    struct load_job  job;
    struct thread   *worker;
    volatile bool    worker_quit;
    volatile bool    worker_done;

    char url[BR_URL_MAX + 1];
    char address[BR_URL_MAX + 1];
    int32_t address_cursor;
    bool    address_focus;
    bool    address_selected;   /* nach dem Anklicken ersetzt Tippen alles */
    bool    caret_on;

    char history[BR_HISTORY][BR_URL_MAX + 1];
    int  history_len;

    bool loading;

    char status[160];

    int32_t scroll;
    int32_t content_height;

    struct link_area links[BR_MAX_LINKS];
    size_t           link_count;

    int pressed;
};

static void browser_navigate(struct window *win, const char *url, bool remember);

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

    if (strncasecmp(href, "http://", 7) == 0 ||
        strncasecmp(href, "https://", 8) == 0 ||
        strncasecmp(href, "datei:", 6) == 0 ||
        strncasecmp(href, "start:", 6) == 0) {
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

    /* Ein Verweis ohne Schema bleibt beim Schema der aktuellen Seite. */
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
/* Seiten laden                                                        */
/* ------------------------------------------------------------------ */

static const char start_page[] =
    "<html><head><title>RetroOS</title></head><body>"
    "<h1>RetroOS-Browser</h1>"
    "<p>Ein einfacher Browser fuer Textseiten. Gib oben eine Adresse ein "
    "oder folge einem der Verweise.</p>"
    "<h2>Aus dem Dateisystem</h2>"
    "<ul>"
    "<li><a href=\"datei:/Dokumente/beispiel.html\">Beispielseite</a></li>"
    "<li><a href=\"datei:/Dokumente/willkommen.txt\">Willkommenstext</a></li>"
    "<li><a href=\"datei:/System/version.txt\">Systemversion</a></li>"
    "</ul>"
    "<h2>Hinweise</h2>"
    "<ul>"
    "<li>Adressen ohne Vorsatz werden als <b>http://</b> gelesen.</li>"
    "<li>HTTPS wird unterstuetzt: TLS 1.3 mit X25519 und AES-GCM oder "
    "ChaCha20-Poly1305, mit Pruefung der Zertifikatskette.</li>"
    "<li>Bilder werden als Platzhalter angezeigt.</li>"
    "</ul>"
    "</body></html>";

static void load_start_page(struct br_state *st)
{
    html_free(&st->doc);
    html_parse(&st->doc, start_page, sizeof(start_page) - 1);
    strlcpy(st->status, "Startseite", sizeof(st->status));
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

    html_free(&st->doc);

    const char *dot = strrchr(node->name, '.');
    bool is_html = dot && (strcasecmp(dot, ".html") == 0 ||
                           strcasecmp(dot, ".htm") == 0);

    if (is_html)
        html_parse(&st->doc, (const char *)node->data, node->size);
    else
        html_parse_plain(&st->doc, (const char *)node->data, node->size);

    ksnprintf(st->status, sizeof(st->status), "%s - %u Byte", node->name,
              (unsigned)node->size);
    return true;
}

/* Wertet das Ergebnis des Arbeits-Threads aus. */
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

    html_free(&st->doc);

    if (strncasecmp(response->content_type, "text/plain", 10) == 0)
        html_parse_plain(&st->doc, response->body, response->body_length);
    else
        html_parse(&st->doc, response->body, response->body_length);

    if (response->security[0])
        ksnprintf(st->status, sizeof(st->status), "%d - %u Byte - %s - %s",
                  response->status, (unsigned)response->body_length,
                  response->content_type, response->security);
    else
        ksnprintf(st->status, sizeof(st->status),
                  "%d - %u Byte - %s - unverschluesselt",
                  response->status, (unsigned)response->body_length,
                  response->content_type);

    http_response_free(response);
    return true;
}

/* Laeuft im Arbeits-Thread: holt die Seite, waehrend die Oberflaeche
 * weiterlaeuft. Ausgewertet wird das Ergebnis wieder im Fenster-Thread. */
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

static void do_load(struct window *win, const char *url)
{
    struct br_state *st = win->user;
    char title[WIN_TITLE_MAX + 1];

    st->scroll = 0;
    st->link_count = 0;

    if (strncasecmp(url, "start:", 6) == 0)
        load_start_page(st);
    else if (strncasecmp(url, "datei:", 6) == 0)
        load_local(st, url + 6);
    else
        finish_http(st);

    ksnprintf(title, sizeof(title), "Browser - %s",
              st->doc.title[0] ? st->doc.title : url);
    gui_set_title(win, title);
    gui_invalidate();
}

static void browser_navigate(struct window *win, const char *url, bool remember)
{
    struct br_state *st = win->user;
    char full[BR_URL_MAX + 1];

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
        /* Aus dem Dateisystem geht es sofort. */
        do_load(win, full);
        return;
    }

    if (!net_ready()) {
        strlcpy(st->status, "Keine Netzwerkverbindung.", sizeof(st->status));
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

    /* Auftrag abgeben und weiterzeichnen - der Rest geschieht nebenher. */
    strlcpy(st->job.url, full, sizeof(st->job.url));
    st->job.state = JOB_REQUESTED;
    wake_one(&st->job);

    strlcpy(st->status, "Lade ...", sizeof(st->status));
    st->loading = true;
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

    return rect_make(left, 5, gui_client_width(win) - left - 56, BR_TOOLBAR_H - 10);
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
/* Seitenaufbau                                                        */
/* ------------------------------------------------------------------ */

struct layout {
    struct canvas *canvas;      /* NULL = nur messen */
    struct br_state *state;
    struct rect area;
    int32_t x, y;
    int32_t line_height;
    bool    line_started;
};

static int32_t heading_scale(uint8_t level)
{
    if (level == 1) return 3;
    if (level == 2) return 2;
    return 1;
}

static void newline(struct layout *l)
{
    l->x = l->area.x + BR_MARGIN;
    l->y += l->line_height;
    l->line_height = BR_LINE;
    l->line_started = false;
}

/* Zeichnet ein Wort und rueckt weiter; bricht am Rand um. */
static void emit_word(struct layout *l, const char *word, size_t length,
                      bool bold, uint8_t heading, bool link, const char *href)
{
    char buffer[256];

    if (length == 0 || length >= sizeof(buffer))
        return;

    memcpy(buffer, word, length);
    buffer[length] = '\0';

    int32_t scale = heading ? heading_scale(heading) : 1;
    int32_t width = gfx_text_width_scaled(buffer, scale);
    int32_t height = FONT_HEIGHT * scale + 2;
    int32_t limit = l->area.x + l->area.w - BR_MARGIN;

    if (l->line_started && l->x + width > limit)
        newline(l);

    l->line_height = MAX(l->line_height, height);

    if (l->canvas) {
        uint32_t color = link ? COL_LINK : (heading ? COL_HEADING : COL_TEXT);
        bool     heavy = bold || heading > 0;

        /* Nur zeichnen, was ins Fenster faellt. */
        if (l->y + height >= l->area.y && l->y <= l->area.y + l->area.h) {
            gfx_text_scaled(l->canvas, l->x, l->y, buffer, color, scale, heavy);

            if (link) {
                gfx_hline(l->canvas, l->x, l->y + FONT_HEIGHT * scale,
                          width, COL_LINK);

                if (l->state->link_count < BR_MAX_LINKS) {
                    struct link_area *area =
                        &l->state->links[l->state->link_count++];

                    /* Die Luecke zum naechsten Wort gehoert mit dazu -
                     * sonst trifft man zwischen zwei Woertern ins Leere. */
                    area->rect = rect_make(l->x, l->y,
                                           width + FONT_WIDTH * scale, height);
                    area->href = href;
                }
            }
        }
    }

    l->x += width + FONT_WIDTH * scale;   /* ein Leerzeichen breit */
    l->line_started = true;
}

static void emit_text(struct layout *l, const struct html_item *item)
{
    const char *p = item->text;

    if (!p)
        return;

    if (item->pre) {
        /* Vorformatierter Text wird nicht umgebrochen. */
        emit_word(l, p, strlen(p), item->bold, item->heading, false, NULL);
        return;
    }

    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;

        const char *start = p;
        while (*p && *p != ' ')
            p++;

        emit_word(l, start, (size_t)(p - start), item->bold, item->heading,
                  item->type == HTML_LINK, item->href);
    }
}

/* Laeuft einmal durch das Dokument - zum Zeichnen oder nur zum Messen. */
static int32_t run_layout(struct window *win, struct canvas *canvas)
{
    struct br_state *st = win->user;
    struct rect area = page_rect(win);
    struct layout l;

    memset(&l, 0, sizeof(l));
    l.canvas = canvas;
    l.state  = st;
    l.area   = area;
    l.x      = area.x + BR_MARGIN;
    l.y      = area.y + BR_MARGIN - st->scroll;
    l.line_height = BR_LINE;

    if (canvas)
        st->link_count = 0;

    for (size_t i = 0; i < st->doc.count; i++) {
        const struct html_item *item = &st->doc.items[i];

        switch (item->type) {
        case HTML_TEXT:
        case HTML_LINK:
            emit_text(&l, item);
            break;

        case HTML_BREAK:
            newline(&l);
            break;

        case HTML_PARAGRAPH:
            newline(&l);
            l.y += 8;
            break;

        case HTML_RULE:
            newline(&l);
            l.y += 6;
            if (canvas)
                gfx_hline(canvas, area.x + BR_MARGIN, l.y,
                          area.w - 2 * BR_MARGIN, COL_SHADOW);
            l.y += 8;
            break;

        case HTML_BULLET:
            newline(&l);
            l.x += 16;
            if (canvas && l.y >= area.y - BR_LINE && l.y <= area.y + area.h)
                gfx_fill(canvas, rect_make(l.x - 10, l.y + 6, 4, 4), COL_TEXT);
            l.line_started = true;
            break;

        case HTML_IMAGE: {
            char label[160];

            ksnprintf(label, sizeof(label), "[%s]", item->text ? item->text : "Bild");
            emit_word(&l, label, strlen(label), false, 0, false, NULL);
            break;
        }
        }
    }

    newline(&l);
    return l.y - (area.y - st->scroll) + BR_MARGIN;
}

/* ------------------------------------------------------------------ */
/* Zeichnen                                                            */
/* ------------------------------------------------------------------ */

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
                     st->address_focus && st->caret_on ? st->address_cursor : -1,
                     st->address_focus);
    }
    widget_button(&local, go_rect(win), "Los", st->pressed == BR_GO, true);

    /* Seite */
    gfx_fill(&local, area, COL_PAGE_BG);
    gfx_bevel_thin(&local, area, false);

    struct canvas page = local;
    gfx_set_clip(&page, rect_intersect(local.clip,
                                       rect_make(area.x + 2, area.y + 2,
                                                 area.w - 4, area.h - 4)));

    if (st->loading) {
        gfx_text_scaled(&page, area.x + BR_MARGIN, area.y + BR_MARGIN,
                        "Lade ...", COL_TEXT_DIM, 2, true);
    } else if (st->doc.count == 0) {
        gfx_text(&page, area.x + BR_MARGIN, area.y + BR_MARGIN,
                 "Keine Seite geladen.", COL_TEXT_DIM);
    } else {
        st->content_height = run_layout(win, &page);
    }

    int32_t visible = area.h;
    widget_vscroll(&local, rect_make(area.x + area.w, area.y,
                                     SCROLLBAR_WIDTH, area.h),
                   st->scroll / BR_LINE,
                   MAX(st->content_height, visible) / BR_LINE,
                   visible / BR_LINE);

    /* Statuszeile */
    char right[48];
    ksnprintf(right, sizeof(right), "%u Bausteine", (unsigned)st->doc.count);
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

    return MAX(st->content_height - page_rect(win).h + BR_MARGIN, 0);
}

static void br_action(struct window *win, int action)
{
    struct br_state *st = win->user;

    switch (action) {
    case BR_BACK:
        if (st->history_len > 0) {
            char previous[BR_URL_MAX + 1];

            strlcpy(previous, st->history[--st->history_len], sizeof(previous));
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

static void br_address_key(struct window *win, const struct gui_event *ev)
{
    struct br_state *st = win->user;
    size_t length = strlen(st->address);

    /* Strg+A markiert alles - das naechste Zeichen ersetzt die Adresse. */
    if ((ev->mods & MOD_CTRL) && (ev->ascii == 'a' || ev->ascii == 'A')) {
        st->address_selected = true;
        gui_invalidate();
        return;
    }

    if (st->address_selected && ev->ascii >= 32 &&
        (unsigned char)ev->ascii != 127) {
        st->address[0] = '\0';
        st->address_cursor = 0;
        st->address_selected = false;
        length = 0;
    } else if (ev->key != KEY_ENTER && ev->key != KEY_ESCAPE) {
        st->address_selected = false;
    }

    switch (ev->key) {
    case KEY_ENTER:
        st->address_focus = false;
        browser_navigate(win, st->address, true);
        return;
    case KEY_ESCAPE:
        st->address_focus = false;
        strlcpy(st->address, st->url, sizeof(st->address));
        break;
    case KEY_LEFT:
        if (st->address_cursor > 0)
            st->address_cursor--;
        break;
    case KEY_RIGHT:
        if (st->address[st->address_cursor])
            st->address_cursor++;
        break;
    case KEY_HOME:
        st->address_cursor = 0;
        break;
    case KEY_END:
        st->address_cursor = (int32_t)length;
        break;
    case KEY_BACKSPACE:
        if (st->address_cursor > 0) {
            memmove(&st->address[st->address_cursor - 1],
                    &st->address[st->address_cursor],
                    length - (size_t)st->address_cursor + 1);
            st->address_cursor--;
        }
        break;
    case KEY_DELETE:
        if (st->address[st->address_cursor])
            memmove(&st->address[st->address_cursor],
                    &st->address[st->address_cursor + 1],
                    length - (size_t)st->address_cursor);
        break;
    default:
        if (ev->ascii >= 32 && (unsigned char)ev->ascii != 127 &&
            length < BR_URL_MAX) {
            memmove(&st->address[st->address_cursor + 1],
                    &st->address[st->address_cursor],
                    length - (size_t)st->address_cursor + 1);
            st->address[st->address_cursor++] = ev->ascii;
        } else {
            return;
        }
        break;
    }
    st->caret_on = true;
    gui_invalidate();
}

static void br_event(struct window *win, const struct gui_event *ev)
{
    struct br_state *st = win->user;

    switch (ev->type) {
    case EV_MOUSE_DOWN:
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
                st->scroll = CLAMP(((ev->y - scroll.y) * st->content_height) /
                                   MAX(scroll.h, 1) - step, 0, max_scroll(win));
            gui_invalidate();
            return;
        }

        /* Verweis getroffen? */
        for (size_t i = 0; i < st->link_count; i++) {
            if (rect_contains(st->links[i].rect, ev->x, ev->y)) {
                char target[BR_URL_MAX + 1];

                resolve_url(st->url, st->links[i].href, target, sizeof(target));
                st->address_focus = false;
                browser_navigate(win, target, true);
                return;
            }
        }
        st->address_focus = false;
        gui_invalidate();
        break;

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
        switch (ev->key) {
        case KEY_DOWN:     st->scroll = MIN(st->scroll + BR_LINE, max_scroll(win)); break;
        case KEY_UP:       st->scroll = MAX(st->scroll - BR_LINE, 0); break;
        case KEY_PAGEDOWN: st->scroll = MIN(st->scroll + page_rect(win).h - BR_LINE,
                                            max_scroll(win)); break;
        case KEY_PAGEUP:   st->scroll = MAX(st->scroll - page_rect(win).h + BR_LINE, 0); break;
        case KEY_HOME:     st->scroll = 0; break;
        case KEY_END:      st->scroll = max_scroll(win); break;
        case KEY_F5:       br_action(win, BR_RELOAD); return;
        case KEY_BACKSPACE: br_action(win, BR_BACK); return;
        default: return;
        }
        gui_invalidate();
        break;

    case EV_TICK:
        st->caret_on = !st->caret_on;

        /* Ist der Arbeits-Thread fertig, wird die Seite jetzt gesetzt. */
        if (st->loading && st->job.state == JOB_DONE) {
            st->loading = false;
            st->job.state = JOB_IDLE;
            do_load(win, st->job.url);
            return;
        }
        if (st->loading)
            gui_invalidate();   /* Anzeige "Lade ..." lebendig halten */
        if (st->address_focus)
            gui_invalidate();
        break;

    case EV_RESIZED:
        st->scroll = MIN(st->scroll, max_scroll(win));
        gui_invalidate();
        break;

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

    html_free(&st->doc);
    kfree(st);
    win->user = NULL;
}

/* Oeffnet eine Adresse - in einem vorhandenen Fenster, wenn eines offen ist. */
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

    struct window *win = gui_create_window("Browser", 120 + offset, 50 + offset,
                                           760, 560, WF_RESIZABLE, ICON_BROWSER);
    if (!win) {
        kfree(st);
        return;
    }

    st->pressed = -1;
    st->caret_on = true;

    win->user     = st;
    win->on_paint = br_paint;
    win->on_event = br_event;
    win->on_close = br_close;
    win->min_w    = 460;
    win->min_h    = 300;

    gui_focus_window(win);
    browser_navigate(win, "start:", false);
}
