/* jsinterp.c - der auswertende Teil des Deuters.
 *
 * Der Baum wird unmittelbar durchlaufen. Rueckgabe, Abbruch und
 * geworfene Werte reisen ueber ein Zeichen im Zusammenhang nach oben,
 * statt ueber lange Spruenge - das kommt ohne setjmp aus.
 */

#include "jsint.h"
#include "kstring.h"
#include <stdarg.h>

#define MAX_DEPTH 96
#define STEP_LIMIT 40000000ULL

/* ------------------------------------------------------------------ */
/* Namensraum                                                          */
/* ------------------------------------------------------------------ */

struct js_scope *js_scope_new(struct js_context *ctx, struct js_scope *parent)
{
    struct js_scope *scope = js_alloc(ctx, sizeof(*scope));

    if (scope)
        scope->parent = parent;
    return scope;
}

void js_declare(struct js_context *ctx, struct js_scope *scope,
                const char *name, struct js_value value, bool constant)
{
    if (!scope || !name)
        return;

    for (struct js_binding *b = scope->bindings; b; b = b->next) {
        if (strcmp(b->name, name) == 0) {
            b->value = value;
            b->constant = constant;
            return;
        }
    }

    struct js_binding *b = js_alloc(ctx, sizeof(*b));

    if (!b)
        return;
    b->name = js_strdup(ctx, name);
    b->value = value;
    b->constant = constant;
    b->next = scope->bindings;
    scope->bindings = b;
}

