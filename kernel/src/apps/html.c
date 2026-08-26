/* html.c - ein sehr kleiner HTML-Leser.
 *
 * Aus dem Quelltext einer Seite wird eine flache Liste von Bausteinen:
 * Textstuecke mit Auszeichnung, Verweise, Ueberschriften, Absatzwechsel,
 * Trennlinien, Aufzaehlungspunkte. Der Browser setzt daraus sein Bild.
 *
 * Was fehlt, fehlt bewusst: kein Layoutmodell mit Kaesten, keine
 * Formatvorlagen, kein JavaScript. Fuer Textseiten reicht das erstaunlich
 * weit; alles Weitere waere ein eigenes Projekt.
 */

#include "html.h"
#include "kstring.h"
#include "mm.h"

#define TEXT_CHUNK 512

static void doc_push(struct html_doc *doc, struct html_item item)
{
    if (doc->count == doc->capacity) {
        size_t bigger = doc->capacity ? doc->capacity * 2 : 64;
        struct html_item *grown = krealloc(doc->items,
                                           bigger * sizeof(struct html_item));

        if (!grown)
            return;
        doc->items = grown;
        doc->capacity = bigger;
    }
    doc->items[doc->count++] = item;
}

/* --- Zeichenersetzungen --- */

struct entity { const char *name; unsigned char value; };

static const struct entity entities[] = {
    { "amp", '&' },   { "lt", '<' },    { "gt", '>' },   { "quot", '"' },
    { "apos", '\'' }, { "nbsp", ' ' },  { "auml", 0xE4 },{ "ouml", 0xF6 },
    { "uuml", 0xFC }, { "Auml", 0xC4 }, { "Ouml", 0xD6 },{ "Uuml", 0xDC },
    { "szlig", 0xDF },{ "eacute", 0xE9 },{ "egrave", 0xE8 },
    { "copy", 0xA9 }, { "reg", 0xAE },  { "deg", 0xB0 }, { "euro", 0x80 },
    { "mdash", '-' }, { "ndash", '-' }, { "hellip", '.' },
    { "laquo", '"' }, { "raquo", '"' }, { "bdquo", '"' }, { "ldquo", '"' },
    { "rdquo", '"' }, { "lsquo", '\'' },{ "rsquo", '\'' },
};

/* Liest "&name;" oder "&#123;" und liefert die Laenge des Verbrauchten. */
static size_t decode_entity(const char *p, unsigned char *out)
{
    if (*p != '&')
        return 0;

    const char *semicolon = strchr(p, ';');
    if (!semicolon || semicolon - p > 10)
        return 0;

    size_t length = (size_t)(semicolon - p - 1);
    char name[12];

    memcpy(name, p + 1, length);
    name[length] = '\0';

    if (name[0] == '#') {
        uint32_t value = 0;
        const char *digits = name + 1;
        int base = 10;

        if (*digits == 'x' || *digits == 'X') {
            base = 16;
            digits++;
        }
        for (; *digits; digits++) {
            uint32_t digit;

            if (*digits >= '0' && *digits <= '9')      digit = (uint32_t)(*digits - '0');
            else if (*digits >= 'a' && *digits <= 'f') digit = (uint32_t)(*digits - 'a' + 10);
            else if (*digits >= 'A' && *digits <= 'F') digit = (uint32_t)(*digits - 'A' + 10);
            else return 0;

            value = value * (uint32_t)base + digit;
        }
        *out = (value > 0 && value < 256) ? (unsigned char)value : '?';
        return length + 2;
    }

    for (size_t i = 0; i < ARRAY_LEN(entities); i++) {
        if (strcmp(entities[i].name, name) == 0) {
            *out = entities[i].value;
            return length + 2;
        }
    }
    return 0;
}

/* --- Textpuffer --- */

struct text_buffer {
    char  *data;
    size_t length;
    size_t capacity;
};

static void buffer_reset(struct text_buffer *b)
{
    b->length = 0;
    if (b->data)
        b->data[0] = '\0';
}

static void buffer_add(struct text_buffer *b, char c)
{
    if (b->length + 2 > b->capacity) {
        size_t bigger = b->capacity ? b->capacity * 2 : TEXT_CHUNK;
        char *grown = krealloc(b->data, bigger);

        if (!grown)
            return;
        b->data = grown;
        b->capacity = bigger;
    }
    b->data[b->length++] = c;
    b->data[b->length] = '\0';
}

/* --- Zustand des Lesers --- */

struct parser {
    struct html_doc *doc;
    struct text_buffer text;

    bool bold;
    bool link;
    char href[256];
    uint8_t heading;
    bool preformatted;
    bool in_title;
};

