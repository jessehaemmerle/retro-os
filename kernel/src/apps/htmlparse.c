/* htmlparse.c - erzeugt aus HTML einen Dokumentbaum.
 *
 * Der Leser haelt einen Stapel offener Elemente. Kommt ein Schlusszeichen
 * fuer ein Element, das nicht obenauf liegt, werden die dazwischen
 * liegenden Elemente stillschweigend geschlossen. Bestimmte Elemente
 * schliessen sich ausserdem gegenseitig, etwa zwei aufeinanderfolgende
 * Absaetze oder Tabellenzeilen.
 */

#include "htmlparse.h"
#include "kstring.h"
#include "mm.h"

#define STACK_DEPTH 64

struct builder {
    struct document *doc;
    struct node     *stack[STACK_DEPTH];
    int32_t          depth;
};

/* ------------------------------------------------------------------ */
/* Zeichenverweise                                                     */
/* ------------------------------------------------------------------ */

struct entity {
    const char *name;
    uint16_t    code;
};

static const struct entity entities[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
    { "apos", '\'' }, { "nbsp", 160 }, { "copy", 169 }, { "reg", 174 },
    { "deg", 176 }, { "plusmn", 177 }, { "sup2", 178 }, { "sup3", 179 },
    { "micro", 181 }, { "para", 182 }, { "middot", 183 }, { "frac14", 188 },
    { "frac12", 189 }, { "frac34", 190 }, { "times", 215 }, { "divide", 247 },
    { "auml", 228 }, { "ouml", 246 }, { "uuml", 252 },
    { "Auml", 196 }, { "Ouml", 214 }, { "Uuml", 220 }, { "szlig", 223 },
    { "agrave", 224 }, { "aacute", 225 }, { "acirc", 226 }, { "atilde", 227 },
    { "aring", 229 }, { "aelig", 230 }, { "ccedil", 231 }, { "egrave", 232 },
    { "eacute", 233 }, { "ecirc", 234 }, { "euml", 235 }, { "igrave", 236 },
    { "iacute", 237 }, { "icirc", 238 }, { "iuml", 239 }, { "ntilde", 241 },
    { "ograve", 242 }, { "oacute", 243 }, { "ocirc", 244 }, { "otilde", 245 },
    { "oslash", 248 }, { "ugrave", 249 }, { "uacute", 250 }, { "ucirc", 251 },
    { "yacute", 253 }, { "cent", 162 }, { "pound", 163 }, { "yen", 165 },
    { "sect", 167 }, { "laquo", 171 }, { "raquo", 187 }, { "iquest", 191 },
    { "hellip", 0x2026 }, { "ndash", 0x2013 }, { "mdash", 0x2014 },
    { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C },
    { "rdquo", 0x201D }, { "bull", 0x2022 }, { "dagger", 0x2020 },
    { "euro", 0x20AC }, { "trade", 0x2122 }, { "larr", 0x2190 },
    { "uarr", 0x2191 }, { "rarr", 0x2192 }, { "darr", 0x2193 },
    { "harr", 0x2194 }, { "minus", 0x2212 }, { "shy", 173 },
    { "thinsp", ' ' }, { "ensp", ' ' }, { "emsp", ' ' },
};

/* Bildet einen Unicode-Wert auf die eingebaute Latin-1-Schrift ab. */
static uint8_t to_latin1(uint32_t code)
{
    if (code < 256)
        return (uint8_t)code;

    switch (code) {
    case 0x2018: case 0x2019: case 0x201B: return '\'';
    case 0x201C: case 0x201D: case 0x201F: return '"';
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2022: case 0x00B7:              return 149;
    case 0x2026:                           return '.';
    case 0x00A0:                           return ' ';
    case 0x20AC:                           return 128;
    case 0x2190:                           return '<';
    case 0x2192:                           return '>';
    case 0x2191:                           return '^';
    case 0x2193:                           return 'v';
    case 0x2122:                           return 174;
    default:
        if (code >= 0x2000 && code <= 0x200F)
            return ' ';
        return '?';
    }
}