struct js_binding *js_lookup(struct js_scope *scope, const char *name)
{
    for (struct js_scope *s = scope; s; s = s->parent)
        for (struct js_binding *b = s->bindings; b; b = b->next)
            if (strcmp(b->name, name) == 0)
                return b;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Fehler                                                              */
/* ------------------------------------------------------------------ */

void js_throw(struct js_context *ctx, const char *fmt, ...)
{
    char message[240];
    va_list ap;

    va_start(ap, fmt);
    /* ksnprintf kann keine Argumentliste - darum von Hand. */
    ksnprintf(message, sizeof(message), "%s", fmt);
    va_end(ap);

    ctx->signal = SIGNAL_THROW;
    ctx->signal_value = js_str(ctx, message);
    if (!ctx->failed)
        strlcpy(ctx->error, message, sizeof(ctx->error));
    ctx->failed = true;
}

/* ------------------------------------------------------------------ */
/* Umwandlungen                                                        */
/* ------------------------------------------------------------------ */

bool js_truthy(struct js_value v)
{
    switch (v.type) {
    case JS_UNDEFINED:
    case JS_NULL:
        return false;
    case JS_BOOL:
        return v.as.boolean;
    case JS_NUMBER:
        return v.as.number != 0 && !js_is_nan(v.as.number);
    case JS_STRING:
        return v.as.string && v.as.string->length > 0;
    default:
        return true;
    }
}

/* Liest eine Zahl aus einem Text, wie parseFloat es tut. */
static js_num text_to_number(const char *s, size_t length, bool strict)
{
    size_t at = 0;

    while (at < length && (s[at] == ' ' || s[at] == '\t' || s[at] == '\n' ||
                           s[at] == '\r'))
        at++;
    if (at >= length)
        return strict ? 0 : JS_NAN;

    bool negative = false;

    if (s[at] == '-') {
        negative = true;
        at++;
    } else if (s[at] == '+') {
        at++;
    }

    if (at + 8 <= length && strncmp(s + at, "Infinity", 8) == 0)
        return negative ? JS_NEG_INF : JS_POS_INF;

    /* Hexadezimal */
    if (at + 1 < length && s[at] == '0' && (s[at + 1] == 'x' || s[at + 1] == 'X')) {
        int64_t value = 0;

        at += 2;
        if (at >= length)
            return JS_NAN;
        while (at < length) {
            char c = s[at];
            int64_t d;

            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                break;
            value = value * 16 + d;
            at++;
        }
        return js_from_int(negative ? -value : value);
    }

    if (at >= length || (!(s[at] >= '0' && s[at] <= '9') && s[at] != '.'))
        return JS_NAN;

    int64_t whole = 0;
    bool any = false;

    while (at < length && s[at] >= '0' && s[at] <= '9') {
        whole = whole * 10 + (s[at++] - '0');
        any = true;
    }

    js_num result = js_from_int(whole);

    if (at < length && s[at] == '.') {
        at++;

        int64_t frac = 0, scale = 1;

        while (at < length && s[at] >= '0' && s[at] <= '9' &&
               scale < 1000000000LL) {
            frac = frac * 10 + (s[at++] - '0');
            scale *= 10;
            any = true;
        }
        while (at < length && s[at] >= '0' && s[at] <= '9')
            at++;
        result = js_add_num(result,
                            (js_num)((((__int128)frac << JS_FRACTION) +
                                      scale / 2) / scale));
    }
    if (!any)
        return JS_NAN;

    if (at < length && (s[at] == 'e' || s[at] == 'E')) {
        at++;

        bool exponent_negative = false;

        if (at < length && (s[at] == '+' || s[at] == '-'))
            exponent_negative = s[at++] == '-';

        int32_t exponent = 0;

        while (at < length && s[at] >= '0' && s[at] <= '9')
            exponent = exponent * 10 + (s[at++] - '0');
        for (int32_t i = 0; i < exponent && i < 40; i++)
            result = exponent_negative ? js_div_num(result, js_from_int(10))
                                       : js_mul_num(result, js_from_int(10));
    }

    /* Bei Number("12abc") gilt der ganze Text - sonst wie parseFloat. */
    if (strict) {
        while (at < length && (s[at] == ' ' || s[at] == '\t' ||
                               s[at] == '\n' || s[at] == '\r'))
            at++;
        if (at != length)
            return JS_NAN;
    }
    return negative ? js_sub_num(0, result) : result;
}

js_num js_to_number(struct js_context *ctx, struct js_value v)
{
    switch (v.type) {
    case JS_NUMBER:
        return v.as.number;
    case JS_BOOL:
        return v.as.boolean ? JS_ONE : 0;
    case JS_NULL:
        return 0;
    case JS_UNDEFINED:
        return JS_NAN;
    case JS_STRING:
        if (!v.as.string || v.as.string->length == 0)
            return 0;
        return text_to_number(v.as.string->data, v.as.string->length, true);
    default: {
        struct js_object *o = v.as.object;

        if (o->klass == CLASS_ARRAY) {
            if (o->length == 0)
                return 0;
            if (o->length == 1)
                return js_to_number(ctx, o->elements[0]);
        }
        return JS_NAN;
    }
    }
}

size_t js_number_to_text(js_num value, char *out, size_t size)
{
    if (size == 0)
        return 0;
    if (value == JS_NAN)
        return strlcpy(out, "NaN", size);
    if (value == JS_POS_INF)
        return strlcpy(out, "Infinity", size);
    if (value == JS_NEG_INF)
        return strlcpy(out, "-Infinity", size);

    char buffer[64];
    size_t at = 0;
    bool negative = value < 0;
    uint64_t magnitude = negative ? (uint64_t)(-value) : (uint64_t)value;
    uint64_t whole = magnitude >> JS_FRACTION;
    uint64_t frac = magnitude & (JS_ONE - 1);

    /* Ganzer Teil rueckwaerts. */
    char digits[24];
    size_t count = 0;

    if (whole == 0) {
        digits[count++] = '0';
    } else {
        uint64_t rest = whole;

        while (rest > 0 && count < sizeof(digits)) {
            digits[count++] = (char)('0' + (rest % 10));
            rest /= 10;
        }
    }

    if (negative)
        buffer[at++] = '-';
    while (count > 0)
        buffer[at++] = digits[--count];

    /* Vier Nachkommastellen sind alles, was 1/65536 hergibt. Es wird
     * gerundet und Nullen am Ende fallen weg. */
    uint64_t scaled = ((frac * 10000) + (JS_ONE / 2)) >> JS_FRACTION;

    if (scaled >= 10000) {
        scaled = 0;
        whole++;

        /* Der Uebertrag muss noch in die Ziffern des ganzen Teils. */
        at = 0;
        count = 0;
        if (whole == 0) {
            digits[count++] = '0';
        } else {
            uint64_t rest = whole;

            while (rest > 0 && count < sizeof(digits)) {
                digits[count++] = (char)('0' + (rest % 10));
                rest /= 10;
            }
        }
        if (negative)
            buffer[at++] = '-';
        while (count > 0)
            buffer[at++] = digits[--count];
    }

    if (scaled != 0) {
        char decimals[4];
        size_t written = 4;

        for (size_t i = 4; i > 0; i--) {
            decimals[i - 1] = (char)('0' + (scaled % 10));
            scaled /= 10;
        }
        while (written > 0 && decimals[written - 1] == '0')
            written--;

        buffer[at++] = '.';
        for (size_t i = 0; i < written; i++)
            buffer[at++] = decimals[i];
    }

    buffer[at] = '\0';
    return strlcpy(out, buffer, size);
}

static void append(struct js_context *ctx, char **buffer, size_t *at,
                   size_t *capacity, const char *text, size_t length)
{
    if (*at + length + 1 > *capacity) {
        size_t want = *capacity ? *capacity * 2 : 128;

        while (want < *at + length + 1)
            want *= 2;
        if (want > (4u << 20))
            return;

        char *bigger = js_alloc(ctx, want);

        if (!bigger)
            return;
        memcpy(bigger, *buffer, *at);
        *buffer = bigger;
        *capacity = want;
    }
    memcpy(*buffer + *at, text, length);
    *at += length;
    (*buffer)[*at] = '\0';
}

static void stringify(struct js_context *ctx, struct js_value v, char **buffer,
                      size_t *at, size_t *capacity, int32_t depth);

static void stringify_object(struct js_context *ctx, struct js_object *o,
                             char **buffer, size_t *at, size_t *capacity,
                             int32_t depth)
{
    if (depth > 6) {
        append(ctx, buffer, at, capacity, "...", 3);
        return;
    }

    if (o->klass == CLASS_ARRAY) {
        for (size_t i = 0; i < o->length; i++) {
            if (i)
                append(ctx, buffer, at, capacity, ",", 1);
            if (o->elements[i].type != JS_UNDEFINED &&
                o->elements[i].type != JS_NULL)
                stringify(ctx, o->elements[i], buffer, at, capacity, depth + 1);
        }
        return;
    }
    if (o->klass == CLASS_FUNCTION || o->klass == CLASS_NATIVE) {
        append(ctx, buffer, at, capacity, "function ", 9);
        if (o->name)
            append(ctx, buffer, at, capacity, o->name, strlen(o->name));
        append(ctx, buffer, at, capacity, "() { ... }", 10);
        return;
    }
    if (o->klass == CLASS_NODE && o->dom) {
        char text[96];

        if (o->dom->kind == NODE_TEXT)
            ksnprintf(text, sizeof(text), "[object Text]");
        else
            ksnprintf(text, sizeof(text), "[object HTML%sElement]",
                      o->dom->name ? o->dom->name : "");
        append(ctx, buffer, at, capacity, text, strlen(text));
        return;
    }

    /* Ein eigenes toString gewinnt. */
    struct js_value custom = js_get(ctx, o, "toString");

    if (custom.type == JS_OBJECT && custom.as.object &&
        (custom.as.object->klass == CLASS_FUNCTION ||
         custom.as.object->klass == CLASS_NATIVE) && depth < 4) {
        struct js_value result = js_call(ctx, custom, js_object_value(o),
                                         NULL, 0);

        if (result.type == JS_STRING && result.as.string) {
            append(ctx, buffer, at, capacity, result.as.string->data,
                   result.as.string->length);
            return;
        }
    }
    append(ctx, buffer, at, capacity, "[object Object]", 15);
}

static void stringify(struct js_context *ctx, struct js_value v, char **buffer,
                      size_t *at, size_t *capacity, int32_t depth)
{
    char scratch[64];

    switch (v.type) {
    case JS_UNDEFINED:
        append(ctx, buffer, at, capacity, "undefined", 9);
        break;
    case JS_NULL:
        append(ctx, buffer, at, capacity, "null", 4);
        break;
    case JS_BOOL:
        if (v.as.boolean)
            append(ctx, buffer, at, capacity, "true", 4);
        else
            append(ctx, buffer, at, capacity, "false", 5);
        break;
    case JS_NUMBER: {
        size_t length = js_number_to_text(v.as.number, scratch,
                                          sizeof(scratch));

        append(ctx, buffer, at, capacity, scratch, MIN(length,
                                                       sizeof(scratch) - 1));
        break;
    }
    case JS_STRING:
        if (v.as.string)
            append(ctx, buffer, at, capacity, v.as.string->data,
                   v.as.string->length);
        break;
    default:
        if (v.as.object)
            stringify_object(ctx, v.as.object, buffer, at, capacity, depth);
        break;
    }
}

struct js_string *js_to_string(struct js_context *ctx, struct js_value v)
{
    if (v.type == JS_STRING && v.as.string)
        return v.as.string;

    char *buffer = js_alloc(ctx, 128);
    size_t at = 0, capacity = 128;

