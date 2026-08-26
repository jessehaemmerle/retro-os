/* jsparse.c - Zerteiler fuer JavaScript.
 *
 * Erst werden Zeichen zu Wortmarken zusammengefasst, dann baut ein
 * rekursiver Abstieg daraus den Syntaxbaum. Die Rangfolge der
 * zweistelligen Verknuepfungen steht in einer Tabelle.
 */

#include "jsint.h"
#include "kstring.h"

enum token_kind {
    T_END, T_NUMBER, T_STRING, T_TEMPLATE, T_IDENT, T_KEYWORD, T_PUNCT,
    T_REGEX,
};

struct token {
    enum token_kind kind;
    const char *start;
    size_t      length;
    js_num      number;
    char       *text;           /* bei Zeichenketten der entpackte Inhalt */
    int32_t     line;
    bool        newline_before;
};

struct parser {
    struct js_context *ctx;
    const char *source;
    size_t      length;
    size_t      pos;
    int32_t     line;

    struct token current;
    struct token ahead;
    bool         has_ahead;

    bool         failed;
};

static const char *keywords[] = {
    "var", "let", "const", "function", "return", "if", "else", "for",
    "while", "do", "break", "continue", "new", "delete", "typeof", "void",
    "in", "instanceof", "this", "null", "true", "false", "undefined",
    "switch", "case", "default", "throw", "try", "catch", "finally",
    "class", "extends", "super", "yield", "await", "async", "of", "static",
    "get", "set",
};

/* ------------------------------------------------------------------ */
/* Zeichenkunde                                                        */
/* ------------------------------------------------------------------ */

static bool ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c == '$' || (unsigned char)c >= 0x80;
}

static bool ident_part(char c)
{
    return ident_start(c) || (c >= '0' && c <= '9');
}

static bool digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

/* ------------------------------------------------------------------ */
/* Marken lesen                                                        */
/* ------------------------------------------------------------------ */

static void fail(struct parser *p, const char *message)
{
    if (p->failed)
        return;
    p->failed = true;
    ksnprintf(p->ctx->error, sizeof(p->ctx->error),
              "Zeile %d: %s", p->line, message);
    p->ctx->failed = true;
}

/* Liest eine Zahl in Festkommadarstellung. */
static js_num read_number(struct parser *p)
{
    size_t start = p->pos;

    if (p->source[p->pos] == '0' && p->pos + 1 < p->length &&
        (p->source[p->pos + 1] == 'x' || p->source[p->pos + 1] == 'X')) {
        p->pos += 2;

        int64_t value = 0;

        while (p->pos < p->length) {
            char c = p->source[p->pos];
            int64_t d;

            if (digit(c))
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                break;
            value = value * 16 + d;
            p->pos++;
        }
        return js_from_int(value);
    }
    if (p->source[p->pos] == '0' && p->pos + 1 < p->length &&
        (p->source[p->pos + 1] == 'b' || p->source[p->pos + 1] == 'B')) {
        p->pos += 2;

        int64_t value = 0;

        while (p->pos < p->length &&
               (p->source[p->pos] == '0' || p->source[p->pos] == '1'))
            value = value * 2 + (p->source[p->pos++] - '0');
        return js_from_int(value);
    }

    int64_t whole = 0;

    while (p->pos < p->length && (digit(p->source[p->pos]) ||
                                  p->source[p->pos] == '_')) {
        if (p->source[p->pos] != '_')
            whole = whole * 10 + (p->source[p->pos] - '0');
        p->pos++;
    }

    js_num result = js_from_int(whole);

    if (p->pos < p->length && p->source[p->pos] == '.' &&
        p->pos + 1 < p->length && digit(p->source[p->pos + 1])) {
        p->pos++;

        int64_t frac = 0, scale = 1;

        while (p->pos < p->length && digit(p->source[p->pos]) &&
               scale < 1000000000LL) {
            frac = frac * 10 + (p->source[p->pos++] - '0');
            scale *= 10;
        }
        while (p->pos < p->length && digit(p->source[p->pos]))
            p->pos++;

        /* Nachkommateil in Sechzehntausendstel umrechnen, kaufmaennisch
         * gerundet - sonst waere 0.1 spuerbar zu klein. */
        result = js_add_num(result,
                            (js_num)((((__int128)frac << JS_FRACTION) +
                                      scale / 2) / scale));
    }

    if (p->pos < p->length && (p->source[p->pos] == 'e' ||
                               p->source[p->pos] == 'E')) {
        p->pos++;

        bool negative = false;

        if (p->pos < p->length && (p->source[p->pos] == '+' ||
                                   p->source[p->pos] == '-'))
            negative = p->source[p->pos++] == '-';

        int32_t exponent = 0;

        while (p->pos < p->length && digit(p->source[p->pos]))
            exponent = exponent * 10 + (p->source[p->pos++] - '0');
        for (int32_t i = 0; i < exponent && i < 40; i++)
            result = negative ? js_div_num(result, js_from_int(10))
                              : js_mul_num(result, js_from_int(10));
    }

    UNUSED(start);
    return result;
}

