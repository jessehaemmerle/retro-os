/* jsvalue.c - Werte, Objekte und der Sammelbereich des Deuters. */

#include "jsint.h"
#include "kstring.h"
#include "mm.h"

/* ------------------------------------------------------------------ */
/* Sammelbereich                                                       */
/* ------------------------------------------------------------------ */

/* Alles wird aus grossen Bloecken bedient und am Ende gemeinsam
 * freigegeben. Das ist schnell und kann nichts vergessen. */
void *js_alloc(struct js_context *ctx, size_t size)
{
    size = ALIGN_UP(size, 16);

    if (ctx->used + size > ctx->capacity) {
        size_t want = MAX(size + 64, (size_t)128 * 1024);
        struct js_arena *block = kmalloc(sizeof(*block) + want);

        if (!block) {
            ctx->out_of_memory = true;
            return NULL;
        }
        block->next = ctx->arenas;
        ctx->arenas = block;
        ctx->base = block->data;
        ctx->used = 0;
        ctx->capacity = want;
        ctx->total += want;
    }

    void *p = ctx->base + ctx->used;

    ctx->used += size;
    memset(p, 0, size);
    return p;
}

char *js_strdup(struct js_context *ctx, const char *s)
{
    if (!s)
        s = "";

    size_t length = strlen(s);
    char *copy = js_alloc(ctx, length + 1);

    if (copy)
        memcpy(copy, s, length + 1);
    return copy;
}

/* ------------------------------------------------------------------ */
/* Zeichenketten                                                       */
/* ------------------------------------------------------------------ */

struct js_string *js_string_new(struct js_context *ctx, const char *data,
                                size_t length)
{
    struct js_string *s = js_alloc(ctx, sizeof(*s) + length + 1);

    if (!s)
        return NULL;
    s->length = length;
    if (data && length)
        memcpy(s->data, data, length);
    s->data[length] = '\0';
    return s;
}

struct js_value js_str(struct js_context *ctx, const char *text)
{
    struct js_value v;

    v.type = JS_STRING;
    v.as.string = js_string_new(ctx, text, text ? strlen(text) : 0);
    if (!v.as.string)
        return js_undefined();
    return v;
}

struct js_value js_strn(struct js_context *ctx, const char *text, size_t length)
{
    struct js_value v;

    v.type = JS_STRING;
    v.as.string = js_string_new(ctx, text, length);
    if (!v.as.string)
        return js_undefined();
    return v;
}

/* ------------------------------------------------------------------ */
/* Einfache Werte                                                      */
/* ------------------------------------------------------------------ */

struct js_value js_undefined(void)
{
    struct js_value v = { JS_UNDEFINED, { .number = 0 } };

    return v;
}

struct js_value js_null(void)
{
    struct js_value v = { JS_NULL, { .number = 0 } };

    return v;
}

struct js_value js_bool(bool value)
{
    struct js_value v;

    v.type = JS_BOOL;
    v.as.boolean = value;
    return v;
}

struct js_value js_number(js_num value)
{
    struct js_value v;

    v.type = JS_NUMBER;
    v.as.number = value;
    return v;
}

struct js_value js_integer(int64_t value)
{
    return js_number(js_from_int(value));
}

struct js_value js_object_value(struct js_object *object)
{
    struct js_value v;

    if (!object)
        return js_undefined();
    v.type = JS_OBJECT;
    v.as.object = object;
    return v;
}

/* ------------------------------------------------------------------ */
/* Festkommazahlen                                                     */
/* ------------------------------------------------------------------ */

js_num js_from_int(int64_t value)
{
    if (value > (INT64_MAX >> JS_FRACTION) - 2)
        return JS_POS_INF;
    if (value < (INT64_MIN >> JS_FRACTION) + 2)
        return JS_NEG_INF;
    return value << JS_FRACTION;
}