    if (!buffer)
        return js_string_new(ctx, "", 0);
    buffer[0] = '\0';
    stringify(ctx, v, &buffer, &at, &capacity, 0);
    return js_string_new(ctx, buffer, at);
}

const char *js_typeof(struct js_value v)
{
    switch (v.type) {
    case JS_UNDEFINED: return "undefined";
    case JS_NULL:      return "object";
    case JS_BOOL:      return "boolean";
    case JS_NUMBER:    return "number";
    case JS_STRING:    return "string";
    default:
        if (v.as.object && (v.as.object->klass == CLASS_FUNCTION ||
                            v.as.object->klass == CLASS_NATIVE))
            return "function";
        return "object";
    }
}

bool js_equals(struct js_context *ctx, struct js_value a, struct js_value b,
               bool strict)
{
    if (a.type == b.type) {
        switch (a.type) {
        case JS_UNDEFINED:
        case JS_NULL:
            return true;
        case JS_BOOL:
            return a.as.boolean == b.as.boolean;
        case JS_NUMBER:
            if (js_is_nan(a.as.number) || js_is_nan(b.as.number))
                return false;
            return a.as.number == b.as.number;
        case JS_STRING:
            if (!a.as.string || !b.as.string)
                return a.as.string == b.as.string;
            return a.as.string->length == b.as.string->length &&
                   memcmp(a.as.string->data, b.as.string->data,
                          a.as.string->length) == 0;
        default:
            return a.as.object == b.as.object;
        }
    }
    if (strict)
        return false;

    /* Lockerer Vergleich: null und undefined sind gleich. */
    if ((a.type == JS_NULL && b.type == JS_UNDEFINED) ||
        (a.type == JS_UNDEFINED && b.type == JS_NULL))
        return true;
    if (a.type == JS_NULL || a.type == JS_UNDEFINED ||
        b.type == JS_NULL || b.type == JS_UNDEFINED)
        return false;

    if (a.type == JS_OBJECT || b.type == JS_OBJECT) {
        struct js_string *sa = js_to_string(ctx, a);
        struct js_string *sb = js_to_string(ctx, b);

        return sa && sb && sa->length == sb->length &&
               memcmp(sa->data, sb->data, sa->length) == 0;
    }

    js_num na = js_to_number(ctx, a);
    js_num nb = js_to_number(ctx, b);

    if (js_is_nan(na) || js_is_nan(nb))
        return false;
    return na == nb;
}

/* ------------------------------------------------------------------ */
/* Zugriff auf Eigenschaften                                           */
/* ------------------------------------------------------------------ */

/* Prueft, ob ein Name eine reine Zahl ist. */
static bool index_of_name(const char *name, size_t *out)
{
    if (!name || !*name)
        return false;

    size_t value = 0;

    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;
        value = value * 10 + (size_t)(*p - '0');
        if (value > (1u << 24))
            return false;
    }
    *out = value;
    return true;
}

struct js_value js_get_value(struct js_context *ctx, struct js_value target,
                             const char *key)
{
    if (target.type == JS_STRING) {
        struct js_string *s = target.as.string;

        if (strcmp(key, "length") == 0)
            return js_integer((int64_t)(s ? s->length : 0));

        size_t index;

        if (index_of_name(key, &index)) {
            if (!s || index >= s->length)
                return js_undefined();
            return js_strn(ctx, s->data + index, 1);
        }
        return js_get(ctx, ctx->string_prototype, key);
    }

    if (target.type == JS_NUMBER)
        return js_get(ctx, ctx->number_prototype, key);
    if (target.type == JS_BOOL)
        return js_undefined();

    if (target.type != JS_OBJECT || !target.as.object) {
        js_throw(ctx, "Zugriff auf einen Wert, der kein Objekt ist");
        return js_undefined();
    }

    struct js_object *o = target.as.object;

    if (o->klass == CLASS_ARRAY) {
        if (strcmp(key, "length") == 0)
            return js_integer((int64_t)o->length);

        size_t index;

        if (index_of_name(key, &index)) {
            if (index >= o->length)
                return js_undefined();
            return o->elements[index];
        }
    }
    if (o->klass == CLASS_NODELIST) {
        if (strcmp(key, "length") == 0)
            return js_integer((int64_t)o->length);

        size_t index;

        if (index_of_name(key, &index)) {
            if (index >= o->length)
                return js_undefined();
            return o->elements[index];
        }
    }
    if ((o->klass == CLASS_FUNCTION || o->klass == CLASS_NATIVE)) {
        if (strcmp(key, "name") == 0)
            return js_str(ctx, o->name ? o->name : "");
        if (strcmp(key, "length") == 0) {
            int32_t count = o->arity;

            if (o->klass == CLASS_FUNCTION) {
                count = 0;
                for (struct ast *p = o->params; p; p = p->next)
                    count++;
            }
            return js_integer(count);
        }
    }
    return js_get(ctx, o, key);
}

void js_set_value(struct js_context *ctx, struct js_value target,
                  const char *key, struct js_value value)
{
    if (target.type != JS_OBJECT || !target.as.object)
        return;

    struct js_object *o = target.as.object;

    if (o->setter && o->setter(ctx, o, key, value))
        return;

    if (o->klass == CLASS_ARRAY) {
        size_t index;

        if (strcmp(key, "length") == 0) {
            int64_t want = js_to_int(js_to_number(ctx, value));

            if (want >= 0 && (size_t)want < o->length)
                o->length = (size_t)want;
            else
                while ((int64_t)o->length < want)
                    js_array_push(ctx, o, js_undefined());
            return;
        }
        if (index_of_name(key, &index)) {
            js_array_set(ctx, o, index, value);
            return;
        }
    }
    js_set(ctx, o, key, value);
}

/* ------------------------------------------------------------------ */
/* Aufrufe                                                             */
/* ------------------------------------------------------------------ */

static void bind_parameters(struct js_context *ctx, struct js_object *fn,
                            struct js_scope *scope, struct js_value *args,
                            size_t count)
{
    size_t at = 0;

    for (struct ast *p = fn->params; p; p = p->next) {
        if (p->kind == AST_SPREAD) {
            struct js_object *rest = js_new_array(ctx, 0);

            for (size_t i = at; i < count; i++)
                js_array_push(ctx, rest, args[i]);
            js_declare(ctx, scope, p->text, js_object_value(rest), false);
            at = count;
            continue;
        }

        struct js_value value = at < count ? args[at] : js_undefined();

        if (value.type == JS_UNDEFINED && p->a)
            value = js_eval(ctx, p->a, scope);
        js_declare(ctx, scope, p->text, value, false);
        at++;
    }

    struct js_object *arguments = js_new_array(ctx, 0);

    for (size_t i = 0; i < count; i++)
        js_array_push(ctx, arguments, args[i]);
    js_declare(ctx, scope, "arguments", js_object_value(arguments), false);
}

struct js_value js_call(struct js_context *ctx, struct js_value callee,
                        struct js_value self, struct js_value *args,
                        size_t count)
{
    if (callee.type != JS_OBJECT || !callee.as.object ||
        (callee.as.object->klass != CLASS_FUNCTION &&
         callee.as.object->klass != CLASS_NATIVE)) {
        js_throw(ctx, "Aufruf von etwas, das keine Funktion ist");
        return js_undefined();
    }

    struct js_object *fn = callee.as.object;

    if (fn->has_bound_this)
        self = fn->bound_this;

    if (fn->klass == CLASS_NATIVE)
        return fn->native ? fn->native(ctx, self, args, count)
                          : js_undefined();

    if (ctx->depth >= MAX_DEPTH) {
        js_throw(ctx, "zu viele verschachtelte Aufrufe");
        return js_undefined();
    }

    struct js_scope *scope = js_scope_new(ctx, fn->closure);

    if (!scope)
        return js_undefined();

    bind_parameters(ctx, fn, scope, args, count);

    struct js_value saved_this = ctx->this_value;

    if (!fn->is_arrow)
        ctx->this_value = self;

    ctx->depth++;
    js_exec_block(ctx, fn->body ? fn->body->list : NULL, scope);
    ctx->depth--;

    ctx->this_value = saved_this;

    struct js_value result = js_undefined();

    if (ctx->signal == SIGNAL_RETURN) {
        result = ctx->signal_value;
        ctx->signal = SIGNAL_NONE;
    } else if (ctx->signal == SIGNAL_BREAK || ctx->signal == SIGNAL_CONTINUE) {
        ctx->signal = SIGNAL_NONE;
    }
    return result;
}