/* Loest die Fluchtzeichen einer Zeichenkette auf. */
static char *read_quoted(struct parser *p, char quote, bool *has_expression)
{
    size_t start = p->pos;
    size_t need = 0;

    if (has_expression)
        *has_expression = false;

    /* Erst die Laenge bestimmen. */
    size_t scan = p->pos;

    while (scan < p->length && p->source[scan] != quote) {
        if (p->source[scan] == '\\')
            scan++;
        scan++;
        need++;
    }

    char *out = js_alloc(p->ctx, need + 1);

    if (!out) {
        fail(p, "kein Speicher");
        return NULL;
    }

    size_t at = 0;

    while (p->pos < p->length && p->source[p->pos] != quote) {
        char c = p->source[p->pos++];

        if (c == '\n')
            p->line++;
        if (c != '\\') {
            out[at++] = c;
            continue;
        }
        if (p->pos >= p->length)
            break;

        char escape = p->source[p->pos++];

        switch (escape) {
        case 'n': out[at++] = '\n'; break;
        case 't': out[at++] = '\t'; break;
        case 'r': out[at++] = '\r'; break;
        case 'b': out[at++] = '\b'; break;
        case 'f': out[at++] = '\f'; break;
        case 'v': out[at++] = '\v'; break;
        case '0': out[at++] = '\0'; break;
        case '\n': p->line++; break;
        case 'x': {
            int32_t value = 0;

            for (int i = 0; i < 2 && p->pos < p->length; i++) {
                char h = p->source[p->pos++];

                if (digit(h))
                    value = value * 16 + (h - '0');
                else if (h >= 'a' && h <= 'f')
                    value = value * 16 + (h - 'a' + 10);
                else if (h >= 'A' && h <= 'F')
                    value = value * 16 + (h - 'A' + 10);
                else {
                    p->pos--;
                    break;
                }
            }
            out[at++] = (char)value;
            break;
        }
        case 'u': {
            int32_t value = 0;
            int32_t count = 4;

            if (p->pos < p->length && p->source[p->pos] == '{') {
                p->pos++;
                count = 8;
            }
            for (int32_t i = 0; i < count && p->pos < p->length; i++) {
                char h = p->source[p->pos];

                if (h == '}') {
                    p->pos++;
                    break;
                }
                if (digit(h))
                    value = value * 16 + (h - '0');
                else if (h >= 'a' && h <= 'f')
                    value = value * 16 + (h - 'a' + 10);
                else if (h >= 'A' && h <= 'F')
                    value = value * 16 + (h - 'A' + 10);
                else
                    break;
                p->pos++;
            }
            out[at++] = value < 256 ? (char)value : '?';
            break;
        }
        default:
            out[at++] = escape;
            break;
        }
    }
    if (p->pos < p->length)
        p->pos++;                /* schliessendes Anfuehrungszeichen */
    out[at] = '\0';
    UNUSED(start);
    return out;
}

static bool is_keyword(const char *start, size_t length)
{
    for (size_t i = 0; i < ARRAY_LEN(keywords); i++)
        if (strlen(keywords[i]) == length &&
            strncmp(keywords[i], start, length) == 0)
            return true;
    return false;
}

/* Nach diesen Marken kann ein Schraegstrich nur ein Muster einleiten. */
static bool regex_allowed(const struct token *previous)
{
    if (previous->kind == T_NUMBER || previous->kind == T_STRING ||
        previous->kind == T_TEMPLATE || previous->kind == T_IDENT ||
        previous->kind == T_REGEX)
        return false;
    if (previous->kind == T_KEYWORD) {
        if (previous->length == 4 &&
            (strncmp(previous->start, "this", 4) == 0 ||
             strncmp(previous->start, "true", 4) == 0 ||
             strncmp(previous->start, "null", 4) == 0))
            return false;
        return true;
    }
    if (previous->kind == T_PUNCT) {
        if (previous->length == 1 && (previous->start[0] == ')' ||
                                      previous->start[0] == ']' ||
                                      previous->start[0] == '}'))
            return false;
        if (previous->length == 2 && (strncmp(previous->start, "++", 2) == 0 ||
                                      strncmp(previous->start, "--", 2) == 0))
            return false;
    }
    return true;
}

static const char *punctuators[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=",
    "?\?=", "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "**", "<<", ">>",
    "{", "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/",
    "%", "&", "|", "^", "!", "~", "?", ":", "=", ".",
};

static void next_token(struct parser *p, struct token *out,
                       const struct token *previous)
{
    memset(out, 0, sizeof(*out));
    out->newline_before = false;

    for (;;) {
        while (p->pos < p->length && space(p->source[p->pos])) {
            if (p->source[p->pos] == '\n') {
                p->line++;
                out->newline_before = true;
            }
            p->pos++;
        }
        if (p->pos + 1 < p->length && p->source[p->pos] == '/' &&
            p->source[p->pos + 1] == '/') {
            while (p->pos < p->length && p->source[p->pos] != '\n')
                p->pos++;
            continue;
        }
        if (p->pos + 1 < p->length && p->source[p->pos] == '/' &&
            p->source[p->pos + 1] == '*') {
            p->pos += 2;
            while (p->pos + 1 < p->length &&
                   !(p->source[p->pos] == '*' && p->source[p->pos + 1] == '/')) {
                if (p->source[p->pos] == '\n') {
                    p->line++;
                    out->newline_before = true;
                }
                p->pos++;
            }
            p->pos = MIN(p->pos + 2, p->length);
            continue;
        }
        break;
    }

    out->line = p->line;
    out->start = p->source + p->pos;

    if (p->pos >= p->length) {
        out->kind = T_END;
        return;
    }

    char c = p->source[p->pos];

    if (digit(c) || (c == '.' && p->pos + 1 < p->length &&
                     digit(p->source[p->pos + 1]))) {
        out->kind = T_NUMBER;
        out->number = read_number(p);
        out->length = (size_t)(p->source + p->pos - out->start);
        return;
    }

    if (c == '"' || c == '\'') {
        p->pos++;
        out->kind = T_STRING;
        out->text = read_quoted(p, c, NULL);
        out->length = out->text ? strlen(out->text) : 0;
        return;
    }

    if (c == '`') {
        p->pos++;
        out->kind = T_TEMPLATE;
        out->text = read_quoted(p, '`', NULL);
        out->length = out->text ? strlen(out->text) : 0;
        return;
    }

    if (ident_start(c)) {
        size_t start = p->pos;

        while (p->pos < p->length && ident_part(p->source[p->pos]))
            p->pos++;
        out->length = p->pos - start;
        out->kind = is_keyword(out->start, out->length) ? T_KEYWORD : T_IDENT;
        return;
    }

    /* Ein Schraegstrich kann Division oder ein Muster sein. */
    if (c == '/' && previous && regex_allowed(previous)) {
        size_t save = p->pos;

        p->pos++;

        bool in_class = false;
        bool closed = false;

        while (p->pos < p->length) {
            char q = p->source[p->pos];

            if (q == '\\') {
                p->pos += 2;
                continue;
            }
            if (q == '\n')
                break;
            if (q == '[')
                in_class = true;
            else if (q == ']')
                in_class = false;
            else if (q == '/' && !in_class) {
                p->pos++;
                closed = true;
                break;
            }
            p->pos++;
        }
        if (closed) {
            while (p->pos < p->length && ident_part(p->source[p->pos]))
                p->pos++;
            out->kind = T_REGEX;
            out->length = (size_t)(p->source + p->pos - out->start);
            return;
        }
        p->pos = save;
    }

    for (size_t i = 0; i < ARRAY_LEN(punctuators); i++) {
        size_t length = strlen(punctuators[i]);

        if (p->pos + length <= p->length &&
            strncmp(p->source + p->pos, punctuators[i], length) == 0) {
            out->kind = T_PUNCT;
            out->length = length;
            p->pos += length;
            return;
        }
    }

    p->pos++;
    out->kind = T_PUNCT;
    out->length = 1;
}

