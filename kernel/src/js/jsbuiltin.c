/* jsbuiltin.c - die eingebaute Bibliothek: Object, Array, String,
 * Number, Math, JSON, console und die Zeitgeber. */

#include "jsint.h"
#include "kstring.h"
#include "rtc.h"
#include "arch.h"

#define ARG(i) ((i) < count ? args[i] : js_undefined())

static struct js_string *arg_string(struct js_context *ctx,
                                    struct js_value *args, size_t count,
                                    size_t index)
{
    return js_to_string(ctx, index < count ? args[index] : js_undefined());
}

static int64_t arg_int(struct js_context *ctx, struct js_value *args,
                       size_t count, size_t index, int64_t fallback)
{
    if (index >= count || args[index].type == JS_UNDEFINED)
        return fallback;

    js_num n = js_to_number(ctx, args[index]);

    if (js_is_nan(n))
        return fallback;
    return js_to_int(n);
}

/* ------------------------------------------------------------------ */
/* console                                                             */
/* ------------------------------------------------------------------ */

void js_console_write(struct js_context *ctx, const char *text)
{
    size_t length = strlen(text);

    if (ctx->console_used + length + 2 >= JS_CONSOLE_SIZE) {
        /* Die aeltere Haelfte wegwerfen, damit Neues Platz hat. */
        size_t drop = ctx->console_used / 2;

        memmove(ctx->console, ctx->console + drop, ctx->console_used - drop);
        ctx->console_used -= drop;
    }
    if (ctx->console_used + length + 2 >= JS_CONSOLE_SIZE)
        length = JS_CONSOLE_SIZE - ctx->console_used - 2;

    memcpy(ctx->console + ctx->console_used, text, length);
    ctx->console_used += length;
    ctx->console[ctx->console_used++] = '\n';
    ctx->console[ctx->console_used] = '\0';
}

static struct js_value native_log(struct js_context *ctx, struct js_value self,
                                  struct js_value *args, size_t count)
{
    char line[512];
    size_t at = 0;

    UNUSED(self);
    line[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        struct js_string *s = js_to_string(ctx, args[i]);

        if (i && at + 1 < sizeof(line))
            line[at++] = ' ';
        if (s) {
            size_t take = MIN(s->length, sizeof(line) - at - 1);

            memcpy(line + at, s->data, take);
            at += take;
        }
        line[at] = '\0';
    }
    js_console_write(ctx, line);
    kprintf("Skript      : %s\n", line);
    return js_undefined();
}

/* ------------------------------------------------------------------ */
/* Zeitgeber                                                           */
/* ------------------------------------------------------------------ */

static struct js_value native_set_timeout(struct js_context *ctx,
                                          struct js_value self,
                                          struct js_value *args, size_t count)
{
    UNUSED(self);
    if (count == 0)
        return js_integer(0);

    int64_t delay = arg_int(ctx, args, count, 1, 0);

    for (size_t i = 0; i < JS_TIMERS; i++) {
        if (ctx->timers[i].active)
            continue;
        ctx->timers[i].active = true;
        ctx->timers[i].repeating = false;
        ctx->timers[i].period = (uint64_t)MAX(delay, 0);
        ctx->timers[i].due = timer_ms() + (uint64_t)MAX(delay, 0);
        ctx->timers[i].callback = args[0];
        ctx->timers[i].id = ++ctx->next_timer_id;
        return js_integer(ctx->timers[i].id);
    }
    return js_integer(0);
}

static struct js_value native_set_interval(struct js_context *ctx,
                                           struct js_value self,
                                           struct js_value *args, size_t count)
{
    struct js_value id = native_set_timeout(ctx, self, args, count);

    for (size_t i = 0; i < JS_TIMERS; i++) {
        if (ctx->timers[i].active &&
            ctx->timers[i].id == js_to_int(id.as.number)) {
            ctx->timers[i].repeating = true;
            /* Ein Takt unter zehn Millisekunden wuerde alles lahmlegen. */
            if (ctx->timers[i].period < 10)
                ctx->timers[i].period = 10;
        }
    }
    return id;
}

static struct js_value native_clear_timer(struct js_context *ctx,
                                          struct js_value self,
                                          struct js_value *args, size_t count)
{
    int64_t id = arg_int(ctx, args, count, 0, -1);

    UNUSED(self);
    for (size_t i = 0; i < JS_TIMERS; i++)
        if (ctx->timers[i].active && ctx->timers[i].id == id)
            ctx->timers[i].active = false;
    return js_undefined();
}

bool js_run_timers(struct js_context *ctx, uint64_t now_ms)
{
    bool ran = false;

    if (!ctx)
        return false;

    for (size_t i = 0; i < JS_TIMERS; i++) {
        struct js_timer *t = &ctx->timers[i];

        if (!t->active || now_ms < t->due)
            continue;

        struct js_value callback = t->callback;

        if (t->repeating)
            t->due = now_ms + MAX(t->period, 10u);
        else
            t->active = false;

        ctx->signal = SIGNAL_NONE;
        ctx->failed = false;
        ctx->steps = 0;
        js_call(ctx, callback, js_undefined(), NULL, 0);
        ctx->signal = SIGNAL_NONE;
        ran = true;
    }

    /* Was der Zeitgeber am Baum geaendert hat, muss der Browser
     * erfahren - sonst bliebe es unsichtbar. */
    if (ran)
        js_flush_changes(ctx);
    return ran;
}

/* ------------------------------------------------------------------ */
/* Object                                                              */
/* ------------------------------------------------------------------ */

static struct js_value native_object_keys(struct js_context *ctx,
                                          struct js_value self,
                                          struct js_value *args, size_t count)
{
    struct js_object *out = js_new_array(ctx, 0);
    struct js_value target = ARG(0);

    UNUSED(self);
    if (target.type != JS_OBJECT || !target.as.object)
        return js_object_value(out);

    struct js_object *o = target.as.object;

    if (o->klass == CLASS_ARRAY || o->klass == CLASS_NODELIST) {
        for (size_t i = 0; i < o->length; i++) {
            char key[24];

            ksnprintf(key, sizeof(key), "%lu", (unsigned long)i);
            js_array_push(ctx, out, js_str(ctx, key));
        }
        return js_object_value(out);
    }

    /* Die Liste steht rueckwaerts; wir drehen sie um. */
    struct js_prop *names[256];
    size_t found = 0;

    for (struct js_prop *p = o->props; p && found < ARRAY_LEN(names);
         p = p->next)
        if (p->enumerable)
            names[found++] = p;
    while (found > 0)
        js_array_push(ctx, out, js_str(ctx, names[--found]->name));
    return js_object_value(out);
}

static struct js_value native_object_values(struct js_context *ctx,
                                            struct js_value self,
                                            struct js_value *args, size_t count)
{
    struct js_value keys = native_object_keys(ctx, self, args, count);
    struct js_object *out = js_new_array(ctx, 0);
    struct js_value target = ARG(0);

    for (size_t i = 0; i < keys.as.object->length; i++) {
        struct js_string *key = keys.as.object->elements[i].as.string;

        js_array_push(ctx, out, js_get_value(ctx, target, key->data));
    }
    return js_object_value(out);
}

static struct js_value native_object_entries(struct js_context *ctx,
                                             struct js_value self,
                                             struct js_value *args,
                                             size_t count)
{
    struct js_value keys = native_object_keys(ctx, self, args, count);
    struct js_object *out = js_new_array(ctx, 0);
    struct js_value target = ARG(0);

    for (size_t i = 0; i < keys.as.object->length; i++) {
        struct js_string *key = keys.as.object->elements[i].as.string;
        struct js_object *pair = js_new_array(ctx, 0);

        js_array_push(ctx, pair, keys.as.object->elements[i]);
        js_array_push(ctx, pair, js_get_value(ctx, target, key->data));
        js_array_push(ctx, out, js_object_value(pair));
    }
    return js_object_value(out);
}

static struct js_value native_object_assign(struct js_context *ctx,
                                            struct js_value self,
                                            struct js_value *args, size_t count)
{
    struct js_value target = ARG(0);

    UNUSED(self);
    if (target.type != JS_OBJECT || !target.as.object)
        return target;

    for (size_t i = 1; i < count; i++) {
        if (args[i].type != JS_OBJECT || !args[i].as.object)
            continue;
        for (struct js_prop *p = args[i].as.object->props; p; p = p->next)
            js_set(ctx, target.as.object, p->name, p->value);
    }
    return target;
}

static struct js_value native_object_ctor(struct js_context *ctx,
                                          struct js_value self,
                                          struct js_value *args, size_t count)
{
    UNUSED(self);
    if (count > 0 && args[0].type == JS_OBJECT)
        return args[0];
    return js_object_value(js_new_object(ctx, CLASS_OBJECT));
}

static struct js_value native_has_own(struct js_context *ctx,
                                      struct js_value self,
                                      struct js_value *args, size_t count)
{
    struct js_string *key = arg_string(ctx, args, count, 0);