/* ------------------------------------------------------------------ */
/* Auswertung von Ausdruecken                                          */
/* ------------------------------------------------------------------ */

static struct js_value make_function(struct js_context *ctx, struct ast *node,
                                     struct js_scope *scope, bool arrow)
{
    struct js_object *fn = js_new_object(ctx, CLASS_FUNCTION);

    if (!fn)
        return js_undefined();
    fn->params = node->list;
    fn->body = node->a;
    fn->closure = scope;
    fn->name = node->text ? node->text : "";
    fn->is_arrow = arrow;

    struct js_object *prototype = js_new_object(ctx, CLASS_OBJECT);

    if (prototype) {
        js_set_hidden(ctx, prototype, "constructor", js_object_value(fn));
        js_set_hidden(ctx, fn, "prototype", js_object_value(prototype));

        /* Bei einer Klasse haengen die Verfahren am Prototyp. */
        for (struct ast *m = node->b; m; m = m->next) {
            struct js_value method = make_function(ctx, m->a, scope, false);

            if (m->flag)
                js_set(ctx, fn, m->text, method);
            else
                js_set(ctx, prototype, m->text, method);
        }
        /* Ein constructor-Verfahren wird zum Rumpf der Funktion selbst. */
        struct js_value *ctor = js_own_slot(prototype, "constructor");

        for (struct ast *m = node->b; m; m = m->next) {
            if (m->flag || strcmp(m->text, "constructor") != 0)
                continue;
            fn->params = m->a->list;
            fn->body = m->a->a;
        }
        UNUSED(ctor);

        /* Vererbung ueber extends. */
        if (node->d) {
            struct js_value parent = js_eval(ctx, node->d, scope);

            if (parent.type == JS_OBJECT && parent.as.object) {
                struct js_value parent_prototype =
                    js_get(ctx, parent.as.object, "prototype");

                if (parent_prototype.type == JS_OBJECT)
                    prototype->prototype = parent_prototype.as.object;
                js_set_hidden(ctx, fn, "__super__", parent);
            }
        }
    }
    return js_object_value(fn);
}

/* Vorlagen mit ${...} werden ausgewertet. */
static struct js_value eval_template(struct js_context *ctx, const char *text,
                                     struct js_scope *scope)
{
    char *buffer = js_alloc(ctx, 128);
    size_t at = 0, capacity = 128;

    if (!buffer)
        return js_str(ctx, text);
    buffer[0] = '\0';

    const char *p = text;

    while (*p) {
        if (p[0] == '$' && p[1] == '{') {
            const char *start = p + 2;
            const char *end = start;
            int32_t depth = 1;

            while (*end && depth > 0) {
                if (*end == '{')
                    depth++;
                else if (*end == '}')
                    depth--;
                if (depth > 0)
                    end++;
            }

            struct ast *program = js_parse(ctx, start, (size_t)(end - start));
            struct js_value value = js_undefined();

            if (program && program->list) {
                ctx->failed = false;
                value = js_eval(ctx, program->list->kind == AST_EXPRESSION
                                     ? program->list->a : program->list, scope);
            }

            struct js_string *s = js_to_string(ctx, value);

            if (s)
                append(ctx, &buffer, &at, &capacity, s->data, s->length);
            p = *end ? end + 1 : end;
            continue;
        }
        append(ctx, &buffer, &at, &capacity, p, 1);
        p++;
    }
    return js_strn(ctx, buffer, at);
}

/* Verkettet oder rechnet, je nach Art der Werte. */
static struct js_value binary_add(struct js_context *ctx, struct js_value a,
                                  struct js_value b)
{
    if (a.type == JS_STRING || b.type == JS_STRING ||
        a.type == JS_OBJECT || b.type == JS_OBJECT) {
        struct js_string *sa = js_to_string(ctx, a);
        struct js_string *sb = js_to_string(ctx, b);

        if (!sa || !sb)
            return js_undefined();

        struct js_string *joined = js_string_new(ctx, NULL,
                                                 sa->length + sb->length);

        if (!joined)
            return js_undefined();
        memcpy(joined->data, sa->data, sa->length);
        memcpy(joined->data + sa->length, sb->data, sb->length);
        joined->data[joined->length] = '\0';

        struct js_value v;

        v.type = JS_STRING;
        v.as.string = joined;
        return v;
    }
    return js_number(js_add_num(js_to_number(ctx, a), js_to_number(ctx, b)));
}

static int32_t compare_values(struct js_context *ctx, struct js_value a,
                              struct js_value b, bool *unordered)
{
    *unordered = false;

    if (a.type == JS_STRING && b.type == JS_STRING) {
        struct js_string *sa = a.as.string;
        struct js_string *sb = b.as.string;
        size_t shortest = MIN(sa->length, sb->length);
        int result = memcmp(sa->data, sb->data, shortest);

        if (result != 0)
            return result < 0 ? -1 : 1;
        if (sa->length == sb->length)
            return 0;
        return sa->length < sb->length ? -1 : 1;
    }

    js_num na = js_to_number(ctx, a);
    js_num nb = js_to_number(ctx, b);

    if (js_is_nan(na) || js_is_nan(nb)) {
        *unordered = true;
        return 0;
    }
    if (na == nb)
        return 0;
    return na < nb ? -1 : 1;
}

static int32_t to_int32(struct js_context *ctx, struct js_value v)
{
    int64_t value = js_to_int(js_to_number(ctx, v));

    return (int32_t)(uint32_t)(uint64_t)value;
}

static struct js_value eval_binary(struct js_context *ctx, enum ast_op op,
                                   struct js_value a, struct js_value b)
{
    bool unordered;
    int32_t order;