static void advance(struct parser *p)
{
    if (p->has_ahead) {
        p->current = p->ahead;
        p->has_ahead = false;
        return;
    }

    struct token previous = p->current;

    next_token(p, &p->current, &previous);
}

static struct token *peek(struct parser *p)
{
    if (!p->has_ahead) {
        struct token previous = p->current;

        next_token(p, &p->ahead, &previous);
        p->has_ahead = true;
    }
    return &p->ahead;
}

static bool is_punct(const struct token *t, const char *text)
{
    size_t length = strlen(text);

    return t->kind == T_PUNCT && t->length == length &&
           strncmp(t->start, text, length) == 0;
}

static bool is_word(const struct token *t, const char *text)
{
    size_t length = strlen(text);

    return (t->kind == T_KEYWORD || t->kind == T_IDENT) &&
           t->length == length && strncmp(t->start, text, length) == 0;
}

static bool eat_punct(struct parser *p, const char *text)
{
    if (!is_punct(&p->current, text))
        return false;
    advance(p);
    return true;
}

static bool eat_word(struct parser *p, const char *text)
{
    if (!is_word(&p->current, text))
        return false;
    advance(p);
    return true;
}

static void expect_punct(struct parser *p, const char *text)
{
    if (!eat_punct(p, text)) {
        char message[80];

        ksnprintf(message, sizeof(message), "\"%s\" erwartet", text);
        fail(p, message);
    }
}

/* ------------------------------------------------------------------ */
/* Baumknoten                                                          */
/* ------------------------------------------------------------------ */

static struct ast *make(struct parser *p, enum ast_kind kind)
{
    struct ast *node = js_alloc(p->ctx, sizeof(*node));

    if (!node) {
        fail(p, "kein Speicher");
        return NULL;
    }
    node->kind = kind;
    node->line = p->current.line;
    return node;
}

static char *token_text(struct parser *p, const struct token *t)
{
    if (t->text)
        return t->text;

    char *copy = js_alloc(p->ctx, t->length + 1);

    if (copy) {
        memcpy(copy, t->start, t->length);
        copy[t->length] = '\0';
    }
    return copy;
}

/* ------------------------------------------------------------------ */
/* Ausdruecke                                                          */
/* ------------------------------------------------------------------ */

static struct ast *parse_expression(struct parser *p);
static struct ast *parse_assignment(struct parser *p);
static struct ast *parse_statement(struct parser *p);
static struct ast *parse_block(struct parser *p);

/* Ein Parameterverzeichnis oder eine Argumentliste. */
static struct ast *parse_arguments(struct parser *p)
{
    struct ast *head = NULL, *tail = NULL;

    expect_punct(p, "(");
    while (!is_punct(&p->current, ")") && p->current.kind != T_END &&
           !p->failed) {
        struct ast *item;

        if (eat_punct(p, "...")) {
            item = make(p, AST_SPREAD);
            if (item)
                item->a = parse_assignment(p);
        } else {
            item = parse_assignment(p);
        }
        if (!item)
            break;
        if (tail)
            tail->next = item;
        else
            head = item;
        tail = item;
        if (!eat_punct(p, ","))
            break;
    }
    expect_punct(p, ")");
    return head;
}

static struct ast *parse_parameters(struct parser *p)
{
    struct ast *head = NULL, *tail = NULL;

    expect_punct(p, "(");
    while (!is_punct(&p->current, ")") && p->current.kind != T_END &&
           !p->failed) {
        struct ast *param = make(p, AST_IDENT);

        if (!param)
            break;
        if (eat_punct(p, "...")) {
            param->kind = AST_SPREAD;
            param->text = token_text(p, &p->current);
            advance(p);
        } else {
            param->text = token_text(p, &p->current);
            advance(p);
            if (eat_punct(p, "="))
                param->a = parse_assignment(p);
        }
        if (tail)
            tail->next = param;
        else
            head = param;
        tail = param;
        if (!eat_punct(p, ","))
            break;
    }
    expect_punct(p, ")");
    return head;
}

static struct ast *parse_function(struct parser *p, bool expression)
{
    struct ast *fn = make(p, AST_FUNCTION);

    if (!fn)
        return NULL;