    if (self.type != JS_OBJECT || !self.as.object || !key)
        return js_bool(false);
    if (self.as.object->klass == CLASS_ARRAY) {
        struct js_value v = js_get_value(ctx, self, key->data);

        if (v.type != JS_UNDEFINED)
            return js_bool(true);
    }
    return js_bool(js_own_slot(self.as.object, key->data) != NULL);
}

static struct js_value native_to_string(struct js_context *ctx,
                                        struct js_value self,
                                        struct js_value *args, size_t count)
{
    UNUSED(args);
    UNUSED(count);

    if (self.type == JS_NUMBER) {
        int64_t base = arg_int(ctx, args, count, 0, 10);

        if (base != 10 && base >= 2 && base <= 36) {
            char digits[80];
            size_t at = 0;
            int64_t value = js_to_int(self.as.number);
            bool negative = value < 0;
            uint64_t magnitude = negative ? (uint64_t)(-value)
                                          : (uint64_t)value;

            if (magnitude == 0)
                digits[at++] = '0';
            while (magnitude > 0 && at < sizeof(digits) - 1) {
                uint64_t d = magnitude % (uint64_t)base;

                digits[at++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
                magnitude /= (uint64_t)base;
            }

            char out[82];
            size_t written = 0;

            if (negative)
                out[written++] = '-';
            while (at > 0)
                out[written++] = digits[--at];
            out[written] = '\0';
            return js_str(ctx, out);
        }
    }

    struct js_value v;

    v.type = JS_STRING;
    v.as.string = js_to_string(ctx, self);
    return v;
}

/* ------------------------------------------------------------------ */
/* Array                                                               */
/* ------------------------------------------------------------------ */

static struct js_object *self_array(struct js_value self)
{
    if (self.type != JS_OBJECT || !self.as.object)
        return NULL;
    if (self.as.object->klass != CLASS_ARRAY &&
        self.as.object->klass != CLASS_NODELIST)
        return NULL;
    return self.as.object;
}

static struct js_value native_push(struct js_context *ctx,
                                   struct js_value self,
                                   struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    if (!a)
        return js_integer(0);
    for (size_t i = 0; i < count; i++)
        js_array_push(ctx, a, args[i]);
    return js_integer((int64_t)a->length);
}

static struct js_value native_pop(struct js_context *ctx, struct js_value self,
                                  struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    UNUSED(ctx);
    UNUSED(args);
    UNUSED(count);
    if (!a || a->length == 0)
        return js_undefined();
    return a->elements[--a->length];
}

static struct js_value native_shift(struct js_context *ctx,
                                    struct js_value self,
                                    struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    UNUSED(ctx);
    UNUSED(args);
    UNUSED(count);
    if (!a || a->length == 0)
        return js_undefined();

    struct js_value first = a->elements[0];

    for (size_t i = 1; i < a->length; i++)
        a->elements[i - 1] = a->elements[i];
    a->length--;
    return first;
}

static struct js_value native_unshift(struct js_context *ctx,
                                      struct js_value self,
                                      struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    if (!a)
        return js_integer(0);
    for (size_t i = 0; i < count; i++)
        js_array_push(ctx, a, js_undefined());
    for (size_t i = a->length; i > count; i--)
        a->elements[i - 1] = a->elements[i - 1 - count];
    for (size_t i = 0; i < count; i++)
        a->elements[i] = args[i];
    return js_integer((int64_t)a->length);
}

static struct js_value native_slice(struct js_context *ctx,
                                    struct js_value self,
                                    struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    if (!a) {
        /* Zeichenketten haben ihr eigenes slice. */
        struct js_string *s = js_to_string(ctx, self);
        int64_t length = s ? (int64_t)s->length : 0;
        int64_t from = arg_int(ctx, args, count, 0, 0);
        int64_t to = arg_int(ctx, args, count, 1, length);

        if (from < 0)
            from += length;
        if (to < 0)
            to += length;
        from = CLAMP(from, 0, length);
        to = CLAMP(to, from, length);
        return js_strn(ctx, s->data + from, (size_t)(to - from));
    }

    int64_t length = (int64_t)a->length;
    int64_t from = arg_int(ctx, args, count, 0, 0);
    int64_t to = arg_int(ctx, args, count, 1, length);

    if (from < 0)
        from += length;
    if (to < 0)
        to += length;
    from = CLAMP(from, 0, length);
    to = CLAMP(to, from, length);

    struct js_object *out = js_new_array(ctx, 0);

    for (int64_t i = from; i < to; i++)
        js_array_push(ctx, out, a->elements[i]);
    return js_object_value(out);
}

static struct js_value native_splice(struct js_context *ctx,
                                     struct js_value self,
                                     struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    if (!a)
        return js_object_value(js_new_array(ctx, 0));

    int64_t length = (int64_t)a->length;
    int64_t start = arg_int(ctx, args, count, 0, 0);

    if (start < 0)
        start += length;
    start = CLAMP(start, 0, length);

    int64_t remove = arg_int(ctx, args, count, 1, length - start);

    remove = CLAMP(remove, 0, length - start);

    struct js_object *removed = js_new_array(ctx, 0);

    for (int64_t i = 0; i < remove; i++)
        js_array_push(ctx, removed, a->elements[start + i]);

    size_t insert = count > 2 ? count - 2 : 0;
    int64_t shift = (int64_t)insert - remove;

    if (shift > 0) {
        for (int64_t i = 0; i < shift; i++)
            js_array_push(ctx, a, js_undefined());
        for (int64_t i = length - 1; i >= start + remove; i--)
            a->elements[i + shift] = a->elements[i];
    } else if (shift < 0) {
        for (int64_t i = start + remove; i < length; i++)
            a->elements[i + shift] = a->elements[i];
        a->length = (size_t)(length + shift);
    }
    for (size_t i = 0; i < insert; i++)
        a->elements[start + i] = args[2 + i];
    return js_object_value(removed);
}

static struct js_value native_join(struct js_context *ctx,
                                   struct js_value self,
                                   struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);
    struct js_string *sep = count > 0 && args[0].type != JS_UNDEFINED
                            ? js_to_string(ctx, args[0])
                            : js_string_new(ctx, ",", 1);

    if (!a || !sep)
        return js_str(ctx, "");

    size_t total = 0;

    for (size_t i = 0; i < a->length; i++) {
        if (a->elements[i].type == JS_UNDEFINED ||
            a->elements[i].type == JS_NULL)
            continue;

        struct js_string *s = js_to_string(ctx, a->elements[i]);

        total += s ? s->length : 0;
    }
    total += sep->length * (a->length ? a->length - 1 : 0);

    struct js_string *out = js_string_new(ctx, NULL, total);
    size_t at = 0;

    if (!out)
        return js_str(ctx, "");
    for (size_t i = 0; i < a->length; i++) {
        if (i && at + sep->length <= total) {
            memcpy(out->data + at, sep->data, sep->length);
            at += sep->length;
        }
        if (a->elements[i].type == JS_UNDEFINED ||
            a->elements[i].type == JS_NULL)
            continue;

        struct js_string *s = js_to_string(ctx, a->elements[i]);

        if (s && at + s->length <= total) {
            memcpy(out->data + at, s->data, s->length);
            at += s->length;
        }
    }
    out->data[at] = '\0';
    out->length = at;

    struct js_value v;

    v.type = JS_STRING;
    v.as.string = out;
    return v;
}

static struct js_value native_index_of(struct js_context *ctx,
                                       struct js_value self,
                                       struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    if (a) {
        for (size_t i = 0; i < a->length; i++)
            if (js_equals(ctx, a->elements[i], ARG(0), true))
                return js_integer((int64_t)i);
        return js_integer(-1);
    }

    struct js_string *haystack = js_to_string(ctx, self);
    struct js_string *needle = arg_string(ctx, args, count, 0);
    int64_t from = arg_int(ctx, args, count, 1, 0);

    if (!haystack || !needle || needle->length > haystack->length)
        return js_integer(-1);
    from = CLAMP(from, 0, (int64_t)haystack->length);
    for (size_t i = (size_t)from; i + needle->length <= haystack->length; i++)
        if (memcmp(haystack->data + i, needle->data, needle->length) == 0)
            return js_integer((int64_t)i);
    return js_integer(-1);
}

static struct js_value native_last_index_of(struct js_context *ctx,
                                            struct js_value self,
                                            struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    if (a) {
        for (size_t i = a->length; i > 0; i--)
            if (js_equals(ctx, a->elements[i - 1], ARG(0), true))
                return js_integer((int64_t)(i - 1));
        return js_integer(-1);
    }

    struct js_string *haystack = js_to_string(ctx, self);
    struct js_string *needle = arg_string(ctx, args, count, 0);

    if (!haystack || !needle || needle->length > haystack->length)
        return js_integer(-1);
    for (size_t i = haystack->length - needle->length + 1; i > 0; i--)
        if (memcmp(haystack->data + i - 1, needle->data, needle->length) == 0)
            return js_integer((int64_t)(i - 1));
    return js_integer(-1);
}