    switch (op) {
    case OP_ADD:
        return binary_add(ctx, a, b);
    case OP_SUB:
        return js_number(js_sub_num(js_to_number(ctx, a), js_to_number(ctx, b)));
    case OP_MUL:
        return js_number(js_mul_num(js_to_number(ctx, a), js_to_number(ctx, b)));
    case OP_DIV:
        return js_number(js_div_num(js_to_number(ctx, a), js_to_number(ctx, b)));
    case OP_MOD:
        return js_number(js_mod_num(js_to_number(ctx, a), js_to_number(ctx, b)));
    case OP_POW: {
        js_num base = js_to_number(ctx, a);
        int64_t exponent = js_to_int(js_to_number(ctx, b));
        js_num result = JS_ONE;
        bool negative = exponent < 0;

        if (negative)
            exponent = -exponent;
        for (int64_t i = 0; i < exponent && i < 256; i++)
            result = js_mul_num(result, base);
        return js_number(negative ? js_div_num(JS_ONE, result) : result);
    }
    case OP_LT:
        order = compare_values(ctx, a, b, &unordered);
        return js_bool(!unordered && order < 0);
    case OP_GT:
        order = compare_values(ctx, a, b, &unordered);
        return js_bool(!unordered && order > 0);
    case OP_LE:
        order = compare_values(ctx, a, b, &unordered);
        return js_bool(!unordered && order <= 0);
    case OP_GE:
        order = compare_values(ctx, a, b, &unordered);
        return js_bool(!unordered && order >= 0);
    case OP_EQ:
        return js_bool(js_equals(ctx, a, b, false));
    case OP_NE:
        return js_bool(!js_equals(ctx, a, b, false));
    case OP_STRICT_EQ:
        return js_bool(js_equals(ctx, a, b, true));
    case OP_STRICT_NE:
        return js_bool(!js_equals(ctx, a, b, true));
    case OP_AND:
        return js_integer(to_int32(ctx, a) & to_int32(ctx, b));
    case OP_OR:
        return js_integer(to_int32(ctx, a) | to_int32(ctx, b));
    case OP_XOR:
        return js_integer(to_int32(ctx, a) ^ to_int32(ctx, b));
    case OP_SHL:
        return js_integer((int32_t)((uint32_t)to_int32(ctx, a) <<
                                    (to_int32(ctx, b) & 31)));
    case OP_SHR:
        return js_integer(to_int32(ctx, a) >> (to_int32(ctx, b) & 31));
    case OP_USHR:
        return js_integer((int64_t)((uint32_t)to_int32(ctx, a) >>
                                    (to_int32(ctx, b) & 31)));
    case OP_IN: {
        if (b.type != JS_OBJECT || !b.as.object)
            return js_bool(false);

        struct js_string *key = js_to_string(ctx, a);
        struct js_value found = js_get_value(ctx, b, key ? key->data : "");

        return js_bool(found.type != JS_UNDEFINED);
    }
    case OP_INSTANCEOF: {
        if (a.type != JS_OBJECT || b.type != JS_OBJECT || !b.as.object)
            return js_bool(false);

        struct js_value prototype = js_get(ctx, b.as.object, "prototype");

        if (prototype.type != JS_OBJECT)
            return js_bool(false);
        for (struct js_object *o = a.as.object->prototype; o;
             o = o->prototype)
            if (o == prototype.as.object)
                return js_bool(true);
        return js_bool(false);
    }
    default:
        return js_undefined();
    }
}

/* Ermittelt Ziel und Schluessel einer Zuweisung. */
struct place {
    bool               is_binding;
    struct js_binding *binding;
    struct js_value    target;
    char               key[128];
};

static bool resolve_place(struct js_context *ctx, struct ast *node,
                          struct js_scope *scope, struct place *out)
{
    memset(out, 0, sizeof(*out));

    if (node->kind == AST_IDENT) {
        struct js_binding *b = js_lookup(scope, node->text);

        if (!b) {
            /* Ohne Deklaration entsteht eine globale Bindung. */
            js_declare(ctx, ctx->global_scope, node->text, js_undefined(),
                       false);
            b = js_lookup(scope, node->text);
            if (!b)
                b = js_lookup(ctx->global_scope, node->text);
        }
        out->is_binding = true;
        out->binding = b;
        return b != NULL;
    }
    if (node->kind == AST_MEMBER) {
        out->target = js_eval(ctx, node->a, scope);
        strlcpy(out->key, node->text ? node->text : "", sizeof(out->key));
        return true;
    }
    if (node->kind == AST_INDEX) {
        out->target = js_eval(ctx, node->a, scope);

        struct js_value key = js_eval(ctx, node->b, scope);
        struct js_string *s = js_to_string(ctx, key);

        strlcpy(out->key, s ? s->data : "", sizeof(out->key));
        return true;
    }
    js_throw(ctx, "ungueltiges Ziel einer Zuweisung");
    return false;
}

static struct js_value place_read(struct js_context *ctx, struct place *p)
{
    if (p->is_binding)
        return p->binding ? p->binding->value : js_undefined();
    return js_get_value(ctx, p->target, p->key);
}

static void place_write(struct js_context *ctx, struct place *p,
                        struct js_value value)
{
    if (p->is_binding) {
        if (!p->binding)
            return;
        if (p->binding->constant) {
            js_throw(ctx, "Zuweisung an eine Konstante");
            return;
        }
        p->binding->value = value;
        return;
    }
    js_set_value(ctx, p->target, p->key, value);
    if (p->target.type == JS_OBJECT && p->target.as.object &&
        p->target.as.object->klass == CLASS_NODE)
        ctx->dirty = true;
}

/* Sammelt die Argumente eines Aufrufs; loest ... auf. */
static size_t collect_arguments(struct js_context *ctx, struct ast *list,
                                struct js_scope *scope, struct js_value *out,
                                size_t max)
{
    size_t count = 0;

    for (struct ast *a = list; a && count < max; a = a->next) {
        if (a->kind == AST_SPREAD) {
            struct js_value spread = js_eval(ctx, a->a, scope);

            if (spread.type == JS_OBJECT && spread.as.object &&
                (spread.as.object->klass == CLASS_ARRAY ||
                 spread.as.object->klass == CLASS_NODELIST)) {
                for (size_t i = 0; i < spread.as.object->length && count < max;
                     i++)
                    out[count++] = spread.as.object->elements[i];
            }
            continue;
        }
        out[count++] = js_eval(ctx, a, scope);
        if (ctx->signal == SIGNAL_THROW)
            break;
    }
    return count;
}

struct js_value js_eval(struct js_context *ctx, struct ast *node,
                        struct js_scope *scope)
{
    if (!node || ctx->signal == SIGNAL_THROW || ctx->out_of_memory)
        return js_undefined();
    if (++ctx->steps > ctx->step_limit) {
        js_throw(ctx, "das Skript laeuft zu lange");
        return js_undefined();
    }