    if (p->current.kind == T_IDENT ||
        (p->current.kind == T_KEYWORD && !is_punct(&p->current, "("))) {
        if (!is_punct(&p->current, "(")) {
            fn->text = token_text(p, &p->current);
            advance(p);
        }
    }
    fn->list = parse_parameters(p);
    fn->a = parse_block(p);
    fn->flag = expression;
    return fn;
}

/* Prueft, ob ab der offenen Klammer eine Pfeilfunktion folgt. */
static bool looks_like_arrow(struct parser *p)
{
    size_t save_pos = p->pos;
    int32_t save_line = p->line;
    struct token save_current = p->current;
    struct token save_ahead = p->ahead;
    bool save_has = p->has_ahead;
    int32_t depth = 0;
    bool result = false;

    /* Vom aktuellen "(" bis zur passenden ")" springen. */
    struct token t = p->current;

    for (int32_t guard = 0; guard < 4096; guard++) {
        if (t.kind == T_END)
            break;
        if (is_punct(&t, "("))
            depth++;
        else if (is_punct(&t, ")")) {
            depth--;
            if (depth == 0) {
                struct token after;
                struct token previous = t;

                next_token(p, &after, &previous);
                result = is_punct(&after, "=>");
                break;
            }
        }

        struct token previous = t;

        next_token(p, &t, &previous);
    }

    p->pos = save_pos;
    p->line = save_line;
    p->current = save_current;
    p->ahead = save_ahead;
    p->has_ahead = save_has;
    return result;
}

static struct ast *parse_arrow_body(struct parser *p, struct ast *params)
{
    struct ast *fn = make(p, AST_ARROW);

    if (!fn)
        return NULL;
    fn->list = params;
    expect_punct(p, "=>");
    if (is_punct(&p->current, "{")) {
        fn->a = parse_block(p);
    } else {
        struct ast *ret = make(p, AST_RETURN);

        if (ret) {
            ret->a = parse_assignment(p);

            struct ast *block = make(p, AST_BLOCK);

            if (block)
                block->list = ret;
            fn->a = block;
        }
    }
    return fn;
}

static struct ast *parse_object_literal(struct parser *p)
{
    struct ast *object = make(p, AST_OBJECT);
    struct ast *tail = NULL;

    if (!object)
        return NULL;
    expect_punct(p, "{");

    while (!is_punct(&p->current, "}") && p->current.kind != T_END &&
           !p->failed) {
        struct ast *entry = make(p, AST_ASSIGN);

        if (!entry)
            break;

        if (eat_punct(p, "...")) {
            entry->kind = AST_SPREAD;
            entry->a = parse_assignment(p);
        } else {
            bool computed = false;

            if (is_punct(&p->current, "[")) {
                advance(p);
                entry->b = parse_assignment(p);
                expect_punct(p, "]");
                computed = true;
            } else if (p->current.kind == T_STRING ||
                       p->current.kind == T_NUMBER) {
                if (p->current.kind == T_NUMBER) {
                    char buffer[48];

                    js_number_to_text(p->current.number, buffer,
                                      sizeof(buffer));
                    entry->text = js_strdup(p->ctx, buffer);
                } else {
                    entry->text = token_text(p, &p->current);
                }
                advance(p);
            } else {
                entry->text = token_text(p, &p->current);
                advance(p);
            }
            entry->flag = computed;

            if (eat_punct(p, ":")) {
                entry->a = parse_assignment(p);
            } else if (is_punct(&p->current, "(")) {
                /* Kurzschreibweise fuer Verfahren. */
                struct ast *fn = make(p, AST_FUNCTION);

                if (fn) {
                    fn->list = parse_parameters(p);
                    fn->a = parse_block(p);
                    fn->flag = true;
                }
                entry->a = fn;
            } else {
                /* Kurzschreibweise: { x } bedeutet { x: x } */
                struct ast *ref = make(p, AST_IDENT);

                if (ref)
                    ref->text = entry->text;
                entry->a = ref;
            }
        }

        if (tail)
            tail->next = entry;
        else
            object->list = entry;
        tail = entry;

        if (!eat_punct(p, ","))
            break;
    }
    expect_punct(p, "}");
    return object;
}

static struct ast *parse_array_literal(struct parser *p)
{
    struct ast *array = make(p, AST_ARRAY);
    struct ast *tail = NULL;

    if (!array)
        return NULL;
    expect_punct(p, "[");

    while (!is_punct(&p->current, "]") && p->current.kind != T_END &&
           !p->failed) {
        struct ast *item;

        if (is_punct(&p->current, ",")) {
            item = make(p, AST_UNDEFINED);
        } else if (eat_punct(p, "...")) {
            item = make(p, AST_SPREAD);
            if (item)
                item->a = parse_assignment(p);
        } else {
            item = parse_assignment(p);
        }
        if (!item)
            break;
        if (tail)
            tail->next = item;
        else
            array->list = item;
        tail = item;
        if (!eat_punct(p, ","))
            break;
    }
    expect_punct(p, "]");
    return array;
}

static struct ast *parse_primary(struct parser *p)
{
    struct token t = p->current;