static struct js_value native_includes(struct js_context *ctx,
                                       struct js_value self,
                                       struct js_value *args, size_t count)
{
    struct js_value found = native_index_of(ctx, self, args, count);

    return js_bool(js_to_int(found.as.number) >= 0);
}

/* Die Reihe der Funktionen, die eine Rueckruffunktion bekommen. */
enum iterate_kind { IT_FOREACH, IT_MAP, IT_FILTER, IT_FIND, IT_FIND_INDEX,
                    IT_SOME, IT_EVERY };

static struct js_value iterate(struct js_context *ctx, struct js_value self,
                               struct js_value *args, size_t count,
                               enum iterate_kind kind)
{
    struct js_object *a = self_array(self);

    if (!a || count == 0)
        return kind == IT_MAP || kind == IT_FILTER
               ? js_object_value(js_new_array(ctx, 0)) : js_undefined();

    struct js_object *out = (kind == IT_MAP || kind == IT_FILTER)
                            ? js_new_array(ctx, 0) : NULL;
    size_t length = a->length;

    for (size_t i = 0; i < length && i < a->length; i++) {
        struct js_value call_args[3] = {
            a->elements[i], js_integer((int64_t)i), self
        };
        struct js_value result = js_call(ctx, args[0],
                                         count > 1 ? args[1] : js_undefined(),
                                         call_args, 3);

        if (ctx->signal == SIGNAL_THROW)
            break;

        switch (kind) {
        case IT_MAP:
            js_array_push(ctx, out, result);
            break;
        case IT_FILTER:
            if (js_truthy(result))
                js_array_push(ctx, out, a->elements[i]);
            break;
        case IT_FIND:
            if (js_truthy(result))
                return a->elements[i];
            break;
        case IT_FIND_INDEX:
            if (js_truthy(result))
                return js_integer((int64_t)i);
            break;
        case IT_SOME:
            if (js_truthy(result))
                return js_bool(true);
            break;
        case IT_EVERY:
            if (!js_truthy(result))
                return js_bool(false);
            break;
        default:
            break;
        }
    }

    switch (kind) {
    case IT_MAP:
    case IT_FILTER:
        return js_object_value(out);
    case IT_FIND_INDEX:
        return js_integer(-1);
    case IT_SOME:
        return js_bool(false);
    case IT_EVERY:
        return js_bool(true);
    default:
        return js_undefined();
    }
}

static struct js_value native_foreach(struct js_context *ctx,
                                      struct js_value self,
                                      struct js_value *args, size_t count)
{
    return iterate(ctx, self, args, count, IT_FOREACH);
}

static struct js_value native_map(struct js_context *ctx, struct js_value self,
                                  struct js_value *args, size_t count)
{
    return iterate(ctx, self, args, count, IT_MAP);
}

static struct js_value native_filter(struct js_context *ctx,
                                     struct js_value self,
                                     struct js_value *args, size_t count)
{
    return iterate(ctx, self, args, count, IT_FILTER);
}

static struct js_value native_find(struct js_context *ctx, struct js_value self,
                                   struct js_value *args, size_t count)
{
    return iterate(ctx, self, args, count, IT_FIND);
}

static struct js_value native_find_index(struct js_context *ctx,
                                         struct js_value self,
                                         struct js_value *args, size_t count)
{
    return iterate(ctx, self, args, count, IT_FIND_INDEX);
}

static struct js_value native_some(struct js_context *ctx, struct js_value self,
                                   struct js_value *args, size_t count)
{
    return iterate(ctx, self, args, count, IT_SOME);
}

static struct js_value native_every(struct js_context *ctx,
                                    struct js_value self,
                                    struct js_value *args, size_t count)
{
    return iterate(ctx, self, args, count, IT_EVERY);
}

static struct js_value native_reduce(struct js_context *ctx,
                                     struct js_value self,
                                     struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    if (!a || count == 0)
        return js_undefined();

    size_t start = 0;
    struct js_value acc;

    if (count > 1) {
        acc = args[1];
    } else {
        if (a->length == 0)
            return js_undefined();
        acc = a->elements[0];
        start = 1;
    }

    for (size_t i = start; i < a->length; i++) {
        struct js_value call_args[4] = {
            acc, a->elements[i], js_integer((int64_t)i), self
        };

        acc = js_call(ctx, args[0], js_undefined(), call_args, 4);
        if (ctx->signal == SIGNAL_THROW)
            break;
    }
    return acc;
}

static struct js_value native_reverse(struct js_context *ctx,
                                      struct js_value self,
                                      struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    UNUSED(ctx);
    UNUSED(args);
    UNUSED(count);
    if (!a)
        return self;
    for (size_t i = 0; i < a->length / 2; i++) {
        struct js_value swap = a->elements[i];

        a->elements[i] = a->elements[a->length - 1 - i];
        a->elements[a->length - 1 - i] = swap;
    }
    return self;
}

static struct js_value native_sort(struct js_context *ctx, struct js_value self,
                                   struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    if (!a || a->length < 2)
        return self;

    bool custom = count > 0 && args[0].type == JS_OBJECT;

    /* Einfaches Einfuegesortieren; die Listen sind hier kurz. */
    for (size_t i = 1; i < a->length; i++) {
        struct js_value key = a->elements[i];
        size_t j = i;

        while (j > 0) {
            bool greater;

            if (custom) {
                struct js_value pair[2] = { a->elements[j - 1], key };
                struct js_value result = js_call(ctx, args[0], js_undefined(),
                                                 pair, 2);

                greater = js_to_int(js_to_number(ctx, result)) > 0;
            } else {
                struct js_string *left = js_to_string(ctx, a->elements[j - 1]);
                struct js_string *right = js_to_string(ctx, key);
                size_t shortest = MIN(left->length, right->length);
                int order = memcmp(left->data, right->data, shortest);

                if (order == 0)
                    greater = left->length > right->length;
                else
                    greater = order > 0;
            }
            if (!greater)
                break;
            a->elements[j] = a->elements[j - 1];
            j--;
        }
        a->elements[j] = key;
    }
    return self;
}

static struct js_value native_concat(struct js_context *ctx,
                                     struct js_value self,
                                     struct js_value *args, size_t count)
{
    struct js_object *a = self_array(self);

    if (!a) {
        /* Zeichenketten aneinanderhaengen. */
        struct js_value result = self;

        for (size_t i = 0; i < count; i++) {
            struct js_string *left = js_to_string(ctx, result);
            struct js_string *right = js_to_string(ctx, args[i]);
            struct js_string *out = js_string_new(ctx, NULL,
                                                  left->length + right->length);

            memcpy(out->data, left->data, left->length);
            memcpy(out->data + left->length, right->data, right->length);
            result.type = JS_STRING;
            result.as.string = out;
        }
        return result;
    }

    struct js_object *out = js_new_array(ctx, 0);

    for (size_t i = 0; i < a->length; i++)
        js_array_push(ctx, out, a->elements[i]);
    for (size_t i = 0; i < count; i++) {
        struct js_object *other = self_array(args[i]);

        if (other)
            for (size_t j = 0; j < other->length; j++)
                js_array_push(ctx, out, other->elements[j]);
        else
            js_array_push(ctx, out, args[i]);
    }
    return js_object_value(out);
}

static struct js_value native_array_ctor(struct js_context *ctx,
                                         struct js_value self,
                                         struct js_value *args, size_t count)
{
    UNUSED(self);
    if (count == 1 && args[0].type == JS_NUMBER)
        return js_object_value(js_new_array(ctx,
                                            (size_t)js_to_int(args[0].as.number)));

    struct js_object *out = js_new_array(ctx, 0);

    for (size_t i = 0; i < count; i++)
        js_array_push(ctx, out, args[i]);
    return js_object_value(out);
}

static struct js_value native_is_array(struct js_context *ctx,
                                       struct js_value self,
                                       struct js_value *args, size_t count)
{
    UNUSED(ctx);
    UNUSED(self);
    return js_bool(count > 0 && args[0].type == JS_OBJECT &&
                   args[0].as.object &&
                   args[0].as.object->klass == CLASS_ARRAY);
}

static struct js_value native_array_from(struct js_context *ctx,
                                         struct js_value self,
                                         struct js_value *args, size_t count)
{
    struct js_object *out = js_new_array(ctx, 0);
    struct js_value source = ARG(0);

    UNUSED(self);
    if (source.type == JS_STRING && source.as.string) {
        for (size_t i = 0; i < source.as.string->length; i++)
            js_array_push(ctx, out,
                          js_strn(ctx, source.as.string->data + i, 1));
        return js_object_value(out);
    }
    if (source.type == JS_OBJECT && source.as.object) {
        struct js_object *o = source.as.object;

        if (o->klass == CLASS_ARRAY || o->klass == CLASS_NODELIST) {
            for (size_t i = 0; i < o->length; i++)
                js_array_push(ctx, out, o->elements[i]);
        } else {
            int64_t length = js_to_int(js_to_number(ctx,
                                                    js_get(ctx, o, "length")));

            for (int64_t i = 0; i < length && i < 100000; i++) {
                char key[24];

                ksnprintf(key, sizeof(key), "%ld", (long)i);
                js_array_push(ctx, out, js_get_value(ctx, source, key));
            }
        }
    }
    return js_object_value(out);
}

