/* writedoc.c - Absaetze, Auszeichnungen und der Weg nach HTML.
 *
 * Das Einfuegen und Loeschen einzelner Zeichen ist gewoehnliches
 * Umschichten im Feld. Interessant sind nur die beiden Enden: HTML
 * schreiben und HTML lesen.
 *
 * Geschrieben wird eng an dem, was der Browser dieses Systems ohnehin
 * versteht - Ueberschriften, Absaetze, Listen, ein Zitat, dazu <b> und
 * <u>. Gelesen wird ueber denselben Zerteiler, mit dem der Browser
 * arbeitet: Ein Dokument, das von aussen kommt, soll nicht an einer
 * Kleinigkeit scheitern, die ein richtiger Zerteiler laengst kennt.
 */

#include "writedoc.h"

#include "dom.h"
#include "htmlparse.h"
#include "kstring.h"
#include "lang.h"

void wdoc_clear(struct wdoc *doc)
{
    memset(doc, 0, sizeof(*doc));
    doc->count = 1;
    doc->paras[0].style = STYLE_BODY;
}

int wdoc_insert(struct wdoc *doc, int index, uint8_t style)
{
    if (doc->count >= DOC_PARAS_MAX)
        return index;

    index = CLAMP(index, -1, doc->count - 1);

    for (int i = doc->count; i > index + 1; i--)
        doc->paras[i] = doc->paras[i - 1];

    doc->count++;

    struct paragraph *para = &doc->paras[index + 1];

    memset(para, 0, sizeof(*para));
    para->style = style;
    return index + 1;
}

void wdoc_remove(struct wdoc *doc, int index)
{
    if (index < 0 || index >= doc->count || doc->count <= 1)
        return;

    for (int i = index; i + 1 < doc->count; i++)
        doc->paras[i] = doc->paras[i + 1];
    doc->count--;
}

void wdoc_insert_char(struct wdoc *doc, int para, int at, char ch,
                      uint8_t marks)
{
    if (para < 0 || para >= doc->count)
        return;

    struct paragraph *p = &doc->paras[para];

    if (p->len + 1 >= PARA_TEXT_MAX)
        return;

    at = CLAMP(at, 0, p->len);

    for (int i = p->len; i > at; i--) {
        p->text[i] = p->text[i - 1];
        p->marks[i] = p->marks[i - 1];
    }
    p->text[at] = ch;
    p->marks[at] = marks;
    p->len++;
    p->text[p->len] = '\0';
}

void wdoc_erase_char(struct wdoc *doc, int para, int at)
{
    if (para < 0 || para >= doc->count)
        return;

    struct paragraph *p = &doc->paras[para];

    if (at < 0 || at >= p->len)
        return;

    for (int i = at; i + 1 < p->len; i++) {
        p->text[i] = p->text[i + 1];
        p->marks[i] = p->marks[i + 1];
    }
    p->len--;
    p->text[p->len] = '\0';
}

void wdoc_split(struct wdoc *doc, int para, int at)
{
    if (para < 0 || para >= doc->count || doc->count >= DOC_PARAS_MAX)
        return;

    struct paragraph *p = &doc->paras[para];

    at = CLAMP(at, 0, p->len);

    /* Eine neue Ueberschrift waere selten gemeint: Was hinter einer
     * Ueberschrift beginnt, ist gewoehnlicher Text. Eine Liste dagegen
     * geht weiter. */
    uint8_t style = p->style == STYLE_LIST ? STYLE_LIST : STYLE_BODY;

    if (p->style == STYLE_QUOTE)
        style = STYLE_QUOTE;

    int next = wdoc_insert(doc, para, style);

    p = &doc->paras[para];                 /* das Feld hat sich bewegt */

    struct paragraph *q = &doc->paras[next];
    int rest = p->len - at;

    for (int i = 0; i < rest; i++) {
        q->text[i] = p->text[at + i];
        q->marks[i] = p->marks[at + i];
    }
    q->len = rest;
    q->text[rest] = '\0';
    q->align = p->align;

    p->len = at;
    p->text[at] = '\0';
}

int wdoc_join(struct wdoc *doc, int para)
{
    if (para <= 0 || para >= doc->count)
        return 0;

    struct paragraph *before = &doc->paras[para - 1];
    struct paragraph *here = &doc->paras[para];
    int at = before->len;
    int room = PARA_TEXT_MAX - 1 - before->len;
    int take = MIN(here->len, room);

    for (int i = 0; i < take; i++) {
        before->text[before->len + i] = here->text[i];
        before->marks[before->len + i] = here->marks[i];
    }
    before->len += take;
    before->text[before->len] = '\0';

    wdoc_remove(doc, para);
    return at;
}