    if (t.kind == T_NUMBER) {
        struct ast *node = make(p, AST_NUMBER);

        if (node)
            node->number = t.number;
        advance(p);
        return node;
    }
    if (t.kind == T_STRING) {
        struct ast *node = make(p, AST_STRING);

        if (node)
            node->text = token_text(p, &t);
        advance(p);
        return node;
    }
    if (t.kind == T_TEMPLATE) {
        struct ast *node = make(p, AST_TEMPLATE);

        if (node)
            node->text = token_text(p, &t);
        advance(p);
        return node;
    }
    if (t.kind == T_REGEX) {
        /* Muster koennen wir nicht auswerten - wir liefern den Text. */
        struct ast *node = make(p, AST_STRING);

        if (node)
            node->text = token_text(p, &t);
        advance(p);
        return node;
    }
    if (is_punct(&t, "(")) {
        if (looks_like_arrow(p)) {
            struct ast *params = parse_parameters(p);

            return parse_arrow_body(p, params);
        }
        advance(p);

        struct ast *inner = parse_expression(p);

        expect_punct(p, ")");
        return inner;
    }
    if (is_punct(&t, "[")) {
        return parse_array_literal(p);
    }
    if (is_punct(&t, "{")) {
        return parse_object_literal(p);
    }
    if (is_word(&t, "function")) {
        advance(p);
        return parse_function(p, true);
    }
    if (is_word(&t, "async")) {
        advance(p);
        if (is_word(&p->current, "function")) {
            advance(p);
            return parse_function(p, true);
        }
        return parse_primary(p);
    }
    if (is_word(&t, "new")) {
        advance(p);

        struct ast *node = make(p, AST_NEW);

        if (!node)
            return NULL;
        node->a = parse_primary(p);

        /* Etwaige Punkte gehoeren noch zum Namen. */
        while (is_punct(&p->current, ".")) {
            advance(p);

            struct ast *member = make(p, AST_MEMBER);

            if (!member)
                break;
            member->a = node->a;
            member->text = token_text(p, &p->current);
            advance(p);
            node->a = member;
        }
        if (is_punct(&p->current, "("))
            node->list = parse_arguments(p);
        return node;
    }
    if (is_word(&t, "this")) {
        advance(p);
        return make(p, AST_THIS);
    }
    if (is_word(&t, "true") || is_word(&t, "false")) {
        struct ast *node = make(p, AST_BOOL);

        if (node)
            node->flag = is_word(&t, "true");
        advance(p);
        return node;
    }
    if (is_word(&t, "null")) {
        advance(p);
        return make(p, AST_NULL);
    }
    if (is_word(&t, "undefined")) {
        advance(p);
        return make(p, AST_UNDEFINED);
    }
    if (t.kind == T_IDENT || t.kind == T_KEYWORD) {
        /* Eine einzelne Pfeilfunktion ohne Klammern: x => ... */
        struct token *after = peek(p);

        if (is_punct(after, "=>")) {
            struct ast *param = make(p, AST_IDENT);

            if (param)
                param->text = token_text(p, &t);
            advance(p);
            return parse_arrow_body(p, param);
        }

        struct ast *node = make(p, AST_IDENT);

        if (node)
            node->text = token_text(p, &t);
        advance(p);
        return node;
    }

    fail(p, "unerwartetes Zeichen");
    advance(p);
    return make(p, AST_UNDEFINED);
}

static struct ast *parse_call_chain(struct parser *p, struct ast *base)
{
    for (;;) {
        if (is_punct(&p->current, ".") || is_punct(&p->current, "?.")) {
            bool optional = is_punct(&p->current, "?.");

            advance(p);
            if (optional && is_punct(&p->current, "(")) {
                struct ast *call = make(p, AST_CALL);

                if (!call)
                    return base;
                call->a = base;
                call->list = parse_arguments(p);
                call->flag = true;
                base = call;
                continue;
            }

            struct ast *member = make(p, AST_MEMBER);

            if (!member)
                return base;
            member->a = base;
            member->text = token_text(p, &p->current);
            member->flag = optional;
            advance(p);
            base = member;
            continue;
        }
        if (is_punct(&p->current, "[")) {
            advance(p);

            struct ast *index = make(p, AST_INDEX);

            if (!index)
                return base;
            index->a = base;
            index->b = parse_expression(p);
            expect_punct(p, "]");
            base = index;
            continue;
        }
        if (is_punct(&p->current, "(")) {
            struct ast *call = make(p, AST_CALL);

            if (!call)
                return base;
            call->a = base;
            call->list = parse_arguments(p);
            base = call;
            continue;
        }
        if (p->current.kind == T_TEMPLATE) {
            /* Markierte Vorlagen behandeln wir wie einen Aufruf. */
            struct ast *call = make(p, AST_CALL);
            struct ast *arg = make(p, AST_STRING);

            if (!call || !arg)
                return base;
            arg->text = token_text(p, &p->current);
            advance(p);
            call->a = base;
            call->list = arg;
            base = call;
            continue;
        }
        break;
    }
    return base;
}

static struct ast *parse_unary(struct parser *p)
{
    struct token t = p->current;
    enum ast_op op = OP_NONE;

    if (is_punct(&t, "!"))
        op = OP_NOT;
    else if (is_punct(&t, "-"))
        op = OP_NEGATE;
    else if (is_punct(&t, "+"))
        op = OP_PLUS;
    else if (is_punct(&t, "~"))
        op = OP_BITNOT;
    else if (is_word(&t, "typeof"))
        op = OP_TYPEOF;
    else if (is_word(&t, "void"))
        op = OP_VOID;
    else if (is_word(&t, "delete"))
        op = OP_DELETE;
    else if (is_word(&t, "await")) {
        advance(p);
        return parse_unary(p);
    }

    if (op != OP_NONE) {
        advance(p);

        struct ast *node = make(p, AST_UNARY);

        if (!node)
            return NULL;
        node->op = op;
        node->a = parse_unary(p);
        return node;
    }

    if (is_punct(&t, "++") || is_punct(&t, "--")) {
        bool increment = is_punct(&t, "++");

        advance(p);

        struct ast *node = make(p, AST_UPDATE);

        if (!node)
            return NULL;
        node->op = increment ? OP_INCREMENT : OP_DECREMENT;
        node->flag = true;             /* Vorne stehend */
        node->a = parse_unary(p);
        return node;
    }