/* ------------------------------------------------------------------ */
/* String                                                              */
/* ------------------------------------------------------------------ */

static struct js_value native_char_at(struct js_context *ctx,
                                      struct js_value self,
                                      struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    int64_t index = arg_int(ctx, args, count, 0, 0);

    if (!s || index < 0 || (size_t)index >= s->length)
        return js_str(ctx, "");
    return js_strn(ctx, s->data + index, 1);
}

static struct js_value native_char_code(struct js_context *ctx,
                                        struct js_value self,
                                        struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    int64_t index = arg_int(ctx, args, count, 0, 0);

    if (!s || index < 0 || (size_t)index >= s->length)
        return js_number(JS_NAN);
    return js_integer((unsigned char)s->data[index]);
}

static struct js_value native_from_char_code(struct js_context *ctx,
                                             struct js_value self,
                                             struct js_value *args,
                                             size_t count)
{
    char buffer[64];
    size_t at = 0;

    UNUSED(self);
    for (size_t i = 0; i < count && at + 1 < sizeof(buffer); i++)
        buffer[at++] = (char)js_to_int(js_to_number(ctx, args[i]));
    buffer[at] = '\0';
    return js_strn(ctx, buffer, at);
}

static struct js_value native_substring(struct js_context *ctx,
                                        struct js_value self,
                                        struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    int64_t length = s ? (int64_t)s->length : 0;
    int64_t from = CLAMP(arg_int(ctx, args, count, 0, 0), 0, length);
    int64_t to = CLAMP(arg_int(ctx, args, count, 1, length), 0, length);

    if (from > to) {
        int64_t swap = from;

        from = to;
        to = swap;
    }
    return js_strn(ctx, s->data + from, (size_t)(to - from));
}

static struct js_value native_substr(struct js_context *ctx,
                                     struct js_value self,
                                     struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    int64_t length = s ? (int64_t)s->length : 0;
    int64_t from = arg_int(ctx, args, count, 0, 0);

    if (from < 0)
        from += length;
    from = CLAMP(from, 0, length);

    int64_t take = CLAMP(arg_int(ctx, args, count, 1, length - from), 0,
                         length - from);

    return js_strn(ctx, s->data + from, (size_t)take);
}

static struct js_value native_to_upper(struct js_context *ctx,
                                       struct js_value self,
                                       struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    struct js_string *out = js_string_new(ctx, s->data, s->length);

    UNUSED(args);
    UNUSED(count);
    for (size_t i = 0; i < out->length; i++)
        if (out->data[i] >= 'a' && out->data[i] <= 'z')
            out->data[i] = (char)(out->data[i] - 32);

    struct js_value v;

    v.type = JS_STRING;
    v.as.string = out;
    return v;
}

static struct js_value native_to_lower(struct js_context *ctx,
                                       struct js_value self,
                                       struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    struct js_string *out = js_string_new(ctx, s->data, s->length);

    UNUSED(args);
    UNUSED(count);
    for (size_t i = 0; i < out->length; i++)
        if (out->data[i] >= 'A' && out->data[i] <= 'Z')
            out->data[i] = (char)(out->data[i] + 32);

    struct js_value v;

    v.type = JS_STRING;
    v.as.string = out;
    return v;
}

static bool white(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

static struct js_value native_trim(struct js_context *ctx, struct js_value self,
                                   struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    size_t from = 0, to = s ? s->length : 0;

    UNUSED(args);
    UNUSED(count);
    while (from < to && white(s->data[from]))
        from++;
    while (to > from && white(s->data[to - 1]))
        to--;
    return js_strn(ctx, s->data + from, to - from);
}

static struct js_value native_split(struct js_context *ctx,
                                    struct js_value self,
                                    struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    struct js_object *out = js_new_array(ctx, 0);

    if (!s)
        return js_object_value(out);

    if (count == 0 || args[0].type == JS_UNDEFINED) {
        struct js_value whole;

        whole.type = JS_STRING;
        whole.as.string = s;
        js_array_push(ctx, out, whole);
        return js_object_value(out);
    }

    struct js_string *sep = js_to_string(ctx, args[0]);

    if (sep->length == 0) {
        for (size_t i = 0; i < s->length; i++)
            js_array_push(ctx, out, js_strn(ctx, s->data + i, 1));
        return js_object_value(out);
    }

    size_t start = 0;

    for (size_t i = 0; i + sep->length <= s->length; ) {
        if (memcmp(s->data + i, sep->data, sep->length) == 0) {
            js_array_push(ctx, out, js_strn(ctx, s->data + start, i - start));
            i += sep->length;
            start = i;
            continue;
        }
        i++;
    }
    js_array_push(ctx, out, js_strn(ctx, s->data + start, s->length - start));
    return js_object_value(out);
}

static struct js_value native_replace(struct js_context *ctx,
                                      struct js_value self,
                                      struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    struct js_string *find = arg_string(ctx, args, count, 0);
    struct js_string *with = arg_string(ctx, args, count, 1);
    bool all = false;

    if (!s || !find || !with || find->length == 0)
        return self;

    /* Ein Muster mit g am Ende ersetzt ueberall. */
    if (count > 0 && args[0].type == JS_STRING && find->length >= 2 &&
        find->data[0] == '/') {
        for (size_t i = find->length; i > 1; i--) {
            if (find->data[i - 1] == '/') {
                struct js_string *inner = js_string_new(ctx, find->data + 1,
                                                        i - 2);

                all = i < find->length &&
                      strchr(find->data + i, 'g') != NULL;
                find = inner;
                break;
            }
        }
    }
    if (find->length == 0)
        return self;

    char *buffer = js_alloc(ctx, s->length + 64);
    size_t capacity = s->length + 64;
    size_t at = 0;
    size_t i = 0;

    if (!buffer)
        return self;

    while (i < s->length) {
        if (i + find->length <= s->length &&
            memcmp(s->data + i, find->data, find->length) == 0) {
            if (at + with->length + 1 > capacity) {
                size_t want = (at + with->length + 64) * 2;
                char *bigger = js_alloc(ctx, want);

                if (!bigger)
                    break;
                memcpy(bigger, buffer, at);
                buffer = bigger;
                capacity = want;
            }
            memcpy(buffer + at, with->data, with->length);
            at += with->length;
            i += find->length;
            if (!all) {
                size_t rest = s->length - i;

                if (at + rest + 1 > capacity) {
                    char *bigger = js_alloc(ctx, at + rest + 8);

                    if (!bigger)
                        break;
                    memcpy(bigger, buffer, at);
                    buffer = bigger;
                    capacity = at + rest + 8;
                }
                memcpy(buffer + at, s->data + i, rest);
                at += rest;
                i = s->length;
            }
            continue;
        }
        if (at + 2 > capacity) {
            size_t want = capacity * 2;
            char *bigger = js_alloc(ctx, want);

            if (!bigger)
                break;
            memcpy(bigger, buffer, at);
            buffer = bigger;
            capacity = want;
        }
        buffer[at++] = s->data[i++];
    }
    return js_strn(ctx, buffer, at);
}

static struct js_value native_starts_with(struct js_context *ctx,
                                          struct js_value self,
                                          struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    struct js_string *prefix = arg_string(ctx, args, count, 0);

    if (!s || !prefix || prefix->length > s->length)
        return js_bool(false);
    return js_bool(memcmp(s->data, prefix->data, prefix->length) == 0);
}

static struct js_value native_ends_with(struct js_context *ctx,
                                        struct js_value self,
                                        struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    struct js_string *suffix = arg_string(ctx, args, count, 0);

    if (!s || !suffix || suffix->length > s->length)
        return js_bool(false);
    return js_bool(memcmp(s->data + s->length - suffix->length, suffix->data,
                          suffix->length) == 0);
}

static struct js_value native_repeat(struct js_context *ctx,
                                     struct js_value self,
                                     struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, self);
    int64_t times = CLAMP(arg_int(ctx, args, count, 0, 0), 0, 10000);

    if (!s || s->length * (size_t)times > (1u << 20))
        return js_str(ctx, "");

    struct js_string *out = js_string_new(ctx, NULL, s->length * (size_t)times);

    for (int64_t i = 0; i < times; i++)
        memcpy(out->data + (size_t)i * s->length, s->data, s->length);

    struct js_value v;

    v.type = JS_STRING;
    v.as.string = out;
    return v;
}