const char *wdoc_style_name(uint8_t style)
{
    switch (style) {
    case STYLE_H1:    return tr("Ueberschrift 1");
    case STYLE_H2:    return tr("Ueberschrift 2");
    case STYLE_LIST:  return tr("Aufzaehlung");
    case STYLE_QUOTE: return tr("Zitat");
    default:          return tr("Textkoerper");
    }
}

size_t wdoc_chars(const struct wdoc *doc)
{
    size_t n = 0;

    for (int i = 0; i < doc->count; i++)
        n += (size_t)doc->paras[i].len;
    return n;
}

size_t wdoc_words(const struct wdoc *doc)
{
    size_t n = 0;

    for (int i = 0; i < doc->count; i++) {
        const struct paragraph *p = &doc->paras[i];
        bool inside = false;

        for (int k = 0; k < p->len; k++) {
            bool space = p->text[k] == ' ' || p->text[k] == '\t';

            if (!space && !inside)
                n++;
            inside = !space;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* HTML schreiben                                                      */
/* ------------------------------------------------------------------ */

struct sink {
    char  *buf;
    size_t size;
    size_t used;
};

static void emit(struct sink *s, const char *text)
{
    while (*text) {
        if (s->used + 1 < s->size)
            s->buf[s->used] = *text;
        s->used++;
        text++;
    }
}

static void emit_escaped(struct sink *s, char ch)
{
    switch (ch) {
    case '<': emit(s, "&lt;");  break;
    case '>': emit(s, "&gt;");  break;
    case '&': emit(s, "&amp;"); break;
    default:
        if (s->used + 1 < s->size)
            s->buf[s->used] = ch;
        s->used++;
        break;
    }
}

static const char *tag_for(uint8_t style)
{
    switch (style) {
    case STYLE_H1:    return "h1";
    case STYLE_H2:    return "h2";
    case STYLE_LIST:  return "li";
    case STYLE_QUOTE: return "blockquote";
    default:          return "p";
    }
}

/* Schreibt den Text eines Absatzes und setzt dabei <b> und <u>, wo die
 * Auszeichnung wechselt. */
static void emit_runs(struct sink *s, const struct paragraph *p)
{
    uint8_t open = 0;

    for (int i = 0; i < p->len; i++) {
        uint8_t want = p->marks[i];

        if ((open & MARK_UNDERLINE) && !(want & MARK_UNDERLINE)) {
            emit(s, "</u>");
            open &= (uint8_t)~MARK_UNDERLINE;
        }
        if ((open & MARK_BOLD) && !(want & MARK_BOLD)) {
            emit(s, "</b>");
            open &= (uint8_t)~MARK_BOLD;
        }
        if (!(open & MARK_BOLD) && (want & MARK_BOLD)) {
            emit(s, "<b>");
            open |= MARK_BOLD;
        }
        if (!(open & MARK_UNDERLINE) && (want & MARK_UNDERLINE)) {
            emit(s, "<u>");
            open |= MARK_UNDERLINE;
        }
        emit_escaped(s, p->text[i]);
    }

    if (open & MARK_UNDERLINE)
        emit(s, "</u>");
    if (open & MARK_BOLD)
        emit(s, "</b>");
}

size_t wdoc_to_html(const struct wdoc *doc, const char *title,
                    char *out, size_t size)
{
    struct sink s = { out, size, 0 };

    emit(&s, "<!doctype html>\n<html lang=\"de\">\n<head>\n"
             "<meta charset=\"iso-8859-1\">\n<title>");
    emit(&s, title && title[0] ? title : "Dokument");
    emit(&s, "</title>\n<style>\n"
             "body { font-family: monospace; margin: 2em auto; max-width: 40em; }\n"
             "blockquote { margin-left: 2em; color: #555; }\n"
             "</style>\n</head>\n<body>\n");

    bool in_list = false;

    for (int i = 0; i < doc->count; i++) {
        const struct paragraph *p = &doc->paras[i];
        bool is_list = p->style == STYLE_LIST;

        /* Leere Absaetze am Ende nicht mitschreiben. */
        if (p->len == 0 && i + 1 == doc->count && i > 0)
            break;

        if (is_list && !in_list) {
            emit(&s, "<ul>\n");
            in_list = true;
        } else if (!is_list && in_list) {
            emit(&s, "</ul>\n");
            in_list = false;
        }

        emit(&s, "<");
        emit(&s, tag_for(p->style));
        if (p->align == WA_CENTER)
            emit(&s, " style=\"text-align:center\"");
        else if (p->align == WA_RIGHT)
            emit(&s, " style=\"text-align:right\"");
        emit(&s, ">");
        emit_runs(&s, p);
        emit(&s, "</");
        emit(&s, tag_for(p->style));
        emit(&s, ">\n");
    }

    if (in_list)
        emit(&s, "</ul>\n");
    emit(&s, "</body>\n</html>\n");

    if (size)
        out[MIN(s.used, size - 1)] = '\0';
    return s.used;
}

/* ------------------------------------------------------------------ */
/* HTML lesen                                                          */
/* ------------------------------------------------------------------ */

struct reader {
    struct wdoc *doc;
    int          para;
    uint8_t      marks;
};

/* Haengt Text an den laufenden Absatz und faltet dabei Leerraum
 * zusammen - so, wie HTML es meint. */
static void add_text(struct reader *r, const char *text)
{
    struct paragraph *p = &r->doc->paras[r->para];

    for (const char *c = text; *c; c++) {
        char ch = *c;

        if (ch == '\n' || ch == '\r' || ch == '\t')
            ch = ' ';
        if (ch == ' ' && (p->len == 0 || p->text[p->len - 1] == ' '))
            continue;
        wdoc_insert_char(r->doc, r->para, p->len, ch, r->marks);
    }
}

static uint8_t style_for(const char *name)
{
    if (!strcmp(name, "h1")) return STYLE_H1;
    if (!strcmp(name, "h2")) return STYLE_H2;
    if (!strcmp(name, "h3")) return STYLE_H2;
    if (!strcmp(name, "li")) return STYLE_LIST;
    if (!strcmp(name, "blockquote")) return STYLE_QUOTE;
    return STYLE_BODY;
}

static bool is_block(const char *name)
{
    static const char *blocks[] = {
        "p", "h1", "h2", "h3", "h4", "li", "blockquote", "div", "pre",
    };

    for (size_t i = 0; i < ARRAY_LEN(blocks); i++) {
        if (!strcmp(name, blocks[i]))
            return true;
    }
    return false;
}

static uint8_t align_of(const struct node *node)
{
    const char *style = node_attribute(node, "style");

    if (!style)
        return WA_LEFT;

    /* Es geht nur um die eine Angabe - der Rest interessiert hier
     * nicht. */
    for (const char *p = style; *p; p++) {
        if (strncasecmp(p, "center", 6) == 0)
            return WA_CENTER;
        if (strncasecmp(p, "right", 5) == 0)
            return WA_RIGHT;
    }
    return WA_LEFT;
}

static void walk(struct reader *r, const struct node *node)
{
    for (const struct node *child = node->first; child; child = child->next) {
        if (child->kind == NODE_TEXT) {
            add_text(r, child->text ? child->text : "");
            continue;
        }
        if (child->kind != NODE_ELEMENT || !child->name)
            continue;

        const char *name = child->name;

        if (!strcmp(name, "script") || !strcmp(name, "style") ||
            !strcmp(name, "head"))
            continue;

        if (!strcmp(name, "br")) {
            r->para = wdoc_insert(r->doc, r->para, STYLE_BODY);
            continue;
        }

        uint8_t before = r->marks;

        if (!strcmp(name, "b") || !strcmp(name, "strong"))
            r->marks |= MARK_BOLD;
        else if (!strcmp(name, "u") || !strcmp(name, "ins"))
            r->marks |= MARK_UNDERLINE;
        else if (!strcmp(name, "i") || !strcmp(name, "em"))
            r->marks |= MARK_UNDERLINE;   /* kursiv gibt es nicht */

        if (is_block(name)) {
            struct paragraph *current = &r->doc->paras[r->para];

            /* Nur einen neuen Absatz anfangen, wenn der laufende schon
             * etwas enthaelt. */
            if (current->len > 0)
                r->para = wdoc_insert(r->doc, r->para, STYLE_BODY);

            r->doc->paras[r->para].style = style_for(name);
            r->doc->paras[r->para].align = align_of(child);

            walk(r, child);

            if (r->doc->paras[r->para].len > 0 &&
                r->doc->count < DOC_PARAS_MAX)
                r->para = wdoc_insert(r->doc, r->para, STYLE_BODY);
        } else {
            walk(r, child);
        }

        r->marks = before;
    }
}

void wdoc_from_html(struct wdoc *doc, const char *html, size_t length,
                    char *title, size_t title_size)
{
    struct document page;

    memset(&page, 0, sizeof(page));
    html_build(&page, html, length);

    wdoc_clear(doc);

    struct reader r = { doc, 0, 0 };

    if (page.body)
        walk(&r, page.body);
    else if (page.root)
        walk(&r, page.root);

    /* Einen leeren Absatz am Ende laesst der Aufbau oft stehen. */
    while (doc->count > 1 && doc->paras[doc->count - 1].len == 0)
        doc->count--;

    if (title && title_size)
        strlcpy(title, page.title, title_size);

    document_free(&page);
}