    switch (node->kind) {
    case AST_NUMBER:
        return js_number(node->number);
    case AST_STRING:
        return js_str(ctx, node->text);
    case AST_TEMPLATE:
        return eval_template(ctx, node->text, scope);
    case AST_BOOL:
        return js_bool(node->flag);
    case AST_NULL:
        return js_null();
    case AST_UNDEFINED:
        return js_undefined();
    case AST_THIS:
        return ctx->this_value;

    case AST_IDENT: {
        struct js_binding *b = js_lookup(scope, node->text);

        if (b)
            return b->value;

        struct js_value global = js_get(ctx, ctx->global, node->text);

        if (global.type != JS_UNDEFINED)
            return global;
        return js_undefined();
    }

    case AST_ARRAY: {
        struct js_object *array = js_new_array(ctx, 0);

        for (struct ast *item = node->list; item; item = item->next) {
            if (item->kind == AST_SPREAD) {
                struct js_value spread = js_eval(ctx, item->a, scope);

                if (spread.type == JS_OBJECT && spread.as.object &&
                    (spread.as.object->klass == CLASS_ARRAY ||
                     spread.as.object->klass == CLASS_NODELIST))
                    for (size_t i = 0; i < spread.as.object->length; i++)
                        js_array_push(ctx, array,
                                      spread.as.object->elements[i]);
                continue;
            }
            js_array_push(ctx, array, js_eval(ctx, item, scope));
        }
        return js_object_value(array);
    }

    case AST_OBJECT: {
        struct js_object *object = js_new_object(ctx, CLASS_OBJECT);

        for (struct ast *entry = node->list; entry; entry = entry->next) {
            if (entry->kind == AST_SPREAD) {
                struct js_value spread = js_eval(ctx, entry->a, scope);

                if (spread.type == JS_OBJECT && spread.as.object)
                    for (struct js_prop *p = spread.as.object->props; p;
                         p = p->next)
                        js_set(ctx, object, p->name, p->value);
                continue;
            }

            char name[128];

            if (entry->flag && entry->b) {
                struct js_string *key = js_to_string(ctx,
                                                     js_eval(ctx, entry->b,
                                                             scope));

                strlcpy(name, key ? key->data : "", sizeof(name));
            } else {
                strlcpy(name, entry->text ? entry->text : "", sizeof(name));
            }
            js_set(ctx, object, name, js_eval(ctx, entry->a, scope));
        }
        return js_object_value(object);
    }

    case AST_FUNCTION:
        return make_function(ctx, node, scope, false);
    case AST_ARROW:
        return make_function(ctx, node, scope, true);

    case AST_MEMBER: {
        struct js_value target = js_eval(ctx, node->a, scope);

        if (node->flag && (target.type == JS_NULL ||
                           target.type == JS_UNDEFINED))
            return js_undefined();
        return js_get_value(ctx, target, node->text ? node->text : "");
    }

    case AST_INDEX: {
        struct js_value target = js_eval(ctx, node->a, scope);
        struct js_value key = js_eval(ctx, node->b, scope);
        struct js_string *s = js_to_string(ctx, key);

        return js_get_value(ctx, target, s ? s->data : "");
    }

    case AST_CALL: {
        struct js_value self = js_undefined();
        struct js_value callee;

        if (node->a && (node->a->kind == AST_MEMBER ||
                        node->a->kind == AST_INDEX)) {
            self = js_eval(ctx, node->a->a, scope);
            if (node->a->flag && (self.type == JS_NULL ||
                                  self.type == JS_UNDEFINED))
                return js_undefined();

            if (node->a->kind == AST_MEMBER) {
                callee = js_get_value(ctx, self,
                                      node->a->text ? node->a->text : "");
            } else {
                struct js_string *key = js_to_string(ctx,
                                                     js_eval(ctx, node->a->b,
                                                             scope));

                callee = js_get_value(ctx, self, key ? key->data : "");
            }
        } else {
            callee = js_eval(ctx, node->a, scope);
        }

        if (node->flag && (callee.type == JS_NULL ||
                           callee.type == JS_UNDEFINED))
            return js_undefined();

        struct js_value args[24];
        size_t count = collect_arguments(ctx, node->list, scope, args,
                                         ARRAY_LEN(args));

        if (ctx->signal == SIGNAL_THROW)
            return js_undefined();
        return js_call(ctx, callee, self, args, count);
    }

    case AST_NEW: {
        struct js_value callee = js_eval(ctx, node->a, scope);

        if (callee.type != JS_OBJECT || !callee.as.object) {
            js_throw(ctx, "new auf etwas, das keine Funktion ist");
            return js_undefined();
        }

        struct js_value args[24];
        size_t count = collect_arguments(ctx, node->list, scope, args,
                                         ARRAY_LEN(args));

        /* Eingebaute Erzeuger bauen ihr Ergebnis selbst. */
        if (callee.as.object->klass == CLASS_NATIVE)
            return js_call(ctx, callee, js_undefined(), args, count);

        struct js_object *instance = js_new_object(ctx, CLASS_OBJECT);
        struct js_value prototype = js_get(ctx, callee.as.object, "prototype");

        if (prototype.type == JS_OBJECT)
            instance->prototype = prototype.as.object;

        struct js_value self = js_object_value(instance);
        struct js_value result = js_call(ctx, callee, self, args, count);

        if (result.type == JS_OBJECT)
            return result;
        return self;
    }

    case AST_UNARY: {
        if (node->op == OP_TYPEOF && node->a && node->a->kind == AST_IDENT &&
            !js_lookup(scope, node->a->text)) {
            struct js_value global = js_get(ctx, ctx->global, node->a->text);

            return js_str(ctx, js_typeof(global));
        }
        if (node->op == OP_DELETE) {
            struct place p;

            if (node->a->kind == AST_MEMBER || node->a->kind == AST_INDEX) {
                if (!resolve_place(ctx, node->a, scope, &p))
                    return js_bool(false);
                if (p.target.type == JS_OBJECT && p.target.as.object)
                    return js_bool(js_delete(p.target.as.object, p.key));
            }
            return js_bool(true);
        }

        struct js_value value = js_eval(ctx, node->a, scope);

        switch (node->op) {
        case OP_NOT:
            return js_bool(!js_truthy(value));
        case OP_NEGATE: {
            js_num n = js_to_number(ctx, value);

            if (js_is_nan(n))
                return js_number(JS_NAN);
            if (n == JS_POS_INF)
                return js_number(JS_NEG_INF);
            if (n == JS_NEG_INF)
                return js_number(JS_POS_INF);
            return js_number(-n);
        }
        case OP_PLUS:
            return js_number(js_to_number(ctx, value));
        case OP_BITNOT:
            return js_integer(~to_int32(ctx, value));
        case OP_TYPEOF:
            return js_str(ctx, js_typeof(value));
        case OP_VOID:
            return js_undefined();
        default:
            return js_undefined();
        }
    }

    case AST_UPDATE: {
        struct place p;

        if (!resolve_place(ctx, node->a, scope, &p))
            return js_undefined();

        js_num old = js_to_number(ctx, place_read(ctx, &p));
        js_num updated = node->op == OP_INCREMENT
                         ? js_add_num(old, JS_ONE)
                         : js_sub_num(old, JS_ONE);

        place_write(ctx, &p, js_number(updated));
        return js_number(node->flag ? updated : old);
    }

    case AST_BINARY: {
        struct js_value a = js_eval(ctx, node->a, scope);
        struct js_value b = js_eval(ctx, node->b, scope);

        return eval_binary(ctx, node->op, a, b);
    }

    case AST_LOGICAL: {
        struct js_value a = js_eval(ctx, node->a, scope);

        if (node->op == OP_LOGICAL_AND)
            return js_truthy(a) ? js_eval(ctx, node->b, scope) : a;
        if (node->op == OP_LOGICAL_OR)
            return js_truthy(a) ? a : js_eval(ctx, node->b, scope);
        /* Der Fragezeichen-Operator prueft nur auf null und undefined. */
        if (a.type == JS_NULL || a.type == JS_UNDEFINED)
            return js_eval(ctx, node->b, scope);
        return a;
    }

    case AST_CONDITIONAL:
        return js_truthy(js_eval(ctx, node->a, scope))
               ? js_eval(ctx, node->b, scope)
               : js_eval(ctx, node->c, scope);

    case AST_ASSIGN: {
        struct place p;

        if (!resolve_place(ctx, node->a, scope, &p))
            return js_undefined();

        struct js_value value;

        if (node->op == OP_NONE) {
            value = js_eval(ctx, node->b, scope);
        } else if (node->op == OP_LOGICAL_AND || node->op == OP_LOGICAL_OR ||
                   node->op == OP_NULLISH) {
            struct js_value current = place_read(ctx, &p);
            bool assign;

            if (node->op == OP_LOGICAL_AND)
                assign = js_truthy(current);
            else if (node->op == OP_LOGICAL_OR)
                assign = !js_truthy(current);
            else
                assign = current.type == JS_NULL ||
                         current.type == JS_UNDEFINED;

            if (!assign)
                return current;
            value = js_eval(ctx, node->b, scope);
        } else {
            value = eval_binary(ctx, node->op, place_read(ctx, &p),
                                js_eval(ctx, node->b, scope));
        }

        place_write(ctx, &p, value);
        return value;
    }

    case AST_SEQUENCE: {
        struct js_value value = js_undefined();

        for (struct ast *item = node->list; item; item = item->next)
            value = js_eval(ctx, item, scope);
        return value;
    }

    case AST_SPREAD:
        return js_eval(ctx, node->a, scope);

    default:
        return js_undefined();
    }
}