static struct js_value native_pad(struct js_context *ctx, struct js_value self,
                                  struct js_value *args, size_t count,
                                  bool start)
{
    struct js_string *s = js_to_string(ctx, self);
    int64_t want = CLAMP(arg_int(ctx, args, count, 0, 0), 0, 100000);
    struct js_string *fill = count > 1 ? js_to_string(ctx, args[1])
                                       : js_string_new(ctx, " ", 1);

    if (!s || !fill || fill->length == 0 || (int64_t)s->length >= want)
        return self;

    struct js_string *out = js_string_new(ctx, NULL, (size_t)want);
    size_t padding = (size_t)want - s->length;

    if (start) {
        for (size_t i = 0; i < padding; i++)
            out->data[i] = fill->data[i % fill->length];
        memcpy(out->data + padding, s->data, s->length);
    } else {
        memcpy(out->data, s->data, s->length);
        for (size_t i = 0; i < padding; i++)
            out->data[s->length + i] = fill->data[i % fill->length];
    }
    out->data[want] = '\0';

    struct js_value v;

    v.type = JS_STRING;
    v.as.string = out;
    return v;
}

static struct js_value native_pad_start(struct js_context *ctx,
                                        struct js_value self,
                                        struct js_value *args, size_t count)
{
    return native_pad(ctx, self, args, count, true);
}

static struct js_value native_pad_end(struct js_context *ctx,
                                      struct js_value self,
                                      struct js_value *args, size_t count)
{
    return native_pad(ctx, self, args, count, false);
}

static struct js_value native_string_ctor(struct js_context *ctx,
                                          struct js_value self,
                                          struct js_value *args, size_t count)
{
    struct js_value v;

    UNUSED(self);
    v.type = JS_STRING;
    v.as.string = js_to_string(ctx, ARG(0));
    return v;
}

/* ------------------------------------------------------------------ */
/* Number, parseInt, Math                                              */
/* ------------------------------------------------------------------ */

static struct js_value native_number_ctor(struct js_context *ctx,
                                          struct js_value self,
                                          struct js_value *args, size_t count)
{
    UNUSED(self);
    if (count == 0)
        return js_integer(0);
    return js_number(js_to_number(ctx, args[0]));
}

static struct js_value native_parse_int(struct js_context *ctx,
                                        struct js_value self,
                                        struct js_value *args, size_t count)
{
    struct js_string *s = arg_string(ctx, args, count, 0);
    int64_t base = arg_int(ctx, args, count, 1, 10);

    UNUSED(self);
    if (!s)
        return js_number(JS_NAN);
    if (base < 2 || base > 36)
        base = 10;

    size_t at = 0;

    while (at < s->length && white(s->data[at]))
        at++;

    bool negative = false;

    if (at < s->length && (s->data[at] == '-' || s->data[at] == '+'))
        negative = s->data[at++] == '-';

    if (base == 10 && at + 1 < s->length && s->data[at] == '0' &&
        (s->data[at + 1] == 'x' || s->data[at + 1] == 'X')) {
        base = 16;
        at += 2;
    }

    int64_t value = 0;
    bool any = false;

    while (at < s->length) {
        char c = s->data[at];
        int64_t digit;

        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'z')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z')
            digit = c - 'A' + 10;
        else
            break;
        if (digit >= base)
            break;
        value = value * base + digit;
        any = true;
        at++;
    }
    if (!any)
        return js_number(JS_NAN);
    return js_integer(negative ? -value : value);
}

static struct js_value native_parse_float(struct js_context *ctx,
                                          struct js_value self,
                                          struct js_value *args, size_t count)
{
    struct js_string *s = arg_string(ctx, args, count, 0);

    UNUSED(self);
    if (!s || s->length == 0)
        return js_number(JS_NAN);

    /* Der lockere Weg: ab dem ersten ungueltigen Zeichen wird
     * abgeschnitten, alles davor gilt als Zahl. */
    size_t at = 0;

    while (at < s->length && white(s->data[at]))
        at++;
    size_t start = at;

    if (at < s->length && (s->data[at] == '-' || s->data[at] == '+'))
        at++;
    while (at < s->length && s->data[at] >= '0' && s->data[at] <= '9')
        at++;
    if (at < s->length && s->data[at] == '.') {
        at++;
        while (at < s->length && s->data[at] >= '0' && s->data[at] <= '9')
            at++;
    }
    if (at < s->length && (s->data[at] == 'e' || s->data[at] == 'E')) {
        size_t save = at;

        at++;
        if (at < s->length && (s->data[at] == '-' || s->data[at] == '+'))
            at++;
        if (at < s->length && s->data[at] >= '0' && s->data[at] <= '9')
            while (at < s->length && s->data[at] >= '0' && s->data[at] <= '9')
                at++;
        else
            at = save;
    }
    if (at == start)
        return js_number(JS_NAN);
    return js_number(js_to_number(ctx, js_strn(ctx, s->data + start,
                                               at - start)));
}

static struct js_value native_is_nan(struct js_context *ctx,
                                     struct js_value self,
                                     struct js_value *args, size_t count)
{
    UNUSED(self);
    return js_bool(js_is_nan(js_to_number(ctx, ARG(0))));
}

static struct js_value native_number_is_finite(struct js_context *ctx,
                                               struct js_value self,
                                               struct js_value *args,
                                               size_t count)
{
    UNUSED(self);
    return js_bool(count > 0 && args[0].type == JS_NUMBER &&
                   js_is_finite(args[0].as.number));
}

static struct js_value native_to_fixed(struct js_context *ctx,
                                       struct js_value self,
                                       struct js_value *args, size_t count)
{
    js_num value = js_to_number(ctx, self);
    int64_t places = CLAMP(arg_int(ctx, args, count, 0, 0), 0, 6);

    if (!js_is_finite(value)) {
        char buffer[24];

        js_number_to_text(value, buffer, sizeof(buffer));
        return js_str(ctx, buffer);
    }

    bool negative = value < 0;
    uint64_t magnitude = negative ? (uint64_t)(-value) : (uint64_t)value;
    uint64_t whole = magnitude >> JS_FRACTION;
    uint64_t frac = magnitude & (JS_ONE - 1);
    char decimals[8];

    for (int64_t i = 0; i < places; i++) {
        frac *= 10;
        decimals[i] = (char)('0' + (frac >> JS_FRACTION));
        frac &= JS_ONE - 1;
    }

    /* Aufrunden, wenn die naechste Stelle es verlangt. */
    frac *= 10;
    if ((frac >> JS_FRACTION) >= 5) {
        int64_t i = places - 1;

        while (i >= 0) {
            if (decimals[i] < '9') {
                decimals[i]++;
                break;
            }
            decimals[i] = '0';
            i--;
        }
        if (i < 0)
            whole++;
    }

    char buffer[48];
    size_t at = 0;
    char digits[24];
    size_t found = 0;

    if (whole == 0)
        digits[found++] = '0';
    while (whole > 0 && found < sizeof(digits)) {
        digits[found++] = (char)('0' + whole % 10);
        whole /= 10;
    }
    if (negative)
        buffer[at++] = '-';
    while (found > 0)
        buffer[at++] = digits[--found];
    if (places > 0) {
        buffer[at++] = '.';
        for (int64_t i = 0; i < places; i++)
            buffer[at++] = decimals[i];
    }
    buffer[at] = '\0';
    return js_strn(ctx, buffer, at);
}

/* Wurzel nach dem Verfahren von Newton, in Festkomma. */
static js_num fixed_sqrt(js_num value)
{
    if (value <= 0)
        return 0;
    if (!js_is_finite(value))
        return value;

    js_num guess = value > JS_ONE ? value : JS_ONE;

    for (int i = 0; i < 40; i++) {
        js_num next = (guess + js_div_num(value, guess)) / 2;

        if (next == guess)
            break;
        guess = next;
    }
    return guess;
}

static struct js_value native_math(struct js_context *ctx, struct js_value self,
                                   struct js_value *args, size_t count)
{
    UNUSED(ctx);
    UNUSED(self);
    UNUSED(args);
    UNUSED(count);
    return js_undefined();
}

#define MATH_FN(name, body)                                             \
    static struct js_value name(struct js_context *ctx,                 \
                                struct js_value self,                   \
                                struct js_value *args, size_t count)    \
    {                                                                   \
        js_num x = js_to_number(ctx, ARG(0));                           \
        UNUSED(self);                                                   \
        body                                                            \
    }

MATH_FN(math_abs, { return js_number(x < 0 && js_is_finite(x) ? -x :
                                     (x == JS_NEG_INF ? JS_POS_INF : x)); })
MATH_FN(math_floor, {
    if (!js_is_finite(x))
        return js_number(x);
    return js_number(x & ~(js_num)(JS_ONE - 1));
})
MATH_FN(math_ceil, {
    if (!js_is_finite(x))
        return js_number(x);
    return js_number((x + JS_ONE - 1) & ~(js_num)(JS_ONE - 1));
})
MATH_FN(math_round, {
    if (!js_is_finite(x))
        return js_number(x);
    return js_number((x + JS_ONE / 2) & ~(js_num)(JS_ONE - 1));
})
MATH_FN(math_trunc, {
    if (!js_is_finite(x))
        return js_number(x);
    return js_number(js_from_int(js_to_int(x)));
})
MATH_FN(math_sqrt, { return js_number(js_is_nan(x) || x < 0 ? JS_NAN
                                                            : fixed_sqrt(x)); })