static void flush_text(struct parser *p)
{
    if (p->text.length == 0)
        return;

    struct html_item item;

    memset(&item, 0, sizeof(item));
    item.type    = p->link ? HTML_LINK : HTML_TEXT;
    item.text    = kstrdup(p->text.data);
    item.bold    = p->bold;
    item.heading = p->heading;
    item.pre     = p->preformatted;

    if (p->link)
        item.href = kstrdup(p->href);

    doc_push(p->doc, item);
    buffer_reset(&p->text);
}

static void push_simple(struct parser *p, enum html_item_type type)
{
    struct html_item item;

    flush_text(p);
    memset(&item, 0, sizeof(item));
    item.type = type;
    doc_push(p->doc, item);
}

/* Verhindert doppelte Leerzeilen hintereinander. */
static void push_break(struct parser *p, bool paragraph)
{
    flush_text(p);

    if (p->doc->count > 0) {
        enum html_item_type last = p->doc->items[p->doc->count - 1].type;

        if (last == HTML_PARAGRAPH)
            return;
        if (last == HTML_BREAK && !paragraph)
            return;
    } else if (paragraph) {
        return;
    }

    push_simple(p, paragraph ? HTML_PARAGRAPH : HTML_BREAK);
}

static bool tag_is(const char *tag, const char *name)
{
    size_t n = strlen(name);

    return strncasecmp(tag, name, n) == 0 &&
           (tag[n] == '\0' || tag[n] == ' ' || tag[n] == '/' || tag[n] == '\t' ||
            tag[n] == '\n');
}

/* Liest ein Attribut aus einem Tag-Inhalt heraus. */
static bool tag_attribute(const char *tag, const char *name, char *out,
                          size_t size)
{
    size_t name_length = strlen(name);

    for (const char *p = tag; *p; p++) {
        if (strncasecmp(p, name, name_length) != 0)
            continue;
        if (p != tag && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n')
            continue;

        const char *q = p + name_length;
        while (*q == ' ')
            q++;
        if (*q != '=')
            continue;
        q++;
        while (*q == ' ')
            q++;

        char quote = '\0';
        if (*q == '"' || *q == '\'')
            quote = *q++;

        size_t n = 0;
        while (*q && n + 1 < size) {
            if (quote && *q == quote)
                break;
            if (!quote && (*q == ' ' || *q == '>'))
                break;

            unsigned char decoded;
            size_t used = decode_entity(q, &decoded);

            if (used) {
                out[n++] = (char)decoded;
                q += used;
            } else {
                out[n++] = *q++;
            }
        }
        out[n] = '\0';
        return n > 0;
    }
    return false;
}

static void handle_tag(struct parser *p, const char *tag)
{
    bool closing = (tag[0] == '/');
    const char *name = closing ? tag + 1 : tag;

    if (tag_is(name, "br")) {
        push_break(p, false);
    } else if (tag_is(name, "p") || tag_is(name, "div") ||
               tag_is(name, "section") || tag_is(name, "article") ||
               tag_is(name, "header") || tag_is(name, "footer") ||
               tag_is(name, "nav") || tag_is(name, "main") ||
               tag_is(name, "blockquote") || tag_is(name, "table") ||
               tag_is(name, "form")) {
        push_break(p, true);
    } else if (tag_is(name, "tr") || tag_is(name, "dt") || tag_is(name, "dd")) {
        push_break(p, false);
    } else if (tag_is(name, "td") || tag_is(name, "th")) {
        if (!closing)
            buffer_add(&p->text, ' ');
    } else if (tag_is(name, "hr")) {
        push_simple(p, HTML_RULE);
    } else if (tag_is(name, "ul") || tag_is(name, "ol")) {
        push_break(p, true);
    } else if (tag_is(name, "li")) {
        if (!closing)
            push_simple(p, HTML_BULLET);
        else
            push_break(p, false);
    } else if (tag_is(name, "b") || tag_is(name, "strong")) {
        flush_text(p);
        p->bold = !closing;
    } else if (tag_is(name, "i") || tag_is(name, "em")) {
        flush_text(p);
    } else if (tag_is(name, "pre")) {
        flush_text(p);
        p->preformatted = !closing;
        push_break(p, true);
    } else if (tag_is(name, "title")) {
        flush_text(p);
        p->in_title = !closing;
    } else if (tag_is(name, "img")) {
        char alt[128];

        flush_text(p);
        if (tag_attribute(name, "alt", alt, sizeof(alt)) && alt[0]) {
            struct html_item item;

            memset(&item, 0, sizeof(item));
            item.type = HTML_IMAGE;
            item.text = kstrdup(alt);
            doc_push(p->doc, item);
        } else {
            struct html_item item;

            memset(&item, 0, sizeof(item));
            item.type = HTML_IMAGE;
            item.text = kstrdup("Bild");
            doc_push(p->doc, item);
        }
    } else if (tag_is(name, "a")) {
        flush_text(p);
        if (closing) {
            p->link = false;
            p->href[0] = '\0';
        } else if (tag_attribute(name, "href", p->href, sizeof(p->href))) {
            p->link = true;
        }
    } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' &&
               (name[2] == '\0' || name[2] == ' ' || name[2] == '>')) {
        push_break(p, true);
        p->heading = closing ? 0 : (uint8_t)(name[1] - '0');
    }
}