int64_t js_to_int(js_num value)
{
    if (js_is_nan(value))
        return 0;
    if (value == JS_POS_INF)
        return INT64_MAX >> JS_FRACTION;
    if (value == JS_NEG_INF)
        return INT64_MIN >> JS_FRACTION;
    /* Richtung null abschneiden, wie es JavaScript tut. */
    if (value < 0)
        return -((-value) >> JS_FRACTION);
    return value >> JS_FRACTION;
}

bool js_is_nan(js_num value)
{
    return value == JS_NAN;
}

bool js_is_finite(js_num value)
{
    return value != JS_NAN && value != JS_POS_INF && value != JS_NEG_INF;
}

js_num js_add_num(js_num a, js_num b)
{
    if (js_is_nan(a) || js_is_nan(b))
        return JS_NAN;
    if (!js_is_finite(a) || !js_is_finite(b)) {
        if (a == JS_POS_INF && b == JS_NEG_INF)
            return JS_NAN;
        if (a == JS_NEG_INF && b == JS_POS_INF)
            return JS_NAN;
        return js_is_finite(a) ? b : a;
    }

    __int128 sum = (__int128)a + b;

    if (sum > (__int128)INT64_MAX - 2)
        return JS_POS_INF;
    if (sum < (__int128)INT64_MIN + 2)
        return JS_NEG_INF;
    return (js_num)sum;
}

js_num js_sub_num(js_num a, js_num b)
{
    if (js_is_nan(a) || js_is_nan(b))
        return JS_NAN;
    if (b == JS_POS_INF)
        return js_add_num(a, JS_NEG_INF);
    if (b == JS_NEG_INF)
        return js_add_num(a, JS_POS_INF);
    return js_add_num(a, -b);
}

js_num js_mul_num(js_num a, js_num b)
{
    if (js_is_nan(a) || js_is_nan(b))
        return JS_NAN;
    if (!js_is_finite(a) || !js_is_finite(b)) {
        if (a == 0 || b == 0)
            return JS_NAN;
        bool negative = (a < 0) != (b < 0);

        return negative ? JS_NEG_INF : JS_POS_INF;
    }

    __int128 product = ((__int128)a * b) >> JS_FRACTION;

    if (product > (__int128)INT64_MAX - 2)
        return JS_POS_INF;
    if (product < (__int128)INT64_MIN + 2)
        return JS_NEG_INF;
    return (js_num)product;
}

js_num js_div_num(js_num a, js_num b)
{
    if (js_is_nan(a) || js_is_nan(b))
        return JS_NAN;
    if (b == 0) {
        if (a == 0)
            return JS_NAN;
        return a > 0 ? JS_POS_INF : JS_NEG_INF;
    }
    if (!js_is_finite(a) && !js_is_finite(b))
        return JS_NAN;
    if (!js_is_finite(a))
        return (a > 0) == (b > 0) ? JS_POS_INF : JS_NEG_INF;
    if (!js_is_finite(b))
        return 0;

    __int128 quotient = ((__int128)a << JS_FRACTION) / b;

    if (quotient > (__int128)INT64_MAX - 2)
        return JS_POS_INF;
    if (quotient < (__int128)INT64_MIN + 2)
        return JS_NEG_INF;
    return (js_num)quotient;
}

js_num js_mod_num(js_num a, js_num b)
{
    if (js_is_nan(a) || js_is_nan(b) || b == 0 || !js_is_finite(a))
        return JS_NAN;
    if (!js_is_finite(b))
        return a;
    return a % b;
}

/* ------------------------------------------------------------------ */
/* Objekte                                                             */
/* ------------------------------------------------------------------ */

struct js_object *js_new_object(struct js_context *ctx, enum js_class klass)
{
    struct js_object *o = js_alloc(ctx, sizeof(*o));