    struct ast *base = parse_call_chain(p, parse_primary(p));

    if ((is_punct(&p->current, "++") || is_punct(&p->current, "--")) &&
        !p->current.newline_before) {
        bool increment = is_punct(&p->current, "++");

        advance(p);

        struct ast *node = make(p, AST_UPDATE);

        if (!node)
            return base;
        node->op = increment ? OP_INCREMENT : OP_DECREMENT;
        node->flag = false;            /* Hinten stehend */
        node->a = base;
        return node;
    }
    return base;
}

struct binary_entry {
    const char *text;
    enum ast_op op;
    int32_t     precedence;
    bool        word;
};

static const struct binary_entry binaries[] = {
    { "**", OP_POW, 11, false },
    { "*", OP_MUL, 10, false },
    { "/", OP_DIV, 10, false },
    { "%", OP_MOD, 10, false },
    { "+", OP_ADD, 9, false },
    { "-", OP_SUB, 9, false },
    { "<<", OP_SHL, 8, false },
    { ">>>", OP_USHR, 8, false },
    { ">>", OP_SHR, 8, false },
    { "<=", OP_LE, 7, false },
    { ">=", OP_GE, 7, false },
    { "<", OP_LT, 7, false },
    { ">", OP_GT, 7, false },
    { "instanceof", OP_INSTANCEOF, 7, true },
    { "in", OP_IN, 7, true },
    { "===", OP_STRICT_EQ, 6, false },
    { "!==", OP_STRICT_NE, 6, false },
    { "==", OP_EQ, 6, false },
    { "!=", OP_NE, 6, false },
    { "&", OP_AND, 5, false },
    { "^", OP_XOR, 4, false },
    { "|", OP_OR, 3, false },
    { "&&", OP_LOGICAL_AND, 2, false },
    { "||", OP_LOGICAL_OR, 1, false },
    { "??", OP_NULLISH, 1, false },
};

static const struct binary_entry *match_binary(struct parser *p, bool no_in)
{
    for (size_t i = 0; i < ARRAY_LEN(binaries); i++) {
        const struct binary_entry *e = &binaries[i];

        if (e->word) {
            if (!is_word(&p->current, e->text))
                continue;
            if (no_in && e->op == OP_IN)
                continue;
            return e;
        }
        if (is_punct(&p->current, e->text))
            return e;
    }
    return NULL;
}

static struct ast *parse_binary(struct parser *p, int32_t minimum, bool no_in)
{
    struct ast *left = parse_unary(p);

    for (;;) {
        const struct binary_entry *e = match_binary(p, no_in);

        if (!e || e->precedence < minimum || p->failed)
            break;
        advance(p);

        /* Der Potenzoperator ist rechtsbindend. */
        int32_t next = e->op == OP_POW ? e->precedence : e->precedence + 1;
        struct ast *right = parse_binary(p, next, no_in);
        struct ast *node = make(p, e->op == OP_LOGICAL_AND ||
                                   e->op == OP_LOGICAL_OR ||
                                   e->op == OP_NULLISH
                                   ? AST_LOGICAL : AST_BINARY);

        if (!node)
            break;
        node->op = e->op;
        node->a = left;
        node->b = right;
        left = node;
    }
    return left;
}

static const struct { const char *text; enum ast_op op; } assignments[] = {
    { "+=", OP_ADD }, { "-=", OP_SUB }, { "*=", OP_MUL }, { "/=", OP_DIV },
    { "%=", OP_MOD }, { "**=", OP_POW }, { "&=", OP_AND }, { "|=", OP_OR },
    { "^=", OP_XOR }, { "<<=", OP_SHL }, { ">>=", OP_SHR },
    { ">>>=", OP_USHR }, { "&&=", OP_LOGICAL_AND },
    { "||=", OP_LOGICAL_OR }, { "?\?=", OP_NULLISH },
};

static struct ast *parse_assignment_inner(struct parser *p, bool no_in)
{
    struct ast *left = parse_binary(p, 1, no_in);

    if (is_punct(&p->current, "?")) {
        advance(p);

        struct ast *node = make(p, AST_CONDITIONAL);

        if (!node)
            return left;
        node->a = left;
        node->b = parse_assignment(p);
        expect_punct(p, ":");
        node->c = parse_assignment(p);
        return node;
    }

    if (is_punct(&p->current, "=")) {
        advance(p);

        struct ast *node = make(p, AST_ASSIGN);

        if (!node)
            return left;
        node->op = OP_NONE;
        node->a = left;
        node->b = parse_assignment(p);
        return node;
    }

    for (size_t i = 0; i < ARRAY_LEN(assignments); i++) {
        if (!is_punct(&p->current, assignments[i].text))
            continue;
        advance(p);

        struct ast *node = make(p, AST_ASSIGN);

        if (!node)
            return left;
        node->op = assignments[i].op;
        node->a = left;
        node->b = parse_assignment(p);
        return node;
    }
    return left;
}

static struct ast *parse_assignment(struct parser *p)
{
    return parse_assignment_inner(p, false);
}

static struct ast *parse_expression(struct parser *p)
{
    struct ast *first = parse_assignment(p);

    if (!is_punct(&p->current, ","))
        return first;

    struct ast *node = make(p, AST_SEQUENCE);
    struct ast *tail = first;

    if (!node)
        return first;
    node->list = first;
    while (eat_punct(p, ",")) {
        struct ast *item = parse_assignment(p);

        if (!item)
            break;
        tail->next = item;
        tail = item;
    }
    return node;
}

/* ------------------------------------------------------------------ */
/* Anweisungen                                                         */
/* ------------------------------------------------------------------ */

static void eat_semicolon(struct parser *p)
{
    eat_punct(p, ";");
}