/* ------------------------------------------------------------------ */
/* Anweisungen                                                         */
/* ------------------------------------------------------------------ */

static void exec_statement(struct js_context *ctx, struct ast *node,
                           struct js_scope *scope);

/* Hebt Funktionsdeklarationen an den Anfang, wie es JavaScript tut. */
static void hoist(struct js_context *ctx, struct ast *list,
                  struct js_scope *scope)
{
    for (struct ast *s = list; s; s = s->next) {
        if (s->kind == AST_FUNCTION && !s->flag && s->text)
            js_declare(ctx, scope, s->text,
                       make_function(ctx, s, scope, false), false);
        else if (s->kind == AST_VAR)
            for (struct ast *e = s->list; e; e = e->next)
                if (e->text && !js_lookup(scope, e->text))
                    js_declare(ctx, scope, e->text, js_undefined(), false);
    }
}

void js_exec_block(struct js_context *ctx, struct ast *list,
                   struct js_scope *scope)
{
    hoist(ctx, list, scope);
    for (struct ast *s = list; s; s = s->next) {
        exec_statement(ctx, s, scope);
        if (ctx->signal != SIGNAL_NONE || ctx->out_of_memory)
            return;
    }
}

/* Verteilt die Felder einer zerlegenden Zuweisung. */
static void destructure(struct js_context *ctx, struct ast *entry,
                        struct js_value value, struct js_scope *scope,
                        bool constant)
{
    size_t index = 0;

    for (struct ast *name = entry->list; name; name = name->next, index++) {
        struct js_value item;

        if (entry->kind == AST_ARRAY) {
            char key[24];

            ksnprintf(key, sizeof(key), "%lu", (unsigned long)index);
            item = js_get_value(ctx, value, key);
        } else {
            item = js_get_value(ctx, value, name->text);
        }

        const char *target = name->a && name->a->text ? name->a->text
                                                      : name->text;

        js_declare(ctx, scope, target, item, constant);
    }
}

static void exec_var(struct js_context *ctx, struct ast *node,
                     struct js_scope *scope)
{
    for (struct ast *entry = node->list; entry; entry = entry->next) {
        struct js_value value = entry->b ? js_eval(ctx, entry->b, scope)
                                         : js_undefined();

        if (ctx->signal != SIGNAL_NONE)
            return;
        if (entry->kind == AST_ARRAY || entry->kind == AST_OBJECT)
            destructure(ctx, entry, value, scope, node->flag);
        else
            js_declare(ctx, scope, entry->text, value, node->flag);
    }
}

/* Prueft, ob ein Abbruch diese Schleife meint. */
static bool signal_targets_us(struct js_context *ctx, const char *label)
{
    if (!ctx->signal_label)
        return true;
    if (label && strcmp(ctx->signal_label, label) == 0) {
        ctx->signal_label = NULL;
        return true;
    }
    return false;
}

static void exec_loop_body(struct js_context *ctx, struct ast *body,
                           struct js_scope *scope, const char *label,
                           bool *stop)
{
    exec_statement(ctx, body, scope);

    if (ctx->signal == SIGNAL_BREAK) {
        if (signal_targets_us(ctx, label)) {
            ctx->signal = SIGNAL_NONE;
            *stop = true;
        } else {
            *stop = true;    /* gilt einer aeusseren Schleife */
        }
        return;
    }
    if (ctx->signal == SIGNAL_CONTINUE) {
        if (signal_targets_us(ctx, label))
            ctx->signal = SIGNAL_NONE;
        else
            *stop = true;
        return;
    }
    if (ctx->signal != SIGNAL_NONE)
        *stop = true;
}

static void exec_for_in(struct js_context *ctx, struct ast *node,
                        struct js_scope *scope, const char *label, bool of)
{
    struct js_value source = js_eval(ctx, node->b, scope);

    if (ctx->signal != SIGNAL_NONE)
        return;

    const char *name = NULL;

    if (node->a) {
        if (node->a->kind == AST_VAR && node->a->list)
            name = node->a->list->text;
        else if (node->a->kind == AST_IDENT)
            name = node->a->text;
        else if (node->a->kind == AST_EXPRESSION && node->a->a)
            name = node->a->a->text;
    }
    if (!name)
        return;

    bool stop = false;

    /* Zeichenketten und Felder liefern ihre Elemente. */
    if (of && source.type == JS_STRING && source.as.string) {
        for (size_t i = 0; i < source.as.string->length && !stop; i++) {
            struct js_scope *inner = js_scope_new(ctx, scope);

            js_declare(ctx, inner, name,
                       js_strn(ctx, source.as.string->data + i, 1), false);
            exec_loop_body(ctx, node->c, inner, label, &stop);
        }
        return;
    }

    if (source.type != JS_OBJECT || !source.as.object)
        return;

    struct js_object *o = source.as.object;

    if (o->klass == CLASS_ARRAY || o->klass == CLASS_NODELIST) {
        size_t count = o->length;

        for (size_t i = 0; i < count && i < o->length && !stop; i++) {
            struct js_scope *inner = js_scope_new(ctx, scope);

            if (of)
                js_declare(ctx, inner, name, o->elements[i], false);
            else {
                char key[24];

                ksnprintf(key, sizeof(key), "%lu", (unsigned long)i);
                js_declare(ctx, inner, name, js_str(ctx, key), false);
            }
            exec_loop_body(ctx, node->c, inner, label, &stop);
        }
        if (!of)
            return;
        return;
    }

    /* Bei einem gewoehnlichen Objekt die Namen durchgehen. Die Liste
     * wird vorher kopiert, damit Aenderungen im Rumpf nicht stoeren. */
    struct js_prop *names[256];
    size_t count = 0;

    for (struct js_prop *p = o->props; p && count < ARRAY_LEN(names);
         p = p->next)
        if (p->enumerable)
            names[count++] = p;

    /* Die Liste steht rueckwaerts - in Einfuegereihenfolge bringen. */
    for (size_t i = 0; i < count / 2; i++) {
        struct js_prop *swap = names[i];

        names[i] = names[count - 1 - i];
        names[count - 1 - i] = swap;
    }

