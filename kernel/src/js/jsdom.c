/* jsdom.c - Anbindung des Dokumentbaums an den Deuter.
 *
 * Jeder Knoten bekommt bei Bedarf ein Huellobjekt. Damit ein Knoten
 * immer dasselbe Objekt erhaelt, merkt sich eine kleine Tabelle die
 * bereits gebauten Huellen.
 */

#include "jsint.h"
#include "htmlparse.h"
#include "kstring.h"
#include "mm.h"

static struct js_value node_getter(struct js_context *ctx,
                                   struct js_object *self, const char *name,
                                   bool *handled);
static bool node_setter(struct js_context *ctx, struct js_object *self,
                        const char *name, struct js_value value);

/* ------------------------------------------------------------------ */
/* Huellen                                                             */
/* ------------------------------------------------------------------ */

struct js_object *js_wrap_node(struct js_context *ctx, struct node *node)
{
    if (!node)
        return NULL;

    for (size_t i = 0; i < ctx->wrap_count; i++)
        if (ctx->wrap_nodes[i] == node)
            return ctx->wrap_objects[i];

    struct js_object *o = js_new_object(ctx, CLASS_NODE);

    if (!o)
        return NULL;
    o->dom = node;
    o->prototype = ctx->node_prototype;
    o->getter = node_getter;
    o->setter = node_setter;

    if (ctx->wrap_count < JS_WRAPPERS) {
        ctx->wrap_nodes[ctx->wrap_count] = node;
        ctx->wrap_objects[ctx->wrap_count] = o;
        ctx->wrap_count++;
    }
    return o;
}

static struct js_value wrap(struct js_context *ctx, struct node *node)
{
    struct js_object *o = js_wrap_node(ctx, node);

    return o ? js_object_value(o) : js_null();
}

static struct node *self_node(struct js_value self)
{
    if (self.type != JS_OBJECT || !self.as.object ||
        self.as.object->klass != CLASS_NODE)
        return NULL;
    return self.as.object->dom;
}

static struct js_object *new_node_list(struct js_context *ctx)
{
    struct js_object *o = js_new_object(ctx, CLASS_NODELIST);

    if (o)
        o->prototype = ctx->array_prototype;
    return o;
}

/* ------------------------------------------------------------------ */
/* Suche                                                               */
/* ------------------------------------------------------------------ */

static struct node *search_root(struct js_context *ctx, struct js_value self)
{
    struct node *node = self_node(self);

    if (node)
        return node;
    return ctx->document ? ctx->document->root : NULL;
}

static struct js_value native_get_by_id(struct js_context *ctx,
                                        struct js_value self,
                                        struct js_value *args, size_t count)
{
    struct js_string *id = js_to_string(ctx, count > 0 ? args[0]
                                                       : js_undefined());
    struct node *root = search_root(ctx, self);

    if (!root || !id)
        return js_null();
    return wrap(ctx, dom_by_id(root, id->data));
}

static void collect_tag(struct js_context *ctx, struct node *node,
                        const char *tag, struct js_object *out)
{
    if (node->kind == NODE_ELEMENT && node->name &&
        (strcmp(tag, "*") == 0 || strcasecmp(node->name, tag) == 0))
        js_array_push(ctx, out, wrap(ctx, node));
    for (struct node *c = node->first; c; c = c->next)
        collect_tag(ctx, c, tag, out);
}

static struct js_value native_get_by_tag(struct js_context *ctx,
                                         struct js_value self,
                                         struct js_value *args, size_t count)
{
    struct js_string *tag = js_to_string(ctx, count > 0 ? args[0]
                                                        : js_undefined());
    struct node *root = search_root(ctx, self);
    struct js_object *out = new_node_list(ctx);

    if (root && tag)
        for (struct node *c = root->first; c; c = c->next)
            collect_tag(ctx, c, tag->data, out);
    return js_object_value(out);
}

static void collect_class(struct js_context *ctx, struct node *node,
                          const char *name, struct js_object *out)
{
    if (node->kind == NODE_ELEMENT && node_has_class(node, name))
        js_array_push(ctx, out, wrap(ctx, node));
    for (struct node *c = node->first; c; c = c->next)
        collect_class(ctx, c, name, out);
}

static struct js_value native_get_by_class(struct js_context *ctx,
                                           struct js_value self,
                                           struct js_value *args, size_t count)
{
    struct js_string *name = js_to_string(ctx, count > 0 ? args[0]
                                                         : js_undefined());
    struct node *root = search_root(ctx, self);
    struct js_object *out = new_node_list(ctx);