    if (!o)
        return NULL;
    o->klass = klass;
    if (klass == CLASS_ARRAY)
        o->prototype = ctx->array_prototype;
    else if (klass == CLASS_FUNCTION || klass == CLASS_NATIVE)
        o->prototype = ctx->function_prototype;
    else
        o->prototype = ctx->object_prototype;
    return o;
}

struct js_object *js_new_array(struct js_context *ctx, size_t length)
{
    struct js_object *o = js_new_object(ctx, CLASS_ARRAY);

    if (!o)
        return NULL;
    if (length) {
        o->elements = js_alloc(ctx, length * sizeof(struct js_value));
        if (!o->elements)
            return o;
        o->capacity = length;
        o->length = length;
        for (size_t i = 0; i < length; i++)
            o->elements[i] = js_undefined();
    }
    return o;
}

bool js_array_push(struct js_context *ctx, struct js_object *array,
                   struct js_value value)
{
    if (array->length == array->capacity) {
        size_t want = array->capacity ? array->capacity * 2 : 8;
        struct js_value *bigger = js_alloc(ctx,
                                           want * sizeof(struct js_value));

        if (!bigger)
            return false;
        if (array->elements)
            memcpy(bigger, array->elements,
                   array->length * sizeof(struct js_value));
        array->elements = bigger;
        array->capacity = want;
    }
    array->elements[array->length++] = value;
    return true;
}

bool js_array_set(struct js_context *ctx, struct js_object *array,
                  size_t index, struct js_value value)
{
    if (index >= 1u << 22)
        return false;

    while (array->length <= index)
        if (!js_array_push(ctx, array, js_undefined()))
            return false;
    array->elements[index] = value;
    return true;
}

/* ------------------------------------------------------------------ */
/* Eigenschaften                                                       */
/* ------------------------------------------------------------------ */

static struct js_prop *find_own(struct js_object *object, const char *name)
{
    for (struct js_prop *p = object->props; p; p = p->next)
        if (strcmp(p->name, name) == 0)
            return p;
    return NULL;
}

struct js_value *js_own_slot(struct js_object *object, const char *name)
{
    struct js_prop *p = find_own(object, name);

    return p ? &p->value : NULL;
}

void js_set(struct js_context *ctx, struct js_object *object, const char *name,
            struct js_value value)
{
    if (!object)
        return;

    struct js_prop *p = find_own(object, name);

    if (p) {
        p->value = value;
        return;
    }

    p = js_alloc(ctx, sizeof(*p));
    if (!p)
        return;
    p->name = js_strdup(ctx, name);
    p->value = value;
    p->enumerable = true;
    p->next = object->props;
    object->props = p;
}

void js_set_hidden(struct js_context *ctx, struct js_object *object,
                   const char *name, struct js_value value)
{
    js_set(ctx, object, name, value);

    struct js_prop *p = find_own(object, name);

    if (p)
        p->enumerable = false;
}

void js_set_native(struct js_context *ctx, struct js_object *object,
                   const char *name, js_native fn, int32_t arity)
{
    struct js_object *f = js_new_object(ctx, CLASS_NATIVE);

    if (!f)
        return;
    f->native = fn;
    f->name = js_strdup(ctx, name);
    f->arity = arity;
    js_set_hidden(ctx, object, name, js_object_value(f));
}

struct js_value js_get(struct js_context *ctx, struct js_object *object,
                       const char *name)
{
    for (struct js_object *o = object; o; o = o->prototype) {
        struct js_prop *p = find_own(o, name);

        if (p)
            return p->value;
        if (o->getter) {
            bool handled = false;
            struct js_value v = o->getter(ctx, o, name, &handled);

            if (handled)
                return v;
        }
    }
    return js_undefined();
}

bool js_delete(struct js_object *object, const char *name)
{
    struct js_prop **link = &object->props;

    while (*link) {
        if (strcmp((*link)->name, name) == 0) {
            *link = (*link)->next;
            return true;
        }
        link = &(*link)->next;
    }
    return false;
}