MATH_FN(math_sign, {
    if (js_is_nan(x))
        return js_number(JS_NAN);
    return js_integer(x > 0 ? 1 : (x < 0 ? -1 : 0));
})

static struct js_value math_min_max(struct js_context *ctx,
                                    struct js_value *args, size_t count,
                                    bool maximum)
{
    js_num best = maximum ? JS_NEG_INF : JS_POS_INF;

    if (count == 0)
        return js_number(best);
    for (size_t i = 0; i < count; i++) {
        js_num v = js_to_number(ctx, args[i]);

        if (js_is_nan(v))
            return js_number(JS_NAN);
        if (maximum ? v > best : v < best)
            best = v;
    }
    return js_number(best);
}

static struct js_value math_min(struct js_context *ctx, struct js_value self,
                                struct js_value *args, size_t count)
{
    UNUSED(self);
    return math_min_max(ctx, args, count, false);
}

static struct js_value math_max(struct js_context *ctx, struct js_value self,
                                struct js_value *args, size_t count)
{
    UNUSED(self);
    return math_min_max(ctx, args, count, true);
}

static struct js_value math_pow(struct js_context *ctx, struct js_value self,
                                struct js_value *args, size_t count)
{
    js_num base = js_to_number(ctx, ARG(0));
    int64_t exponent = js_to_int(js_to_number(ctx, ARG(1)));
    js_num result = JS_ONE;
    bool negative = exponent < 0;

    UNUSED(self);
    if (negative)
        exponent = -exponent;
    for (int64_t i = 0; i < exponent && i < 512; i++)
        result = js_mul_num(result, base);
    return js_number(negative ? js_div_num(JS_ONE, result) : result);
}

/* Ein einfacher Zufallszahlengenerator. Fuer Kryptografie taugt er
 * nicht, fuer Seitenskripte reicht er. */
static uint64_t random_state = 0x2545F4914F6CDD1DULL;

static struct js_value math_random(struct js_context *ctx,
                                   struct js_value self,
                                   struct js_value *args, size_t count)
{
    UNUSED(ctx);
    UNUSED(self);
    UNUSED(args);
    UNUSED(count);
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return js_number((js_num)(random_state & (JS_ONE - 1)));
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

struct json_writer {
    struct js_context *ctx;
    char   *buffer;
    size_t  at, capacity;
};

static void json_put(struct json_writer *w, const char *text, size_t length)
{
    if (w->at + length + 1 > w->capacity) {
        size_t want = w->capacity ? w->capacity * 2 : 256;

        while (want < w->at + length + 1)
            want *= 2;
        if (want > (2u << 20))
            return;

        char *bigger = js_alloc(w->ctx, want);

        if (!bigger)
            return;
        memcpy(bigger, w->buffer, w->at);
        w->buffer = bigger;
        w->capacity = want;
    }
    memcpy(w->buffer + w->at, text, length);
    w->at += length;
    w->buffer[w->at] = '\0';
}

static void json_quote(struct json_writer *w, const char *s, size_t length)
{
    json_put(w, "\"", 1);
    for (size_t i = 0; i < length; i++) {
        char c = s[i];

        switch (c) {
        case '"':  json_put(w, "\\\"", 2); break;
        case '\\': json_put(w, "\\\\", 2); break;
        case '\n': json_put(w, "\\n", 2); break;
        case '\r': json_put(w, "\\r", 2); break;
        case '\t': json_put(w, "\\t", 2); break;
        default:
            if ((unsigned char)c < 0x20) {
                char escape[8];

                ksnprintf(escape, sizeof(escape), "\\u%04x",
                          (unsigned char)c);
                json_put(w, escape, 6);
            } else {
                json_put(w, &c, 1);
            }
            break;
        }
    }
    json_put(w, "\"", 1);
}

static void json_write(struct json_writer *w, struct js_value v, int32_t depth)
{
    if (depth > 12) {
        json_put(w, "null", 4);
        return;
    }

    switch (v.type) {
    case JS_UNDEFINED:
    case JS_NULL:
        json_put(w, "null", 4);
        break;
    case JS_BOOL:
        json_put(w, v.as.boolean ? "true" : "false", v.as.boolean ? 4 : 5);
        break;
    case JS_NUMBER: {
        char buffer[48];
        size_t length = js_number_to_text(v.as.number, buffer, sizeof(buffer));

        if (!js_is_finite(v.as.number))
            json_put(w, "null", 4);
        else
            json_put(w, buffer, MIN(length, sizeof(buffer) - 1));
        break;
    }
    case JS_STRING:
        json_quote(w, v.as.string->data, v.as.string->length);
        break;
    default: {
        struct js_object *o = v.as.object;

        if (o->klass == CLASS_ARRAY || o->klass == CLASS_NODELIST) {
            json_put(w, "[", 1);
            for (size_t i = 0; i < o->length; i++) {
                if (i)
                    json_put(w, ",", 1);
                json_write(w, o->elements[i], depth + 1);
            }
            json_put(w, "]", 1);
            break;
        }
        if (o->klass == CLASS_FUNCTION || o->klass == CLASS_NATIVE) {
            json_put(w, "null", 4);
            break;
        }

        struct js_prop *names[256];
        size_t found = 0;

        for (struct js_prop *p = o->props; p && found < ARRAY_LEN(names);
             p = p->next)
            if (p->enumerable)
                names[found++] = p;

        json_put(w, "{", 1);
        for (size_t i = found; i > 0; i--) {
            struct js_prop *p = names[i - 1];

            if (i != found)
                json_put(w, ",", 1);
            json_quote(w, p->name, strlen(p->name));
            json_put(w, ":", 1);
            json_write(w, p->value, depth + 1);
        }
        json_put(w, "}", 1);
        break;
    }
    }
}

static struct js_value native_json_stringify(struct js_context *ctx,
                                             struct js_value self,
                                             struct js_value *args,
                                             size_t count)
{
    struct json_writer w = { ctx, js_alloc(ctx, 256), 0, 256 };

    UNUSED(self);
    if (!w.buffer)
        return js_undefined();
    w.buffer[0] = '\0';
    json_write(&w, ARG(0), 0);
    return js_strn(ctx, w.buffer, w.at);
}

struct json_reader {
    struct js_context *ctx;
    const char *s;
    size_t      length, pos;
    bool        failed;
};

static struct js_value json_read(struct json_reader *r, int32_t depth);

static void json_skip(struct json_reader *r)
{
    while (r->pos < r->length && white(r->s[r->pos]))
        r->pos++;
}

static struct js_value json_read_string(struct json_reader *r)
{
    r->pos++;                    /* Anfuehrungszeichen */

    char *out = js_alloc(r->ctx, r->length - r->pos + 1);
    size_t at = 0;

    if (!out) {
        r->failed = true;
        return js_undefined();
    }

    while (r->pos < r->length && r->s[r->pos] != '"') {
        char c = r->s[r->pos++];

        if (c != '\\') {
            out[at++] = c;
            continue;
        }
        if (r->pos >= r->length)
            break;

        char escape = r->s[r->pos++];

        switch (escape) {
        case 'n': out[at++] = '\n'; break;
        case 't': out[at++] = '\t'; break;
        case 'r': out[at++] = '\r'; break;
        case 'b': out[at++] = '\b'; break;
        case 'f': out[at++] = '\f'; break;
        case 'u': {
            int32_t value = 0;

            for (int i = 0; i < 4 && r->pos < r->length; i++) {
                char h = r->s[r->pos++];

                if (h >= '0' && h <= '9')
                    value = value * 16 + (h - '0');
                else if (h >= 'a' && h <= 'f')
                    value = value * 16 + (h - 'a' + 10);
                else if (h >= 'A' && h <= 'F')
                    value = value * 16 + (h - 'A' + 10);
            }
            out[at++] = value < 256 ? (char)value : '?';
            break;
        }
        default: out[at++] = escape; break;
        }
    }
    if (r->pos < r->length)
        r->pos++;
    out[at] = '\0';
    return js_strn(r->ctx, out, at);
}

static struct js_value json_read(struct json_reader *r, int32_t depth)
{
    json_skip(r);
    if (r->pos >= r->length || depth > 24) {
        r->failed = true;
        return js_undefined();
    }

    char c = r->s[r->pos];