    if (root && name)
        for (struct node *c = root->first; c; c = c->next)
            collect_class(ctx, c, name->data, out);
    return js_object_value(out);
}

static struct js_value native_query(struct js_context *ctx,
                                    struct js_value self,
                                    struct js_value *args, size_t count)
{
    struct js_string *selector = js_to_string(ctx, count > 0 ? args[0]
                                                             : js_undefined());
    struct node *root = search_root(ctx, self);
    struct node *found[1];

    if (!root || !selector)
        return js_null();
    if (dom_query(root, selector->data, found, 1) == 0)
        return js_null();
    return wrap(ctx, found[0]);
}

static struct js_value native_query_all(struct js_context *ctx,
                                        struct js_value self,
                                        struct js_value *args, size_t count)
{
    struct js_string *selector = js_to_string(ctx, count > 0 ? args[0]
                                                             : js_undefined());
    struct node *root = search_root(ctx, self);
    struct js_object *out = new_node_list(ctx);
    struct node *found[128];

    if (root && selector) {
        size_t n = dom_query(root, selector->data, found, ARRAY_LEN(found));

        for (size_t i = 0; i < n; i++)
            js_array_push(ctx, out, wrap(ctx, found[i]));
    }
    return js_object_value(out);
}

/* ------------------------------------------------------------------ */
/* Aendern                                                             */
/* ------------------------------------------------------------------ */

static struct js_value native_create_element(struct js_context *ctx,
                                             struct js_value self,
                                             struct js_value *args,
                                             size_t count)
{
    struct js_string *name = js_to_string(ctx, count > 0 ? args[0]
                                                         : js_undefined());

    UNUSED(self);
    if (!name)
        return js_null();

    struct node *node = node_create(NODE_ELEMENT, name->data);

    if (!node)
        return js_null();
    if (ctx->document)
        node->id = ctx->document->next_id++;
    return wrap(ctx, node);
}

static struct js_value native_create_text(struct js_context *ctx,
                                          struct js_value self,
                                          struct js_value *args, size_t count)
{
    struct js_string *text = js_to_string(ctx, count > 0 ? args[0]
                                                         : js_undefined());
    struct node *node = node_create(NODE_TEXT, NULL);

    UNUSED(self);
    if (!node)
        return js_null();
    node->text = kmalloc(text->length + 1);
    if (node->text)
        memcpy(node->text, text->data, text->length + 1);
    if (ctx->document)
        node->id = ctx->document->next_id++;
    return wrap(ctx, node);
}

static struct js_value native_append(struct js_context *ctx,
                                     struct js_value self,
                                     struct js_value *args, size_t count)
{
    struct node *parent = self_node(self);
    struct node *child = count > 0 ? self_node(args[0]) : NULL;

    if (!parent || !child)
        return js_undefined();
    node_remove(child);
    node_append(parent, child);
    ctx->dirty = true;
    return args[0];
}

static struct js_value native_insert_before(struct js_context *ctx,
                                            struct js_value self,
                                            struct js_value *args, size_t count)
{
    struct node *parent = self_node(self);
    struct node *child = count > 0 ? self_node(args[0]) : NULL;
    struct node *before = count > 1 ? self_node(args[1]) : NULL;

    if (!parent || !child)
        return js_undefined();
    node_remove(child);
    node_insert_before(parent, child, before);
    ctx->dirty = true;
    return args[0];
}

static struct js_value native_remove_child(struct js_context *ctx,
                                           struct js_value self,
                                           struct js_value *args, size_t count)
{
    struct node *child = count > 0 ? self_node(args[0]) : NULL;

    UNUSED(self);
    if (!child)
        return js_undefined();
    node_remove(child);
    ctx->dirty = true;
    return args[0];
}

static struct js_value native_remove(struct js_context *ctx,
                                     struct js_value self,
                                     struct js_value *args, size_t count)
{
    struct node *node = self_node(self);

    UNUSED(args);
    UNUSED(count);
    if (node) {
        node_remove(node);
        ctx->dirty = true;
    }
    return js_undefined();
}

static struct js_value native_get_attribute(struct js_context *ctx,
                                            struct js_value self,
                                            struct js_value *args, size_t count)
{
    struct node *node = self_node(self);
    struct js_string *name = js_to_string(ctx, count > 0 ? args[0]
                                                         : js_undefined());

    if (!node || !name)
        return js_null();

    const char *value = node_attribute(node, name->data);

    return value ? js_str(ctx, value) : js_null();
}