static uint32_t parse_number(const char *s, size_t length)
{
    uint32_t value = 0;
    bool hex = length > 1 && (s[0] == 'x' || s[0] == 'X');
    size_t i = hex ? 1 : 0;

    for (; i < length; i++) {
        char c = s[i];
        uint32_t digit;

        if (c >= '0' && c <= '9')
            digit = (uint32_t)(c - '0');
        else if (hex && c >= 'a' && c <= 'f')
            digit = (uint32_t)(c - 'a' + 10);
        else if (hex && c >= 'A' && c <= 'F')
            digit = (uint32_t)(c - 'A' + 10);
        else
            return 0;
        value = value * (hex ? 16u : 10u) + digit;
        if (value > 0x10FFFF)
            return 0;
    }
    return value;
}

size_t html_unescape(char *text)
{
    char *read = text, *write = text;

    while (*read) {
        if (*read != '&') {
            *write++ = *read++;
            continue;
        }

        const char *name = read + 1;
        const char *end = name;

        while (*end && *end != ';' && (size_t)(end - name) < 10)
            end++;

        size_t length = (size_t)(end - name);

        if (*end != ';' || length == 0) {
            *write++ = *read++;
            continue;
        }

        uint32_t code = 0;

        if (*name == '#') {
            code = parse_number(name + 1, length - 1);
        } else {
            for (size_t i = 0; i < ARRAY_LEN(entities); i++) {
                if (strlen(entities[i].name) == length &&
                    strncmp(entities[i].name, name, length) == 0) {
                    code = entities[i].code;
                    break;
                }
            }
        }

        if (code == 0) {
            *write++ = *read++;
            continue;
        }
        *write++ = (char)to_latin1(code);
        read = (char *)end + 1;
    }
    *write = '\0';
    return (size_t)(write - text);
}

/* Wandelt UTF-8 im Quelltext in Latin-1 um, soweit moeglich. */
static void utf8_to_latin1(char *text)
{
    unsigned char *read = (unsigned char *)text;
    unsigned char *write = (unsigned char *)text;

    while (*read) {
        uint32_t code;
        int extra;

        if (*read < 0x80) {
            *write++ = *read++;
            continue;
        } else if ((*read & 0xE0) == 0xC0) {
            code = *read & 0x1Fu;
            extra = 1;
        } else if ((*read & 0xF0) == 0xE0) {
            code = *read & 0x0Fu;
            extra = 2;
        } else if ((*read & 0xF8) == 0xF0) {
            code = *read & 0x07u;
            extra = 3;
        } else {
            *write++ = *read++;      /* keine gueltige Folge */
            continue;
        }

        unsigned char *save = read;

        read++;
        for (int i = 0; i < extra; i++) {
            if ((*read & 0xC0) != 0x80) {
                code = 0xFFFFFFFFu;
                break;
            }
            code = (code << 6) | (*read++ & 0x3Fu);
        }
        if (code == 0xFFFFFFFFu) {
            read = save;
            *write++ = *read++;
            continue;
        }
        *write++ = to_latin1(code);
    }
    *write = '\0';
}

/* ------------------------------------------------------------------ */
/* Elementkunde                                                        */
/* ------------------------------------------------------------------ */

static bool is_void(const char *name)
{
    static const char *list[] = {
        "area", "base", "br", "col", "embed", "hr", "img", "input",
        "link", "meta", "param", "source", "track", "wbr"
    };

    for (size_t i = 0; i < ARRAY_LEN(list); i++)
        if (strcmp(name, list[i]) == 0)
            return true;
    return false;
}

/* Elemente, deren Inhalt roh gelesen wird. */
static bool is_raw_text(const char *name)
{
    return strcmp(name, "script") == 0 || strcmp(name, "style") == 0 ||
           strcmp(name, "textarea") == 0 || strcmp(name, "title") == 0;
}

/* Ein neues Element dieser Art schliesst ein offenes gleicher Art. */
static bool closes_same(const char *name)
{
    static const char *list[] = {
        "p", "li", "dt", "dd", "option", "tr", "td", "th", "thead",
        "tbody", "tfoot"
    };

    for (size_t i = 0; i < ARRAY_LEN(list); i++)
        if (strcmp(name, list[i]) == 0)
            return true;
    return false;
}