static struct ast *parse_block(struct parser *p)
{
    struct ast *block = make(p, AST_BLOCK);
    struct ast *tail = NULL;

    if (!block)
        return NULL;
    expect_punct(p, "{");
    while (!is_punct(&p->current, "}") && p->current.kind != T_END &&
           !p->failed) {
        struct ast *statement = parse_statement(p);

        if (!statement)
            break;
        if (tail)
            tail->next = statement;
        else
            block->list = statement;
        tail = statement;
    }
    expect_punct(p, "}");
    return block;
}

/* var, let und const - auch mehrere Namen auf einmal. */
static struct ast *parse_var(struct parser *p, bool constant)
{
    struct ast *node = make(p, AST_VAR);
    struct ast *tail = NULL;

    if (!node)
        return NULL;
    node->flag = constant;

    for (;;) {
        struct ast *entry = make(p, AST_ASSIGN);

        if (!entry)
            break;

        /* Zerlegende Zuweisung: { a, b } = ... oder [ a, b ] = ... */
        if (is_punct(&p->current, "{") || is_punct(&p->current, "[")) {
            bool object = is_punct(&p->current, "{");
            char close = object ? '}' : ']';

            advance(p);
            entry->kind = object ? AST_OBJECT : AST_ARRAY;

            struct ast *names = NULL, *names_tail = NULL;

            while (p->current.kind != T_END && !p->failed &&
                   !(p->current.kind == T_PUNCT && p->current.length == 1 &&
                     p->current.start[0] == close)) {
                struct ast *name = make(p, AST_IDENT);

                if (!name)
                    break;
                name->text = token_text(p, &p->current);
                advance(p);
                if (object && eat_punct(p, ":")) {
                    name->a = make(p, AST_IDENT);
                    if (name->a)
                        name->a->text = token_text(p, &p->current);
                    advance(p);
                }
                if (names_tail)
                    names_tail->next = name;
                else
                    names = name;
                names_tail = name;
                if (!eat_punct(p, ","))
                    break;
            }
            if (object)
                expect_punct(p, "}");
            else
                expect_punct(p, "]");
            entry->list = names;
        } else {
            entry->text = token_text(p, &p->current);
            advance(p);
        }

        if (eat_punct(p, "="))
            entry->b = parse_assignment_inner(p, true);

        if (tail)
            tail->next = entry;
        else
            node->list = entry;
        tail = entry;

        if (!eat_punct(p, ","))
            break;
    }
    return node;
}

static struct ast *parse_for(struct parser *p)
{
    expect_punct(p, "(");

    struct ast *init = NULL;
    bool declares = false;
    bool constant = false;

    if (is_word(&p->current, "var") || is_word(&p->current, "let") ||
        is_word(&p->current, "const")) {
        constant = is_word(&p->current, "const");
        advance(p);
        declares = true;
        init = parse_var(p, constant);
    } else if (!is_punct(&p->current, ";")) {
        init = parse_expression(p);
    }

    if (is_word(&p->current, "in") || is_word(&p->current, "of")) {
        bool of = is_word(&p->current, "of");

        advance(p);

        struct ast *node = make(p, of ? AST_FOR_OF : AST_FOR_IN);

        if (!node)
            return NULL;
        node->a = init;
        node->flag = declares;
        node->b = parse_assignment(p);
        expect_punct(p, ")");
        node->c = parse_statement(p);
        return node;
    }

    struct ast *node = make(p, AST_FOR);

    if (!node)
        return NULL;
    node->a = init;
    expect_punct(p, ";");
    if (!is_punct(&p->current, ";"))
        node->b = parse_expression(p);
    expect_punct(p, ";");
    if (!is_punct(&p->current, ")"))
        node->c = parse_expression(p);
    expect_punct(p, ")");
    node->d = parse_statement(p);
    return node;
}

static struct ast *parse_switch(struct parser *p)
{
    struct ast *node = make(p, AST_SWITCH);
    struct ast *tail = NULL;

    if (!node)
        return NULL;
    expect_punct(p, "(");
    node->a = parse_expression(p);
    expect_punct(p, ")");
    expect_punct(p, "{");

    while (!is_punct(&p->current, "}") && p->current.kind != T_END &&
           !p->failed) {
        struct ast *branch = make(p, AST_BLOCK);

        if (!branch)
            break;

        if (eat_word(p, "case")) {
            branch->a = parse_expression(p);
        } else if (eat_word(p, "default")) {
            branch->flag = true;
        } else {
            fail(p, "case oder default erwartet");
            break;
        }
        expect_punct(p, ":");

        struct ast *body_tail = NULL;

        while (!is_word(&p->current, "case") && !is_word(&p->current, "default") &&
               !is_punct(&p->current, "}") && p->current.kind != T_END &&
               !p->failed) {
            struct ast *statement = parse_statement(p);

            if (!statement)
                break;
            if (body_tail)
                body_tail->next = statement;
            else
                branch->list = statement;
            body_tail = statement;
        }

        if (tail)
            tail->next = branch;
        else
            node->list = branch;
        tail = branch;
    }
    expect_punct(p, "}");
    return node;
}

static struct ast *parse_try(struct parser *p)
{
    struct ast *node = make(p, AST_TRY);

    if (!node)
        return NULL;
    node->a = parse_block(p);

    if (eat_word(p, "catch")) {
        if (eat_punct(p, "(")) {
            node->text = token_text(p, &p->current);
            advance(p);
            expect_punct(p, ")");
        }
        node->b = parse_block(p);
    }
    if (eat_word(p, "finally"))
        node->c = parse_block(p);
    return node;
}

/* Klassen werden auf eine Funktion mit Prototyp abgebildet. */
static struct ast *parse_class(struct parser *p)
{
    struct ast *node = make(p, AST_FUNCTION);

    if (!node)
        return NULL;