static struct js_value native_set_attribute(struct js_context *ctx,
                                            struct js_value self,
                                            struct js_value *args, size_t count)
{
    struct node *node = self_node(self);
    struct js_string *name = js_to_string(ctx, count > 0 ? args[0]
                                                         : js_undefined());
    struct js_string *value = js_to_string(ctx, count > 1 ? args[1]
                                                          : js_undefined());

    if (node && name) {
        node_set_attribute(node, name->data, value ? value->data : "");
        ctx->dirty = true;
    }
    return js_undefined();
}

static struct js_value native_remove_attribute(struct js_context *ctx,
                                               struct js_value self,
                                               struct js_value *args,
                                               size_t count)
{
    struct node *node = self_node(self);
    struct js_string *name = js_to_string(ctx, count > 0 ? args[0]
                                                         : js_undefined());

    if (node && name) {
        node_remove_attribute(node, name->data);
        ctx->dirty = true;
    }
    return js_undefined();
}

static struct js_value native_has_attribute(struct js_context *ctx,
                                            struct js_value self,
                                            struct js_value *args, size_t count)
{
    struct node *node = self_node(self);
    struct js_string *name = js_to_string(ctx, count > 0 ? args[0]
                                                         : js_undefined());

    if (!node || !name)
        return js_bool(false);
    return js_bool(node_attribute(node, name->data) != NULL);
}

/* ------------------------------------------------------------------ */
/* Ereignisse                                                          */
/* ------------------------------------------------------------------ */

/* Behandlungen werden als Eigenschaft am Huellobjekt abgelegt. Der
 * Browser ruft sie ueber js_dispatch auf. */
static struct js_value native_add_listener(struct js_context *ctx,
                                           struct js_value self,
                                           struct js_value *args, size_t count)
{
    struct js_string *type = js_to_string(ctx, count > 0 ? args[0]
                                                         : js_undefined());

    if (self.type != JS_OBJECT || !self.as.object || !type || count < 2)
        return js_undefined();

    char key[64];

    ksnprintf(key, sizeof(key), "__on_%s", type->data);

    struct js_value existing = js_get(ctx, self.as.object, key);
    struct js_object *list;

    if (existing.type == JS_OBJECT && existing.as.object &&
        existing.as.object->klass == CLASS_ARRAY) {
        list = existing.as.object;
    } else {
        list = js_new_array(ctx, 0);
        js_set_hidden(ctx, self.as.object, key, js_object_value(list));
    }
    js_array_push(ctx, list, args[1]);
    return js_undefined();
}

static struct js_value native_remove_listener(struct js_context *ctx,
                                              struct js_value self,
                                              struct js_value *args,
                                              size_t count)
{
    struct js_string *type = js_to_string(ctx, count > 0 ? args[0]
                                                         : js_undefined());

    if (self.type != JS_OBJECT || !self.as.object || !type || count < 2)
        return js_undefined();

    char key[64];

    ksnprintf(key, sizeof(key), "__on_%s", type->data);

    struct js_value existing = js_get(ctx, self.as.object, key);

    if (existing.type != JS_OBJECT || !existing.as.object)
        return js_undefined();

    struct js_object *list = existing.as.object;

    for (size_t i = 0; i < list->length; i++) {
        if (!js_equals(ctx, list->elements[i], args[1], true))
            continue;
        for (size_t j = i + 1; j < list->length; j++)
            list->elements[j - 1] = list->elements[j];
        list->length--;
        break;
    }
    return js_undefined();
}

/* Ruft alle Behandlungen eines Knotens fuer eine Art auf. */
bool js_dispatch_event(struct js_context *ctx, struct node *node,
                       const char *type)
{
    if (!ctx || !node)
        return false;

    struct js_object *wrapper = NULL;

    for (size_t i = 0; i < ctx->wrap_count; i++)
        if (ctx->wrap_nodes[i] == node)
            wrapper = ctx->wrap_objects[i];

    bool ran = false;
    char key[64];

    ksnprintf(key, sizeof(key), "__on_%s", type);

    struct js_object *event = js_new_object(ctx, CLASS_OBJECT);

    js_set(ctx, event, "type", js_str(ctx, type));
    js_set(ctx, event, "target", wrap(ctx, node));
    js_set_native(ctx, event, "preventDefault", NULL, 0);
    js_set_native(ctx, event, "stopPropagation", NULL, 0);

    struct js_value argument = js_object_value(event);

    /* Erst die im Baum als Attribut hinterlegte Behandlung. */
    char attribute[16];

    ksnprintf(attribute, sizeof(attribute), "on%s", type);

    const char *inline_code = node_attribute(node, attribute);

    if (inline_code && *inline_code) {
        js_run_handler(ctx, inline_code, node);
        ran = true;
    }

    if (wrapper) {
        struct js_value handler = js_get(ctx, wrapper, attribute);

        if (handler.type == JS_OBJECT && handler.as.object &&
            (handler.as.object->klass == CLASS_FUNCTION ||
             handler.as.object->klass == CLASS_NATIVE)) {
            ctx->signal = SIGNAL_NONE;
            js_call(ctx, handler, js_object_value(wrapper), &argument, 1);
            ctx->signal = SIGNAL_NONE;
            ran = true;
        }

        struct js_value list = js_get(ctx, wrapper, key);

        if (list.type == JS_OBJECT && list.as.object &&
            list.as.object->klass == CLASS_ARRAY) {
            struct js_object *a = list.as.object;

            for (size_t i = 0; i < a->length; i++) {
                ctx->signal = SIGNAL_NONE;
                ctx->steps = 0;
                js_call(ctx, a->elements[i], js_object_value(wrapper),
                        &argument, 1);
                ctx->signal = SIGNAL_NONE;
                ran = true;
            }
        }
    }

    if (ran)
        js_flush_changes(ctx);
    return ran;
}