/* Ueberspringt Bereiche, deren Inhalt kein Text ist. */
static const char *skip_element(const char *p, const char *name)
{
    size_t length = strlen(name);

    while (*p) {
        if (p[0] == '<' && p[1] == '/' &&
            strncasecmp(p + 2, name, length) == 0)
            return p;
        p++;
    }
    return p;
}

void html_parse(struct html_doc *doc, const char *source, size_t length)
{
    struct parser parser;

    memset(doc, 0, sizeof(*doc));
    memset(&parser, 0, sizeof(parser));
    parser.doc = doc;

    const char *p = source;
    const char *end = source + length;
    bool space_pending = false;

    while (p < end && *p) {
        if (*p == '<') {
            /* Kommentare und Anweisungen ueberspringen. */
            if (strncmp(p, "<!--", 4) == 0) {
                const char *close = p;

                while (close < end - 2 &&
                       !(close[0] == '-' && close[1] == '-' && close[2] == '>'))
                    close++;
                p = MIN(close + 3, end);
                continue;
            }

            if (strncasecmp(p, "<script", 7) == 0) {
                p = skip_element(p + 7, "script");
                continue;
            }
            if (strncasecmp(p, "<style", 6) == 0) {
                p = skip_element(p + 6, "style");
                continue;
            }
            if (strncasecmp(p, "<head", 5) == 0) {
                /* Der Kopf wird durchsucht, aber nur der Titel uebernommen. */
            }

            const char *close = strchr(p, '>');
            if (!close)
                break;

            char tag[512];
            size_t tag_length = MIN((size_t)(close - p - 1), sizeof(tag) - 1);

            memcpy(tag, p + 1, tag_length);
            tag[tag_length] = '\0';

            if (tag[0] != '!')
                handle_tag(&parser, tag);

            p = close + 1;
            space_pending = false;
            continue;
        }

        unsigned char c;
        size_t used = decode_entity(p, &c);

        if (used) {
            p += used;
        } else {
            c = (unsigned char)*p++;
        }

        if (!parser.preformatted && (c == ' ' || c == '\t' || c == '\n' ||
                                     c == '\r')) {
            /* Aufeinanderfolgende Leerraeume werden zu einem Leerzeichen. */
            space_pending = parser.text.length > 0;
            continue;
        }

        if (parser.preformatted && c == '\n') {
            push_break(&parser, false);
            continue;
        }

        if (space_pending) {
            buffer_add(&parser.text, ' ');
            space_pending = false;
        }

        if (parser.in_title) {
            size_t n = strlen(doc->title);

            if (n + 1 < sizeof(doc->title)) {
                doc->title[n] = (char)c;
                doc->title[n + 1] = '\0';
            }
            continue;
        }

        buffer_add(&parser.text, (char)c);
    }

    flush_text(&parser);
    kfree(parser.text.data);
}

/* Baut aus reinem Text ein Dokument - fuer text/plain und lokale Dateien. */
void html_parse_plain(struct html_doc *doc, const char *source, size_t length)
{
    memset(doc, 0, sizeof(*doc));

    const char *line = source;
    const char *end  = source + length;

    while (line < end) {
        const char *newline = line;

        while (newline < end && *newline != '\n')
            newline++;

        size_t n = (size_t)(newline - line);
        char *text = kmalloc(n + 1);

        if (!text)
            break;
        memcpy(text, line, n);
        text[n] = '\0';

        /* Wagenruecklauf am Zeilenende entfernen. */
        if (n > 0 && text[n - 1] == '\r')
            text[n - 1] = '\0';

        struct html_item item;
        memset(&item, 0, sizeof(item));
        item.type = HTML_TEXT;
        item.text = text;
        item.pre  = true;
        doc_push(doc, item);

        struct html_item br;
        memset(&br, 0, sizeof(br));
        br.type = HTML_BREAK;
        doc_push(doc, br);

        line = newline + 1;
    }
}

void html_free(struct html_doc *doc)
{
    for (size_t i = 0; i < doc->count; i++) {
        kfree(doc->items[i].text);
        kfree(doc->items[i].href);
    }
    kfree(doc->items);
    doc->items = NULL;
    doc->count = doc->capacity = 0;
    doc->title[0] = '\0';
}