    if (c == '"')
        return json_read_string(r);
    if (c == '{') {
        struct js_object *o = js_new_object(r->ctx, CLASS_OBJECT);

        r->pos++;
        json_skip(r);
        if (r->pos < r->length && r->s[r->pos] == '}') {
            r->pos++;
            return js_object_value(o);
        }
        for (;;) {
            json_skip(r);
            if (r->pos >= r->length || r->s[r->pos] != '"') {
                r->failed = true;
                break;
            }

            struct js_value key = json_read_string(r);

            json_skip(r);
            if (r->pos >= r->length || r->s[r->pos] != ':') {
                r->failed = true;
                break;
            }
            r->pos++;
            js_set(r->ctx, o, key.as.string->data, json_read(r, depth + 1));
            json_skip(r);
            if (r->pos < r->length && r->s[r->pos] == ',') {
                r->pos++;
                continue;
            }
            if (r->pos < r->length && r->s[r->pos] == '}')
                r->pos++;
            break;
        }
        return js_object_value(o);
    }
    if (c == '[') {
        struct js_object *a = js_new_array(r->ctx, 0);

        r->pos++;
        json_skip(r);
        if (r->pos < r->length && r->s[r->pos] == ']') {
            r->pos++;
            return js_object_value(a);
        }
        for (;;) {
            js_array_push(r->ctx, a, json_read(r, depth + 1));
            json_skip(r);
            if (r->pos < r->length && r->s[r->pos] == ',') {
                r->pos++;
                continue;
            }
            if (r->pos < r->length && r->s[r->pos] == ']')
                r->pos++;
            break;
        }
        return js_object_value(a);
    }
    if (r->pos + 4 <= r->length && strncmp(r->s + r->pos, "true", 4) == 0) {
        r->pos += 4;
        return js_bool(true);
    }
    if (r->pos + 5 <= r->length && strncmp(r->s + r->pos, "false", 5) == 0) {
        r->pos += 5;
        return js_bool(false);
    }
    if (r->pos + 4 <= r->length && strncmp(r->s + r->pos, "null", 4) == 0) {
        r->pos += 4;
        return js_null();
    }

    size_t start = r->pos;

    if (r->s[r->pos] == '-' || r->s[r->pos] == '+')
        r->pos++;
    while (r->pos < r->length && ((r->s[r->pos] >= '0' && r->s[r->pos] <= '9') ||
                                  r->s[r->pos] == '.' || r->s[r->pos] == 'e' ||
                                  r->s[r->pos] == 'E' || r->s[r->pos] == '-' ||
                                  r->s[r->pos] == '+'))
        r->pos++;
    if (r->pos == start) {
        r->failed = true;
        return js_undefined();
    }
    return js_number(js_to_number(r->ctx,
                                  js_strn(r->ctx, r->s + start, r->pos - start)));
}

static struct js_value native_json_parse(struct js_context *ctx,
                                         struct js_value self,
                                         struct js_value *args, size_t count)
{
    struct js_string *s = arg_string(ctx, args, count, 0);
    struct json_reader r = { ctx, s ? s->data : "", s ? s->length : 0, 0,
                             false };

    UNUSED(self);

    struct js_value value = json_read(&r, 0);