/* ------------------------------------------------------------------ */
/* Stil und Klassenliste                                               */
/* ------------------------------------------------------------------ */

/* Setzt eine Eigenschaft im style-Attribut des Knotens. */
static void style_write(struct node *node, const char *name, const char *value)
{
    const char *current = node_attribute(node, "style");
    char buffer[512];
    size_t at = 0;

    buffer[0] = '\0';

    /* Vorhandene Eigenschaften uebernehmen, die gesuchte auslassen. */
    if (current) {
        const char *p = current;

        while (*p) {
            while (*p == ' ' || *p == ';')
                p++;
            if (!*p)
                break;

            const char *start = p;

            while (*p && *p != ';')
                p++;

            size_t length = (size_t)(p - start);
            const char *colon = start;

            while (colon < start + length && *colon != ':')
                colon++;

            size_t name_length = (size_t)(colon - start);

            while (name_length > 0 && start[name_length - 1] == ' ')
                name_length--;
            if (name_length == strlen(name) &&
                strncasecmp(start, name, name_length) == 0)
                continue;
            if (at + length + 2 < sizeof(buffer)) {
                memcpy(buffer + at, start, length);
                at += length;
                buffer[at++] = ';';
                buffer[at] = '\0';
            }
        }
    }

    if (value && *value) {
        size_t need = strlen(name) + strlen(value) + 3;

        if (at + need < sizeof(buffer)) {
            ksnprintf(buffer + at, sizeof(buffer) - at, "%s:%s;", name, value);
        }
    }
    node_set_attribute(node, "style", buffer);
}

/* Uebersetzt hoehereGewalt in hoehere-gewalt. */
static void camel_to_dashed(const char *name, char *out, size_t size)
{
    size_t at = 0;

    for (const char *p = name; *p && at + 2 < size; p++) {
        if (*p >= 'A' && *p <= 'Z') {
            out[at++] = '-';
            out[at++] = (char)(*p + 32);
        } else {
            out[at++] = *p;
        }
    }
    out[at] = '\0';
}

static struct js_value style_getter(struct js_context *ctx,
                                    struct js_object *self, const char *name,
                                    bool *handled)
{
    struct node *node = self->dom;

    if (!node || strncmp(name, "__", 2) == 0)
        return js_undefined();

    const char *current = node_attribute(node, "style");

    if (!current)
        return js_undefined();

    char wanted[96];

    camel_to_dashed(name, wanted, sizeof(wanted));

    const char *p = current;

    while (*p) {
        while (*p == ' ' || *p == ';')
            p++;
        if (!*p)
            break;

        const char *start = p;

        while (*p && *p != ';')
            p++;

        const char *colon = start;

        while (colon < p && *colon != ':')
            colon++;
        if (colon >= p)
            continue;

        size_t name_length = (size_t)(colon - start);

        while (name_length > 0 && start[name_length - 1] == ' ')
            name_length--;

        if (name_length != strlen(wanted) ||
            strncasecmp(start, wanted, name_length) != 0)
            continue;

        const char *value = colon + 1;

        while (value < p && *value == ' ')
            value++;

        size_t value_length = (size_t)(p - value);

        while (value_length > 0 && value[value_length - 1] == ' ')
            value_length--;
        *handled = true;
        return js_strn(ctx, value, value_length);
    }
    *handled = true;
    return js_str(ctx, "");
}