    if (p->current.kind == T_IDENT) {
        node->text = token_text(p, &p->current);
        advance(p);
    }
    if (eat_word(p, "extends")) {
        node->d = parse_unary(p);
    }
    expect_punct(p, "{");

    struct ast *methods = NULL, *tail = NULL;

    while (!is_punct(&p->current, "}") && p->current.kind != T_END &&
           !p->failed) {
        if (eat_punct(p, ";"))
            continue;

        bool is_static = false;

        if (is_word(&p->current, "static")) {
            struct token *after = peek(p);

            if (!is_punct(after, "(")) {
                is_static = true;
                advance(p);
            }
        }

        struct ast *method = make(p, AST_ASSIGN);

        if (!method)
            break;
        method->text = token_text(p, &p->current);
        method->flag = is_static;
        advance(p);

        struct ast *fn = make(p, AST_FUNCTION);

        if (!fn)
            break;
        fn->list = parse_parameters(p);
        fn->a = parse_block(p);
        fn->flag = true;
        method->a = fn;

        if (tail)
            tail->next = method;
        else
            methods = method;
        tail = method;
    }
    expect_punct(p, "}");

    node->list = NULL;
    node->b = methods;      /* Verfahren des Prototyps */
    node->flag = false;
    return node;
}

static struct ast *parse_statement(struct parser *p)
{
    if (p->failed)
        return NULL;

    if (is_punct(&p->current, "{"))
        return parse_block(p);

    if (eat_punct(p, ";"))
        return make(p, AST_EMPTY);

    if (is_word(&p->current, "var") || is_word(&p->current, "let") ||
        is_word(&p->current, "const")) {
        bool constant = is_word(&p->current, "const");

        advance(p);

        struct ast *node = parse_var(p, constant);

        eat_semicolon(p);
        return node;
    }

    if (eat_word(p, "function"))
        return parse_function(p, false);

    if (eat_word(p, "class"))
        return parse_class(p);

    if (is_word(&p->current, "async")) {
        struct token *after = peek(p);

        if (is_word(after, "function")) {
            advance(p);
            advance(p);
            return parse_function(p, false);
        }
    }

    if (eat_word(p, "if")) {
        struct ast *node = make(p, AST_IF);

        if (!node)
            return NULL;
        expect_punct(p, "(");
        node->a = parse_expression(p);
        expect_punct(p, ")");
        node->b = parse_statement(p);
        if (eat_word(p, "else"))
            node->c = parse_statement(p);
        return node;
    }

    if (eat_word(p, "for"))
        return parse_for(p);

    if (eat_word(p, "while")) {
        struct ast *node = make(p, AST_WHILE);

        if (!node)
            return NULL;
        expect_punct(p, "(");
        node->a = parse_expression(p);
        expect_punct(p, ")");
        node->b = parse_statement(p);
        return node;
    }

    if (eat_word(p, "do")) {
        struct ast *node = make(p, AST_DO_WHILE);

        if (!node)
            return NULL;
        node->b = parse_statement(p);
        if (!eat_word(p, "while"))
            fail(p, "while erwartet");
        expect_punct(p, "(");
        node->a = parse_expression(p);
        expect_punct(p, ")");
        eat_semicolon(p);
        return node;
    }

    if (eat_word(p, "return")) {
        struct ast *node = make(p, AST_RETURN);

        if (!node)
            return NULL;
        if (!is_punct(&p->current, ";") && !is_punct(&p->current, "}") &&
            p->current.kind != T_END && !p->current.newline_before)
            node->a = parse_expression(p);
        eat_semicolon(p);
        return node;
    }

    if (eat_word(p, "break") || is_word(&p->current, "continue")) {
        bool is_break = !is_word(&p->current, "continue");

        if (!is_break)
            advance(p);

        struct ast *node = make(p, is_break ? AST_BREAK : AST_CONTINUE);

        if (!node)
            return NULL;
        if (p->current.kind == T_IDENT && !p->current.newline_before) {
            node->text = token_text(p, &p->current);
            advance(p);
        }
        eat_semicolon(p);
        return node;
    }

    if (eat_word(p, "throw")) {
        struct ast *node = make(p, AST_THROW);

        if (!node)
            return NULL;
        node->a = parse_expression(p);
        eat_semicolon(p);
        return node;
    }

    if (eat_word(p, "switch"))
        return parse_switch(p);

    if (eat_word(p, "try"))
        return parse_try(p);

    /* Eine Marke fuer break und continue. */
    if (p->current.kind == T_IDENT) {
        struct token *after = peek(p);

        if (is_punct(after, ":")) {
            struct ast *node = make(p, AST_LABEL);

            if (!node)
                return NULL;
            node->text = token_text(p, &p->current);
            advance(p);
            advance(p);
            node->a = parse_statement(p);
            return node;
        }
    }

    struct ast *node = make(p, AST_EXPRESSION);

    if (!node)
        return NULL;
    node->a = parse_expression(p);
    eat_semicolon(p);
    return node;
}

/* ------------------------------------------------------------------ */
/* Einstieg                                                            */
/* ------------------------------------------------------------------ */

struct ast *js_parse(struct js_context *ctx, const char *source, size_t length)
{
    struct parser p;

    memset(&p, 0, sizeof(p));
    p.ctx = ctx;
    p.source = source;
    p.length = length;
    p.line = 1;

    advance(&p);

    struct ast *program = make(&p, AST_PROGRAM);
    struct ast *tail = NULL;

    if (!program)
        return NULL;

    while (p.current.kind != T_END && !p.failed) {
        struct ast *statement = parse_statement(&p);

        if (!statement)
            break;
        if (tail)
            tail->next = statement;
        else
            program->list = statement;
        tail = statement;
    }

    return p.failed ? NULL : program;
}