    for (size_t i = 0; i < count && !stop; i++) {
        struct js_scope *inner = js_scope_new(ctx, scope);

        js_declare(ctx, inner, name,
                   of ? names[i]->value : js_str(ctx, names[i]->name), false);
        exec_loop_body(ctx, node->c, inner, label, &stop);
    }
}

static void exec_with_label(struct js_context *ctx, struct ast *node,
                            struct js_scope *scope, const char *label)
{
    switch (node->kind) {
    case AST_FOR: {
        struct js_scope *outer = js_scope_new(ctx, scope);

        if (node->a) {
            if (node->a->kind == AST_VAR)
                exec_var(ctx, node->a, outer);
            else
                js_eval(ctx, node->a, outer);
        }

        bool stop = false;

        for (int64_t guard = 0; !stop && guard < 100000000LL; guard++) {
            if (node->b && !js_truthy(js_eval(ctx, node->b, outer)))
                break;
            if (ctx->signal != SIGNAL_NONE)
                break;

            struct js_scope *inner = js_scope_new(ctx, outer);

            exec_loop_body(ctx, node->d, inner, label, &stop);
            if (stop)
                break;
            if (node->c)
                js_eval(ctx, node->c, outer);
            if (ctx->steps > ctx->step_limit)
                break;
        }
        break;
    }

    case AST_FOR_IN:
        exec_for_in(ctx, node, scope, label, false);
        break;
    case AST_FOR_OF:
        exec_for_in(ctx, node, scope, label, true);
        break;

    case AST_WHILE: {
        bool stop = false;

        for (int64_t guard = 0; !stop && guard < 100000000LL; guard++) {
            if (!js_truthy(js_eval(ctx, node->a, scope)))
                break;
            if (ctx->signal != SIGNAL_NONE)
                break;

            struct js_scope *inner = js_scope_new(ctx, scope);

            exec_loop_body(ctx, node->b, inner, label, &stop);
            if (ctx->steps > ctx->step_limit)
                break;
        }
        break;
    }

    case AST_DO_WHILE: {
        bool stop = false;

        for (int64_t guard = 0; !stop && guard < 100000000LL; guard++) {
            struct js_scope *inner = js_scope_new(ctx, scope);

            exec_loop_body(ctx, node->b, inner, label, &stop);
            if (stop)
                break;
            if (!js_truthy(js_eval(ctx, node->a, scope)))
                break;
            if (ctx->signal != SIGNAL_NONE)
                break;
            if (ctx->steps > ctx->step_limit)
                break;
        }
        break;
    }

    case AST_SWITCH: {
        struct js_value value = js_eval(ctx, node->a, scope);
        struct js_scope *inner = js_scope_new(ctx, scope);
        bool running = false;

        for (int32_t pass = 0; pass < 2 && !running; pass++) {
            for (struct ast *branch = node->list; branch;
                 branch = branch->next) {
                if (!running) {
                    if (pass == 0) {
                        if (branch->flag)
                            continue;

                        struct js_value test = js_eval(ctx, branch->a, inner);

                        if (!js_equals(ctx, value, test, true))
                            continue;
                    } else {
                        if (!branch->flag)
                            continue;
                    }
                    running = true;
                }
                js_exec_block(ctx, branch->list, inner);
                if (ctx->signal == SIGNAL_BREAK &&
                    signal_targets_us(ctx, label)) {
                    ctx->signal = SIGNAL_NONE;
                    return;
                }
                if (ctx->signal != SIGNAL_NONE)
                    return;
            }
        }
        break;
    }

    default:
        exec_statement(ctx, node, scope);
        break;
    }
}

static void exec_statement(struct js_context *ctx, struct ast *node,
                           struct js_scope *scope)
{
    if (!node || ctx->signal != SIGNAL_NONE || ctx->out_of_memory)
        return;
    if (++ctx->steps > ctx->step_limit) {
        js_throw(ctx, "das Skript laeuft zu lange");
        return;
    }

    switch (node->kind) {
    case AST_PROGRAM:
    case AST_BLOCK: {
        struct js_scope *inner = node->kind == AST_PROGRAM
                                 ? scope : js_scope_new(ctx, scope);

        js_exec_block(ctx, node->list, inner);
        break;
    }

    case AST_VAR:
        exec_var(ctx, node, scope);
        break;

    case AST_FUNCTION:
        if (!node->flag && node->text &&
            !js_lookup(scope, node->text))
            js_declare(ctx, scope, node->text,
                       make_function(ctx, node, scope, false), false);
        break;

    case AST_EXPRESSION:
        js_eval(ctx, node->a, scope);
        break;

    case AST_IF:
        if (js_truthy(js_eval(ctx, node->a, scope)))
            exec_statement(ctx, node->b, scope);
        else if (node->c)
            exec_statement(ctx, node->c, scope);
        break;

    case AST_RETURN:
        ctx->signal_value = node->a ? js_eval(ctx, node->a, scope)
                                    : js_undefined();
        if (ctx->signal == SIGNAL_NONE)
            ctx->signal = SIGNAL_RETURN;
        break;

    case AST_BREAK:
        ctx->signal = SIGNAL_BREAK;
        ctx->signal_label = node->text;
        break;

    case AST_CONTINUE:
        ctx->signal = SIGNAL_CONTINUE;
        ctx->signal_label = node->text;
        break;

    case AST_THROW:
        ctx->signal_value = js_eval(ctx, node->a, scope);
        ctx->signal = SIGNAL_THROW;
        if (!ctx->failed) {
            struct js_string *s = js_to_string(ctx, ctx->signal_value);

            strlcpy(ctx->error, s ? s->data : "Ausnahme", sizeof(ctx->error));
        }
        break;

    case AST_TRY: {
        struct js_scope *inner = js_scope_new(ctx, scope);

        exec_statement(ctx, node->a, inner);

        if (ctx->signal == SIGNAL_THROW && node->b) {
            struct js_value thrown = ctx->signal_value;

            ctx->signal = SIGNAL_NONE;
            ctx->failed = false;
            ctx->error[0] = '\0';

            struct js_scope *handler = js_scope_new(ctx, scope);

            if (node->text)
                js_declare(ctx, handler, node->text, thrown, false);
            exec_statement(ctx, node->b, handler);
        }

        if (node->c) {
            enum js_signal saved = ctx->signal;
            struct js_value saved_value = ctx->signal_value;

            ctx->signal = SIGNAL_NONE;
            exec_statement(ctx, node->c, js_scope_new(ctx, scope));
            if (ctx->signal == SIGNAL_NONE) {
                ctx->signal = saved;
                ctx->signal_value = saved_value;
            }
        }
        break;
    }

    case AST_LABEL:
        exec_with_label(ctx, node->a, scope, node->text);
        if (ctx->signal == SIGNAL_BREAK && ctx->signal_label &&
            node->text && strcmp(ctx->signal_label, node->text) == 0) {
            ctx->signal = SIGNAL_NONE;
            ctx->signal_label = NULL;
        }
        break;

    case AST_EMPTY:
        break;

    default:
        exec_with_label(ctx, node, scope, NULL);
        break;
    }
}