static bool style_setter(struct js_context *ctx, struct js_object *self,
                         const char *name, struct js_value value)
{
    struct node *node = self->dom;

    if (!node || strncmp(name, "__", 2) == 0)
        return false;

    char dashed[96];
    struct js_string *text = js_to_string(ctx, value);

    camel_to_dashed(name, dashed, sizeof(dashed));
    style_write(node, dashed, text ? text->data : "");
    ctx->dirty = true;
    return true;
}

static struct js_object *style_object(struct js_context *ctx, struct node *node)
{
    struct js_object *o = js_new_object(ctx, CLASS_STYLE);

    if (!o)
        return NULL;
    o->dom = node;
    o->getter = style_getter;
    o->setter = style_setter;
    return o;
}

/* --- classList --- */

static struct js_value class_add(struct js_context *ctx, struct js_value self,
                                 struct js_value *args, size_t count)
{
    struct node *node = self.type == JS_OBJECT && self.as.object
                        ? self.as.object->dom : NULL;

    if (!node)
        return js_undefined();
    for (size_t i = 0; i < count; i++) {
        struct js_string *name = js_to_string(ctx, args[i]);

        if (!name || node_has_class(node, name->data))
            continue;

        const char *current = node_attribute(node, "class");
        char buffer[256];

        if (current && *current)
            ksnprintf(buffer, sizeof(buffer), "%s %s", current, name->data);
        else
            strlcpy(buffer, name->data, sizeof(buffer));
        node_set_attribute(node, "class", buffer);
        ctx->dirty = true;
    }
    return js_undefined();
}

static struct js_value class_remove(struct js_context *ctx,
                                    struct js_value self,
                                    struct js_value *args, size_t count)
{
    struct node *node = self.type == JS_OBJECT && self.as.object
                        ? self.as.object->dom : NULL;

    if (!node)
        return js_undefined();

    for (size_t i = 0; i < count; i++) {
        struct js_string *name = js_to_string(ctx, args[i]);
        const char *current = node_attribute(node, "class");

        if (!name || !current)
            continue;

        char buffer[256];
        size_t at = 0;
        const char *p = current;

        buffer[0] = '\0';
        while (*p) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;

            const char *start = p;

            while (*p && *p != ' ')
                p++;

            size_t length = (size_t)(p - start);

            if (length == name->length &&
                strncmp(start, name->data, length) == 0)
                continue;
            if (at && at + 1 < sizeof(buffer))
                buffer[at++] = ' ';
            if (at + length + 1 < sizeof(buffer)) {
                memcpy(buffer + at, start, length);
                at += length;
            }
            buffer[at] = '\0';
        }
        node_set_attribute(node, "class", buffer);
        ctx->dirty = true;
    }
    return js_undefined();
}

static struct js_value class_contains(struct js_context *ctx,
                                      struct js_value self,
                                      struct js_value *args, size_t count)
{
    struct node *node = self.type == JS_OBJECT && self.as.object
                        ? self.as.object->dom : NULL;
    struct js_string *name = js_to_string(ctx, count > 0 ? args[0]
                                                         : js_undefined());

    if (!node || !name)
        return js_bool(false);
    return js_bool(node_has_class(node, name->data));
}

static struct js_value class_toggle(struct js_context *ctx,
                                    struct js_value self,
                                    struct js_value *args, size_t count)
{
    struct js_value has = class_contains(ctx, self, args, count);

    if (js_truthy(has))
        class_remove(ctx, self, args, count);
    else
        class_add(ctx, self, args, count);
    return js_bool(!js_truthy(has));
}

static struct js_object *class_list_object(struct js_context *ctx,
                                           struct node *node)
{
    struct js_object *o = js_new_object(ctx, CLASS_CLASSLIST);

    if (!o)
        return NULL;
    o->dom = node;
    js_set_native(ctx, o, "add", class_add, 1);
    js_set_native(ctx, o, "remove", class_remove, 1);
    js_set_native(ctx, o, "contains", class_contains, 1);
    js_set_native(ctx, o, "toggle", class_toggle, 1);
    return o;
}

/* ------------------------------------------------------------------ */
/* Eigenschaften eines Knotens                                         */
/* ------------------------------------------------------------------ */