    if (r.failed) {
        js_throw(ctx, "JSON konnte nicht gelesen werden");
        return js_undefined();
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* Funktionen auf Funktionen                                           */
/* ------------------------------------------------------------------ */

static struct js_value native_call(struct js_context *ctx,
                                   struct js_value self,
                                   struct js_value *args, size_t count)
{
    return js_call(ctx, self, ARG(0), count > 1 ? args + 1 : NULL,
                   count > 1 ? count - 1 : 0);
}

static struct js_value native_apply(struct js_context *ctx,
                                    struct js_value self,
                                    struct js_value *args, size_t count)
{
    struct js_value list = ARG(1);

    if (list.type == JS_OBJECT && list.as.object &&
        (list.as.object->klass == CLASS_ARRAY ||
         list.as.object->klass == CLASS_NODELIST))
        return js_call(ctx, self, ARG(0), list.as.object->elements,
                       list.as.object->length);
    return js_call(ctx, self, ARG(0), NULL, 0);
}

static struct js_value native_bind(struct js_context *ctx, struct js_value self,
                                   struct js_value *args, size_t count)
{
    if (self.type != JS_OBJECT || !self.as.object)
        return js_undefined();

    struct js_object *bound = js_new_object(ctx, self.as.object->klass);

    if (!bound)
        return js_undefined();
    *bound = *self.as.object;
    bound->props = NULL;
    bound->bound_this = ARG(0);
    bound->has_bound_this = true;
    return js_object_value(bound);
}

/* ------------------------------------------------------------------ */
/* Datum                                                               */
/* ------------------------------------------------------------------ */

static struct js_value native_date_now(struct js_context *ctx,
                                       struct js_value self,
                                       struct js_value *args, size_t count)
{
    UNUSED(ctx);
    UNUSED(self);
    UNUSED(args);
    UNUSED(count);
    return js_integer((int64_t)timer_ms());
}

static struct js_value native_date_ctor(struct js_context *ctx,
                                        struct js_value self,
                                        struct js_value *args, size_t count)
{
    struct js_object *o = js_new_object(ctx, CLASS_OBJECT);
    struct datetime now;

    UNUSED(self);
    UNUSED(args);
    UNUSED(count);
    rtc_read(&now);

    /* Die Abfragen bedienen wir ueber verborgene Eigenschaften. */
    js_set(ctx, o, "__year", js_integer(now.year));
    js_set(ctx, o, "__month", js_integer(now.month - 1));
    js_set(ctx, o, "__day", js_integer(now.day));
    js_set(ctx, o, "__hours", js_integer(now.hour));
    js_set(ctx, o, "__minutes", js_integer(now.minute));
    js_set(ctx, o, "__seconds", js_integer(now.second));

    /* Die Abfrageverfahren haengen am Prototyp des Erzeugers. */
    struct js_value ctor = js_get(ctx, ctx->global, "Date");

    if (ctor.type == JS_OBJECT && ctor.as.object) {
        struct js_value prototype = js_get(ctx, ctor.as.object, "prototype");

        if (prototype.type == JS_OBJECT)
            o->prototype = prototype.as.object;
    }
    return js_object_value(o);
}

#define DATE_GETTER(name, field)                                        \
    static struct js_value name(struct js_context *ctx,                 \
                                struct js_value self,                   \
                                struct js_value *args, size_t count)    \
    {                                                                   \
        UNUSED(args); UNUSED(count);                                    \
        if (self.type != JS_OBJECT || !self.as.object)                  \
            return js_integer(0);                                       \
        return js_get(ctx, self.as.object, field);                      \
    }

DATE_GETTER(date_year, "__year")
DATE_GETTER(date_month, "__month")
DATE_GETTER(date_day, "__day")
DATE_GETTER(date_hours, "__hours")
DATE_GETTER(date_minutes, "__minutes")
DATE_GETTER(date_seconds, "__seconds")

/* ------------------------------------------------------------------ */
/* Einbau                                                              */
/* ------------------------------------------------------------------ */

void js_install_builtins(struct js_context *ctx)
{
    struct js_object *global = ctx->global;

    /* --- Prototypen --- */
    js_set_native(ctx, ctx->object_prototype, "hasOwnProperty",
                  native_has_own, 1);
    js_set_native(ctx, ctx->object_prototype, "toString", native_to_string, 0);
    js_set_native(ctx, ctx->object_prototype, "valueOf", native_to_string, 0);

    struct js_object *ap = ctx->array_prototype;

    js_set_native(ctx, ap, "push", native_push, 1);
    js_set_native(ctx, ap, "pop", native_pop, 0);
    js_set_native(ctx, ap, "shift", native_shift, 0);
    js_set_native(ctx, ap, "unshift", native_unshift, 1);
    js_set_native(ctx, ap, "slice", native_slice, 2);
    js_set_native(ctx, ap, "splice", native_splice, 2);
    js_set_native(ctx, ap, "join", native_join, 1);
    js_set_native(ctx, ap, "indexOf", native_index_of, 1);
    js_set_native(ctx, ap, "lastIndexOf", native_last_index_of, 1);
    js_set_native(ctx, ap, "includes", native_includes, 1);
    js_set_native(ctx, ap, "forEach", native_foreach, 1);
    js_set_native(ctx, ap, "map", native_map, 1);
    js_set_native(ctx, ap, "filter", native_filter, 1);
    js_set_native(ctx, ap, "find", native_find, 1);
    js_set_native(ctx, ap, "findIndex", native_find_index, 1);
    js_set_native(ctx, ap, "some", native_some, 1);
    js_set_native(ctx, ap, "every", native_every, 1);
    js_set_native(ctx, ap, "reduce", native_reduce, 2);
    js_set_native(ctx, ap, "reverse", native_reverse, 0);
    js_set_native(ctx, ap, "sort", native_sort, 1);
    js_set_native(ctx, ap, "concat", native_concat, 1);
    js_set_native(ctx, ap, "toString", native_to_string, 0);

    struct js_object *sp = ctx->string_prototype;

    js_set_native(ctx, sp, "charAt", native_char_at, 1);
    js_set_native(ctx, sp, "charCodeAt", native_char_code, 1);
    js_set_native(ctx, sp, "codePointAt", native_char_code, 1);
    js_set_native(ctx, sp, "indexOf", native_index_of, 1);
    js_set_native(ctx, sp, "lastIndexOf", native_last_index_of, 1);
    js_set_native(ctx, sp, "includes", native_includes, 1);
    js_set_native(ctx, sp, "slice", native_slice, 2);
    js_set_native(ctx, sp, "substring", native_substring, 2);
    js_set_native(ctx, sp, "substr", native_substr, 2);
    js_set_native(ctx, sp, "toUpperCase", native_to_upper, 0);
    js_set_native(ctx, sp, "toLowerCase", native_to_lower, 0);
    js_set_native(ctx, sp, "trim", native_trim, 0);
    js_set_native(ctx, sp, "trimStart", native_trim, 0);
    js_set_native(ctx, sp, "trimEnd", native_trim, 0);
    js_set_native(ctx, sp, "split", native_split, 2);
    js_set_native(ctx, sp, "replace", native_replace, 2);
    js_set_native(ctx, sp, "replaceAll", native_replace, 2);
    js_set_native(ctx, sp, "startsWith", native_starts_with, 1);
    js_set_native(ctx, sp, "endsWith", native_ends_with, 1);
    js_set_native(ctx, sp, "repeat", native_repeat, 1);
    js_set_native(ctx, sp, "padStart", native_pad_start, 2);
    js_set_native(ctx, sp, "padEnd", native_pad_end, 2);
    js_set_native(ctx, sp, "concat", native_concat, 1);
    js_set_native(ctx, sp, "toString", native_to_string, 0);

    struct js_object *np = ctx->number_prototype;

    js_set_native(ctx, np, "toFixed", native_to_fixed, 1);
    js_set_native(ctx, np, "toString", native_to_string, 1);
    js_set_native(ctx, np, "valueOf", native_to_string, 0);

    struct js_object *fp = ctx->function_prototype;

    js_set_native(ctx, fp, "call", native_call, 1);
    js_set_native(ctx, fp, "apply", native_apply, 2);
    js_set_native(ctx, fp, "bind", native_bind, 1);

    /* --- console --- */
    struct js_object *console = js_new_object(ctx, CLASS_OBJECT);

    js_set_native(ctx, console, "log", native_log, 1);
    js_set_native(ctx, console, "info", native_log, 1);
    js_set_native(ctx, console, "warn", native_log, 1);
    js_set_native(ctx, console, "error", native_log, 1);
    js_set_native(ctx, console, "debug", native_log, 1);
    js_set(ctx, global, "console", js_object_value(console));

    /* --- Object --- */
    struct js_object *object_ctor = js_new_object(ctx, CLASS_NATIVE);

    object_ctor->native = native_object_ctor;
    object_ctor->name = "Object";
    js_set_native(ctx, object_ctor, "keys", native_object_keys, 1);
    js_set_native(ctx, object_ctor, "values", native_object_values, 1);
    js_set_native(ctx, object_ctor, "entries", native_object_entries, 1);
    js_set_native(ctx, object_ctor, "assign", native_object_assign, 2);
    js_set_hidden(ctx, object_ctor, "prototype",
                  js_object_value(ctx->object_prototype));
    js_set(ctx, global, "Object", js_object_value(object_ctor));

    /* --- Array --- */
    struct js_object *array_ctor = js_new_object(ctx, CLASS_NATIVE);

    array_ctor->native = native_array_ctor;
    array_ctor->name = "Array";
    js_set_native(ctx, array_ctor, "isArray", native_is_array, 1);
    js_set_native(ctx, array_ctor, "from", native_array_from, 1);
    js_set_native(ctx, array_ctor, "of", native_array_ctor, 0);
    js_set_hidden(ctx, array_ctor, "prototype", js_object_value(ap));
    js_set(ctx, global, "Array", js_object_value(array_ctor));

    /* --- String und Number --- */
    struct js_object *string_ctor = js_new_object(ctx, CLASS_NATIVE);

    string_ctor->native = native_string_ctor;
    string_ctor->name = "String";
    js_set_native(ctx, string_ctor, "fromCharCode", native_from_char_code, 1);
    js_set_hidden(ctx, string_ctor, "prototype", js_object_value(sp));
    js_set(ctx, global, "String", js_object_value(string_ctor));

    struct js_object *number_ctor = js_new_object(ctx, CLASS_NATIVE);

    number_ctor->native = native_number_ctor;
    number_ctor->name = "Number";
    js_set_native(ctx, number_ctor, "parseInt", native_parse_int, 2);
    js_set_native(ctx, number_ctor, "parseFloat", native_parse_float, 1);
    js_set_native(ctx, number_ctor, "isNaN", native_is_nan, 1);
    js_set_native(ctx, number_ctor, "isFinite", native_number_is_finite, 1);
    js_set_native(ctx, number_ctor, "isInteger", native_number_is_finite, 1);
    js_set(ctx, number_ctor, "MAX_SAFE_INTEGER",
           js_integer((int64_t)1 << 46));
    js_set(ctx, number_ctor, "MIN_SAFE_INTEGER",
           js_integer(-((int64_t)1 << 46)));
    js_set(ctx, number_ctor, "EPSILON", js_number(1));
    js_set_hidden(ctx, number_ctor, "prototype", js_object_value(np));
    js_set(ctx, global, "Number", js_object_value(number_ctor));

    struct js_object *boolean_ctor = js_new_object(ctx, CLASS_NATIVE);

    boolean_ctor->native = native_math;
    boolean_ctor->name = "Boolean";
    js_set(ctx, global, "Boolean", js_object_value(boolean_ctor));

    /* --- Math --- */
    struct js_object *math = js_new_object(ctx, CLASS_OBJECT);

    js_set_native(ctx, math, "abs", math_abs, 1);
    js_set_native(ctx, math, "floor", math_floor, 1);
    js_set_native(ctx, math, "ceil", math_ceil, 1);
    js_set_native(ctx, math, "round", math_round, 1);
    js_set_native(ctx, math, "trunc", math_trunc, 1);
    js_set_native(ctx, math, "sqrt", math_sqrt, 1);
    js_set_native(ctx, math, "sign", math_sign, 1);
    js_set_native(ctx, math, "min", math_min, 2);
    js_set_native(ctx, math, "max", math_max, 2);
    js_set_native(ctx, math, "pow", math_pow, 2);
    js_set_native(ctx, math, "random", math_random, 0);
    js_set(ctx, math, "PI", js_number(205887));       /* 3.14159 */
    js_set(ctx, math, "E", js_number(178145));        /* 2.71828 */
    js_set(ctx, math, "LN2", js_number(45426));       /* 0.69315 */
    js_set(ctx, math, "SQRT2", js_number(92682));     /* 1.41421 */
    js_set(ctx, global, "Math", js_object_value(math));

    /* --- JSON --- */
    struct js_object *json = js_new_object(ctx, CLASS_OBJECT);

    js_set_native(ctx, json, "stringify", native_json_stringify, 2);
    js_set_native(ctx, json, "parse", native_json_parse, 1);
    js_set(ctx, global, "JSON", js_object_value(json));

    /* --- Date --- */
    struct js_object *date_prototype = js_new_object(ctx, CLASS_OBJECT);

    js_set_native(ctx, date_prototype, "getFullYear", date_year, 0);
    js_set_native(ctx, date_prototype, "getMonth", date_month, 0);
    js_set_native(ctx, date_prototype, "getDate", date_day, 0);
    js_set_native(ctx, date_prototype, "getHours", date_hours, 0);
    js_set_native(ctx, date_prototype, "getMinutes", date_minutes, 0);
    js_set_native(ctx, date_prototype, "getSeconds", date_seconds, 0);
    js_set_native(ctx, date_prototype, "getTime", native_date_now, 0);

    struct js_object *date_ctor = js_new_object(ctx, CLASS_NATIVE);

    date_ctor->native = native_date_ctor;
    date_ctor->name = "Date";
    js_set_native(ctx, date_ctor, "now", native_date_now, 0);
    js_set_hidden(ctx, date_ctor, "prototype",
                  js_object_value(date_prototype));
    js_set(ctx, global, "Date", js_object_value(date_ctor));

    /* --- Freie Funktionen --- */
    js_set_native(ctx, global, "parseInt", native_parse_int, 2);
    js_set_native(ctx, global, "parseFloat", native_parse_float, 1);
    js_set_native(ctx, global, "isNaN", native_is_nan, 1);
    js_set_native(ctx, global, "isFinite", native_number_is_finite, 1);
    js_set_native(ctx, global, "setTimeout", native_set_timeout, 2);
    js_set_native(ctx, global, "setInterval", native_set_interval, 2);
    js_set_native(ctx, global, "clearTimeout", native_clear_timer, 1);
    js_set_native(ctx, global, "clearInterval", native_clear_timer, 1);
    js_set(ctx, global, "NaN", js_number(JS_NAN));
    js_set(ctx, global, "Infinity", js_number(JS_POS_INF));
    js_set(ctx, global, "undefined", js_undefined());
}