/* Ein Absatz endet, sobald ein Blockelement beginnt. */
static bool closes_paragraph(const char *name)
{
    static const char *list[] = {
        "address", "article", "aside", "blockquote", "div", "dl", "fieldset",
        "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4",
        "h5", "h6", "header", "hr", "main", "nav", "ol", "p", "pre",
        "section", "table", "ul"
    };

    for (size_t i = 0; i < ARRAY_LEN(list); i++)
        if (strcmp(name, list[i]) == 0)
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Stapelverwaltung                                                    */
/* ------------------------------------------------------------------ */

static struct node *top(struct builder *b)
{
    return b->depth > 0 ? b->stack[b->depth - 1] : b->doc->root;
}

static void push(struct builder *b, struct node *node)
{
    if (b->depth < STACK_DEPTH)
        b->stack[b->depth++] = node;
}

/* Schliesst bis einschliesslich zum Element mit diesem Namen. */
static bool close_until(struct builder *b, const char *name)
{
    for (int32_t i = b->depth - 1; i >= 0; i--) {
        if (b->stack[i]->name && strcmp(b->stack[i]->name, name) == 0) {
            b->depth = i;
            return true;
        }
    }
    return false;
}

static bool is_open(struct builder *b, const char *name)
{
    for (int32_t i = b->depth - 1; i >= 0; i--)
        if (b->stack[i]->name && strcmp(b->stack[i]->name, name) == 0)
            return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* Text einfuegen                                                      */
/* ------------------------------------------------------------------ */

static void add_text(struct builder *b, const char *start, size_t length,
                     bool raw)
{
    if (length == 0)
        return;

    char *text = kmalloc(length + 1);

    if (!text)
        return;
    memcpy(text, start, length);
    text[length] = '\0';

    if (!raw) {
        utf8_to_latin1(text);
        html_unescape(text);
    }
    if (text[0] == '\0') {
        kfree(text);
        return;
    }

    struct node *node = node_create(NODE_TEXT, NULL);

    if (!node) {
        kfree(text);
        return;
    }
    node->text = text;
    node->id = b->doc->next_id++;
    node_append(top(b), node);
}

/* ------------------------------------------------------------------ */
/* Zeichen lesen                                                       */
/* ------------------------------------------------------------------ */

static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

static bool space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Liest die Attribute eines Starttags ab pos bis zur schliessenden
 * spitzen Klammer. Gibt die Stelle danach zurueck. */
static size_t read_attributes(struct node *element, const char *s,
                              size_t pos, size_t length, bool *self_closing)
{
    *self_closing = false;

    while (pos < length) {
        while (pos < length && space_char(s[pos]))
            pos++;
        if (pos >= length)
            break;
        if (s[pos] == '>') {
            pos++;
            break;
        }
        if (s[pos] == '/') {
            *self_closing = true;
            pos++;
            continue;
        }

        size_t name_start = pos;

        while (pos < length && !space_char(s[pos]) && s[pos] != '=' &&
               s[pos] != '>' && s[pos] != '/')
            pos++;

        size_t name_length = pos - name_start;

        if (name_length == 0) {
            pos++;
            continue;
        }

        char name[64];

        if (name_length >= sizeof(name))
            name_length = sizeof(name) - 1;
        memcpy(name, s + name_start, name_length);
        name[name_length] = '\0';

        while (pos < length && space_char(s[pos]))
            pos++;

        if (pos >= length || s[pos] != '=') {
            node_set_attribute(element, name, "");
            continue;
        }
        pos++;
        while (pos < length && space_char(s[pos]))
            pos++;
        if (pos >= length)
            break;

        size_t value_start, value_length;

        if (s[pos] == '"' || s[pos] == '\'') {
            char quote = s[pos++];

            value_start = pos;
            while (pos < length && s[pos] != quote)
                pos++;
            value_length = pos - value_start;
            if (pos < length)
                pos++;
        } else {
            value_start = pos;
            while (pos < length && !space_char(s[pos]) && s[pos] != '>')
                pos++;
            value_length = pos - value_start;
        }

        char *value = kmalloc(value_length + 1);

        if (value) {
            memcpy(value, s + value_start, value_length);
            value[value_length] = '\0';
            utf8_to_latin1(value);
            html_unescape(value);
            node_set_attribute(element, name, value);
            kfree(value);
        }
    }
    return pos;
}

/* ------------------------------------------------------------------ */
/* Hauptschleife                                                       */
/* ------------------------------------------------------------------ */

static void build_into(struct builder *b, const char *s, size_t length)
{
    size_t pos = 0;

    while (pos < length) {
        if (s[pos] != '<') {
            size_t start = pos;

            while (pos < length && s[pos] != '<')
                pos++;
            add_text(b, s + start, pos - start, false);
            continue;
        }

        /* Kommentar */
        if (pos + 4 <= length && strncmp(s + pos, "<!--", 4) == 0) {
            size_t end = pos + 4;

            while (end + 3 <= length && strncmp(s + end, "-->", 3) != 0)
                end++;

            struct node *comment = node_create(NODE_COMMENT, NULL);

            if (comment) {
                size_t clen = end - (pos + 4);

                comment->text = kmalloc(clen + 1);
                if (comment->text) {
                    memcpy(comment->text, s + pos + 4, clen);
                    comment->text[clen] = '\0';
                }
                comment->id = b->doc->next_id++;
                node_append(top(b), comment);
            }
            pos = MIN(end + 3, length);
            continue;
        }

        /* Doctype und Verarbeitungsanweisungen */
        if (pos + 2 <= length && (s[pos + 1] == '!' || s[pos + 1] == '?')) {
            while (pos < length && s[pos] != '>')
                pos++;
            if (pos < length)
                pos++;
            continue;
        }

        /* Schlusszeichen */
        if (pos + 2 <= length && s[pos + 1] == '/') {
            size_t start = pos + 2;
            size_t end = start;

            while (end < length && name_char(s[end]))
                end++;

            char name[64];
            size_t nlen = MIN(end - start, sizeof(name) - 1);

            memcpy(name, s + start, nlen);
            name[nlen] = '\0';
            for (size_t i = 0; i < nlen; i++)
                if (name[i] >= 'A' && name[i] <= 'Z')
                    name[i] = (char)(name[i] + 32);

            close_until(b, name);
            while (end < length && s[end] != '>')
                end++;
            pos = MIN(end + 1, length);
            continue;
        }

        /* Starttag */
        if (!(pos + 1 < length && name_char(s[pos + 1]))) {
            add_text(b, s + pos, 1, false);
            pos++;
            continue;
        }

        size_t start = pos + 1;
        size_t end = start;

        while (end < length && name_char(s[end]))
            end++;

        char name[64];
        size_t nlen = MIN(end - start, sizeof(name) - 1);

        memcpy(name, s + start, nlen);
        name[nlen] = '\0';
        for (size_t i = 0; i < nlen; i++)
            if (name[i] >= 'A' && name[i] <= 'Z')
                name[i] = (char)(name[i] + 32);

        /* Stillschweigend schliessen, was nicht offen bleiben darf. */
        if (closes_same(name) && is_open(b, name))
            close_until(b, name);
        else if (closes_paragraph(name) && is_open(b, "p"))
            close_until(b, "p");
        if (strcmp(name, "li") == 0 && is_open(b, "li"))
            close_until(b, "li");

        struct node *element = node_create(NODE_ELEMENT, name);

        if (!element)
            break;
        element->id = b->doc->next_id++;

        bool self_closing = false;

        pos = read_attributes(element, s, end, length, &self_closing);
        node_append(top(b), element);

        if (strcmp(name, "html") == 0 && !b->doc->html)
            b->doc->html = element;
        else if (strcmp(name, "head") == 0 && !b->doc->head)
            b->doc->head = element;
        else if (strcmp(name, "body") == 0 && !b->doc->body)
            b->doc->body = element;

        if (is_void(name) || self_closing)
            continue;

        if (is_raw_text(name)) {
            /* Bis zum passenden Schlusszeichen alles woertlich nehmen. */
            size_t body = pos;
            size_t stop = body;
            size_t want = strlen(name);

            for (;;) {
                while (stop < length && s[stop] != '<')
                    stop++;
                if (stop >= length)
                    break;
                if (stop + 2 + want <= length && s[stop + 1] == '/' &&
                    strncasecmp(s + stop + 2, name, want) == 0)
                    break;
                stop++;
            }

            push(b, element);
            if (strcmp(name, "title") == 0) {
                add_text(b, s + body, stop - body, false);
            } else {
                add_text(b, s + body, stop - body, true);
            }
            b->depth--;

            pos = stop;
            while (pos < length && s[pos] != '>')
                pos++;
            if (pos < length)
                pos++;
            continue;
        }

        push(b, element);
    }
}

/* ------------------------------------------------------------------ */
/* Aufraeumen und Titel                                                */
/* ------------------------------------------------------------------ */

static void find_title(struct document *doc)
{
    struct node *title = dom_by_tag(doc->root, "title", 0);

    if (title)
        dom_text_content(title, doc->title, sizeof(doc->title));

    /* Fuehrende und folgende Leerzeichen wegschneiden. */
    char *p = doc->title;

    while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r')
        p++;
    if (p != doc->title)
        memmove(doc->title, p, strlen(p) + 1);

    size_t length = strlen(doc->title);

    while (length > 0 && (doc->title[length - 1] == ' ' ||
                          doc->title[length - 1] == '\n' ||
                          doc->title[length - 1] == '\t' ||
                          doc->title[length - 1] == '\r'))
        doc->title[--length] = '\0';
}

/* Fehlt ein body, wird alles Sichtbare zu seinem Inhalt. */
static void ensure_body(struct document *doc)
{
    if (doc->body)
        return;

    struct node *body = node_create(NODE_ELEMENT, "body");

    if (!body)
        return;
    body->id = doc->next_id++;

    struct node *source = doc->html ? doc->html : doc->root;
    struct node *child = source->first;

    while (child) {
        struct node *next = child->next;

        if (child->kind == NODE_ELEMENT && child->name &&
            (strcmp(child->name, "head") == 0 ||
             strcmp(child->name, "body") == 0)) {
            child = next;
            continue;
        }
        node_remove(child);
        node_append(body, child);
        child = next;
    }
    node_append(source, body);
    doc->body = body;
}

void html_build(struct document *doc, const char *source, size_t length)
{
    struct builder b;

    memset(&b, 0, sizeof(b));
    b.doc = doc;

    if (!doc->root)
        document_init(doc);

    build_into(&b, source, length);
    ensure_body(doc);
    find_title(doc);
}

void html_build_plain(struct document *doc, const char *source, size_t length)
{
    if (!doc->root)
        document_init(doc);

    struct node *body = node_create(NODE_ELEMENT, "body");
    struct node *pre = node_create(NODE_ELEMENT, "pre");
    struct node *text = node_create(NODE_TEXT, NULL);

    if (!body || !pre || !text) {
        node_free(body);
        node_free(pre);
        node_free(text);
        return;
    }

    text->text = kmalloc(length + 1);
    if (text->text) {
        memcpy(text->text, source, length);
        text->text[length] = '\0';
        utf8_to_latin1(text->text);
    }

    body->id = doc->next_id++;
    pre->id = doc->next_id++;
    text->id = doc->next_id++;

    node_append(pre, text);
    node_append(body, pre);
    node_append(doc->root, body);
    doc->body = body;
}

void html_set_inner(struct node *parent, const char *fragment)
{
    if (!parent)
        return;

    struct node *child = parent->first;

    while (child) {
        struct node *next = child->next;

        node_remove(child);
        node_free(child);
        child = next;
    }

    if (!fragment || !*fragment)
        return;

    /* Ein eigenes Dokument bauen und die Kinder herueberhaengen. */
    struct document temporary;
    struct builder b;

    document_init(&temporary);
    memset(&b, 0, sizeof(b));
    b.doc = &temporary;
    build_into(&b, fragment, strlen(fragment));

    struct node *source = temporary.root;
    struct node *node = source->first;

    while (node) {
        struct node *next = node->next;

        node_remove(node);
        node_append(parent, node);
        node = next;
    }
    document_free(&temporary);
}