/* Baut den HTML-Text eines Teilbaums. */
static void serialize(struct node *node, char *out, size_t size, size_t *at,
                      bool include_self)
{
    if (node->kind == NODE_TEXT) {
        if (node->text) {
            size_t length = strlen(node->text);

            for (size_t i = 0; i < length && *at + 1 < size; i++)
                out[(*at)++] = node->text[i];
            out[*at] = '\0';
        }
        return;
    }
    if (node->kind == NODE_COMMENT)
        return;

    if (include_self && node->kind == NODE_ELEMENT && node->name) {
        int written = ksnprintf(out + *at, size - *at, "<%s", node->name);

        if (written > 0)
            *at += (size_t)written;
        for (struct attribute *a = node->attributes; a; a = a->next) {
            written = ksnprintf(out + *at, size - *at, " %s=\"%s\"",
                                a->name, a->value ? a->value : "");
            if (written > 0)
                *at += (size_t)written;
        }
        if (*at + 2 < size) {
            out[(*at)++] = '>';
            out[*at] = '\0';
        }
    }

    for (struct node *c = node->first; c; c = c->next)
        serialize(c, out, size, at, true);

    if (include_self && node->kind == NODE_ELEMENT && node->name) {
        int written = ksnprintf(out + *at, size - *at, "</%s>", node->name);

        if (written > 0)
            *at += (size_t)written;
    }
}

static struct js_value node_getter(struct js_context *ctx,
                                   struct js_object *self, const char *name,
                                   bool *handled)
{
    struct node *node = self->dom;

    if (!node)
        return js_undefined();
    *handled = true;

    if (strcmp(name, "tagName") == 0 || strcmp(name, "nodeName") == 0) {
        if (node->kind != NODE_ELEMENT || !node->name)
            return js_str(ctx, "#text");

        char upper[64];

        strlcpy(upper, node->name, sizeof(upper));
        for (char *p = upper; *p; p++)
            if (*p >= 'a' && *p <= 'z')
                *p = (char)(*p - 32);
        return js_str(ctx, upper);
    }
    if (strcmp(name, "nodeType") == 0)
        return js_integer(node->kind == NODE_ELEMENT ? 1 :
                          (node->kind == NODE_TEXT ? 3 : 8));
    if (strcmp(name, "id") == 0) {
        const char *value = node_attribute(node, "id");

        return js_str(ctx, value ? value : "");
    }
    if (strcmp(name, "className") == 0) {
        const char *value = node_attribute(node, "class");

        return js_str(ctx, value ? value : "");
    }
    if (strcmp(name, "classList") == 0)
        return js_object_value(class_list_object(ctx, node));
    if (strcmp(name, "style") == 0)
        return js_object_value(style_object(ctx, node));
    if (strcmp(name, "textContent") == 0 || strcmp(name, "innerText") == 0 ||
        strcmp(name, "nodeValue") == 0 || strcmp(name, "data") == 0) {
        if (node->kind == NODE_TEXT)
            return js_str(ctx, node->text ? node->text : "");

        char buffer[4096];

        dom_text_content(node, buffer, sizeof(buffer));
        return js_str(ctx, buffer);
    }
    if (strcmp(name, "innerHTML") == 0 || strcmp(name, "outerHTML") == 0) {
        char *buffer = js_alloc(ctx, 16384);
        size_t at = 0;

        if (!buffer)
            return js_str(ctx, "");
        buffer[0] = '\0';
        serialize(node, buffer, 16384, &at, strcmp(name, "outerHTML") == 0);
        return js_strn(ctx, buffer, at);
    }
    if (strcmp(name, "value") == 0) {
        if (node->value)
            return js_str(ctx, node->value);

        const char *value = node_attribute(node, "value");

        return js_str(ctx, value ? value : "");
    }
    if (strcmp(name, "checked") == 0)
        return js_bool(node->checked);
    if (strcmp(name, "href") == 0 || strcmp(name, "src") == 0 ||
        strcmp(name, "alt") == 0 || strcmp(name, "title") == 0 ||
        strcmp(name, "type") == 0 || strcmp(name, "name") == 0 ||
        strcmp(name, "placeholder") == 0) {
        const char *value = node_attribute(node, name);

        return js_str(ctx, value ? value : "");
    }
    if (strcmp(name, "parentNode") == 0 || strcmp(name, "parentElement") == 0)
        return wrap(ctx, node->parent);
    if (strcmp(name, "firstChild") == 0)
        return wrap(ctx, node->first);
    if (strcmp(name, "lastChild") == 0)
        return wrap(ctx, node->last);
    if (strcmp(name, "nextSibling") == 0)
        return wrap(ctx, node->next);
    if (strcmp(name, "previousSibling") == 0)
        return wrap(ctx, node->previous);
    if (strcmp(name, "firstElementChild") == 0) {
        for (struct node *c = node->first; c; c = c->next)
            if (c->kind == NODE_ELEMENT)
                return wrap(ctx, c);
        return js_null();
    }
    if (strcmp(name, "childNodes") == 0 || strcmp(name, "children") == 0) {
        struct js_object *out = new_node_list(ctx);
        bool elements_only = strcmp(name, "children") == 0;

        for (struct node *c = node->first; c; c = c->next)
            if (!elements_only || c->kind == NODE_ELEMENT)
                js_array_push(ctx, out, wrap(ctx, c));
        return js_object_value(out);
    }
    if (strcmp(name, "offsetWidth") == 0 || strcmp(name, "clientWidth") == 0)
        return js_integer(node->has_box ? node->box.w : 0);
    if (strcmp(name, "offsetHeight") == 0 || strcmp(name, "clientHeight") == 0)
        return js_integer(node->has_box ? node->box.h : 0);
    if (strcmp(name, "offsetTop") == 0)
        return js_integer(node->has_box ? node->box.y : 0);
    if (strcmp(name, "offsetLeft") == 0)
        return js_integer(node->has_box ? node->box.x : 0);
    if (strcmp(name, "dataset") == 0) {
        struct js_object *out = js_new_object(ctx, CLASS_OBJECT);

        for (struct attribute *a = node->attributes; a; a = a->next)
            if (strncmp(a->name, "data-", 5) == 0)
                js_set(ctx, out, a->name + 5, js_str(ctx, a->value));
        return js_object_value(out);
    }

    *handled = false;
    return js_undefined();
}

static bool node_setter(struct js_context *ctx, struct js_object *self,
                        const char *name, struct js_value value)
{
    struct node *node = self->dom;

    if (!node)
        return false;
    if (strncmp(name, "__", 2) == 0)
        return false;

    struct js_string *text = js_to_string(ctx, value);
    const char *s = text ? text->data : "";

    if (strcmp(name, "textContent") == 0 || strcmp(name, "innerText") == 0 ||
        strcmp(name, "nodeValue") == 0) {
        if (node->kind == NODE_TEXT) {
            kfree(node->text);
            node->text = kmalloc(text->length + 1);
            if (node->text)
                memcpy(node->text, s, text->length + 1);
        } else {
            html_set_inner(node, NULL);

            struct node *child = node_create(NODE_TEXT, NULL);

            if (child) {
                child->text = kmalloc(text->length + 1);
                if (child->text)
                    memcpy(child->text, s, text->length + 1);
                child->id = ctx->document ? ctx->document->next_id++ : 0;
                node_append(node, child);
            }
        }
        ctx->dirty = true;
        return true;
    }
    if (strcmp(name, "innerHTML") == 0) {
        html_set_inner(node, s);
        ctx->dirty = true;
        return true;
    }
    if (strcmp(name, "id") == 0) {
        node_set_attribute(node, "id", s);
        ctx->dirty = true;
        return true;
    }
    if (strcmp(name, "className") == 0) {
        node_set_attribute(node, "class", s);
        ctx->dirty = true;
        return true;
    }
    if (strcmp(name, "value") == 0) {
        kfree(node->value);
        node->value = kmalloc(text->length + 1);
        if (node->value)
            memcpy(node->value, s, text->length + 1);
        ctx->dirty = true;
        return true;
    }
    if (strcmp(name, "checked") == 0) {
        node->checked = js_truthy(value);
        ctx->dirty = true;
        return true;
    }
    if (strcmp(name, "href") == 0 || strcmp(name, "src") == 0 ||
        strcmp(name, "alt") == 0 || strcmp(name, "title") == 0 ||
        strcmp(name, "type") == 0 || strcmp(name, "name") == 0 ||
        strcmp(name, "placeholder") == 0) {
        node_set_attribute(node, name, s);
        ctx->dirty = true;
        return true;
    }
    if (strcmp(name, "style") == 0) {
        node_set_attribute(node, "style", s);
        ctx->dirty = true;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* window und document                                                 */
/* ------------------------------------------------------------------ */

static struct js_value native_alert(struct js_context *ctx,
                                    struct js_value self,
                                    struct js_value *args, size_t count)
{
    struct js_string *s = js_to_string(ctx, count > 0 ? args[0]
                                                      : js_undefined());
    char line[320];

    UNUSED(self);
    ksnprintf(line, sizeof(line), "Hinweis: %s", s ? s->data : "");
    js_console_write(ctx, line);
    return js_undefined();
}

static struct js_value native_navigate(struct js_context *ctx,
                                       struct js_value self,
                                       struct js_value *args, size_t count)
{
    struct js_string *url = js_to_string(ctx, count > 0 ? args[0]
                                                        : js_undefined());

    UNUSED(self);
    if (url && ctx->navigate_hook)
        ctx->navigate_hook(ctx->navigate_context, url->data);
    return js_undefined();
}

static bool location_setter(struct js_context *ctx, struct js_object *self,
                            const char *name, struct js_value value)
{
    UNUSED(self);
    if (strcmp(name, "href") != 0)
        return false;

    struct js_string *url = js_to_string(ctx, value);

    if (url && ctx->navigate_hook)
        ctx->navigate_hook(ctx->navigate_context, url->data);
    return true;
}

static struct js_value native_noop(struct js_context *ctx,
                                   struct js_value self,
                                   struct js_value *args, size_t count)
{
    UNUSED(ctx);
    UNUSED(self);
    UNUSED(args);
    UNUSED(count);
    return js_undefined();
}

void js_install_dom(struct js_context *ctx)
{
    /* --- Der gemeinsame Prototyp aller Knoten --- */
    struct js_object *np = js_new_object(ctx, CLASS_OBJECT);

    ctx->node_prototype = np;

    js_set_native(ctx, np, "getElementById", native_get_by_id, 1);
    js_set_native(ctx, np, "getElementsByTagName", native_get_by_tag, 1);
    js_set_native(ctx, np, "getElementsByClassName", native_get_by_class, 1);
    js_set_native(ctx, np, "querySelector", native_query, 1);
    js_set_native(ctx, np, "querySelectorAll", native_query_all, 1);
    js_set_native(ctx, np, "appendChild", native_append, 1);
    js_set_native(ctx, np, "append", native_append, 1);
    js_set_native(ctx, np, "insertBefore", native_insert_before, 2);
    js_set_native(ctx, np, "removeChild", native_remove_child, 1);
    js_set_native(ctx, np, "remove", native_remove, 0);
    js_set_native(ctx, np, "getAttribute", native_get_attribute, 1);
    js_set_native(ctx, np, "setAttribute", native_set_attribute, 2);
    js_set_native(ctx, np, "removeAttribute", native_remove_attribute, 1);
    js_set_native(ctx, np, "hasAttribute", native_has_attribute, 1);
    js_set_native(ctx, np, "addEventListener", native_add_listener, 2);
    js_set_native(ctx, np, "removeEventListener", native_remove_listener, 2);
    js_set_native(ctx, np, "focus", native_noop, 0);
    js_set_native(ctx, np, "blur", native_noop, 0);
    js_set_native(ctx, np, "scrollIntoView", native_noop, 0);
    np->prototype = ctx->object_prototype;

    /* --- window --- */
    struct js_object *global = ctx->global;

    js_set_native(ctx, global, "alert", native_alert, 1);
    js_set(ctx, global, "window", js_object_value(global));
    js_set(ctx, global, "self", js_object_value(global));
    js_set(ctx, global, "globalThis", js_object_value(global));
    js_set_native(ctx, global, "addEventListener", native_add_listener, 2);
    js_set_native(ctx, global, "removeEventListener", native_remove_listener, 2);
    js_set_native(ctx, global, "requestAnimationFrame", native_noop, 1);
    js_set_native(ctx, global, "scrollTo", native_noop, 2);

    struct js_object *navigator = js_new_object(ctx, CLASS_OBJECT);

    js_set(ctx, navigator, "userAgent", js_str(ctx, "RetroOS/1.0"));
    js_set(ctx, navigator, "language", js_str(ctx, "de"));
    js_set(ctx, navigator, "platform", js_str(ctx, "RetroOS"));
    js_set(ctx, global, "navigator", js_object_value(navigator));

    struct js_object *location = js_new_object(ctx, CLASS_OBJECT);

    location->setter = location_setter;
    js_set_native(ctx, location, "assign", native_navigate, 1);
    js_set_native(ctx, location, "replace", native_navigate, 1);
    js_set_native(ctx, location, "reload", native_noop, 0);
    js_set(ctx, global, "location", js_object_value(location));
}

void js_bind_document(struct js_context *ctx, struct document *doc)
{
    ctx->document = doc;
    if (!doc || !doc->root)
        return;

    struct js_object *document = js_wrap_node(ctx, doc->root);

    ctx->document_object = document;

    js_set(ctx, document, "documentElement",
           wrap(ctx, doc->html ? doc->html : doc->root));
    js_set(ctx, document, "body", wrap(ctx, doc->body));
    js_set(ctx, document, "head", wrap(ctx, doc->head));
    js_set(ctx, document, "title", js_str(ctx, doc->title));
    js_set_native(ctx, document, "createElement", native_create_element, 1);
    js_set_native(ctx, document, "createTextNode", native_create_text, 1);
    js_set_native(ctx, document, "write", native_noop, 1);
    js_set(ctx, ctx->global, "document", js_object_value(document));
}
