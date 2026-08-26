/* dom.c - Aufbau und Durchsuchen des Dokumentbaums. */

#include "dom.h"
#include "kstring.h"
#include "mm.h"

static char *dup_string(const char *s)
{
    if (!s)
        return NULL;

    size_t length = strlen(s);
    char *copy = kmalloc(length + 1);

    if (copy)
        memcpy(copy, s, length + 1);
    return copy;
}

static char *dup_lower(const char *s)
{
    char *copy = dup_string(s);

    for (char *p = copy; p && *p; p++)
        if (*p >= 'A' && *p <= 'Z')
            *p = (char)(*p + 32);
    return copy;
}

/* ------------------------------------------------------------------ */
/* Aufbau                                                              */
/* ------------------------------------------------------------------ */

struct node *node_create(enum node_kind kind, const char *name)
{
    struct node *node = kmalloc(sizeof(*node));

    if (!node)
        return NULL;
    memset(node, 0, sizeof(*node));
    node->kind = kind;
    if (name)
        node->name = dup_lower(name);
    return node;
}

void node_append(struct node *parent, struct node *child)
{
    if (!parent || !child)
        return;

    child->parent = parent;
    child->previous = parent->last;
    child->next = NULL;
    if (parent->last)
        parent->last->next = child;
    else
        parent->first = child;
    parent->last = child;
}

void node_insert_before(struct node *parent, struct node *child,
                        struct node *before)
{
    if (!parent || !child)
        return;
    if (!before || before->parent != parent) {
        node_append(parent, child);
        return;
    }

    child->parent = parent;
    child->next = before;
    child->previous = before->previous;
    if (before->previous)
        before->previous->next = child;
    else
        parent->first = child;
    before->previous = child;
}

void node_remove(struct node *child)
{
    if (!child || !child->parent)
        return;

    struct node *parent = child->parent;

    if (child->previous)
        child->previous->next = child->next;
    else
        parent->first = child->next;
    if (child->next)
        child->next->previous = child->previous;
    else
        parent->last = child->previous;

    child->parent = NULL;
    child->previous = NULL;
    child->next = NULL;
}

void node_free(struct node *node)
{
    if (!node)
        return;

    struct node *child = node->first;

    while (child) {
        struct node *next = child->next;

        node_free(child);
        child = next;
    }

    struct attribute *attr = node->attributes;

    while (attr) {
        struct attribute *next = attr->next;

        kfree(attr->name);
        kfree(attr->value);
        kfree(attr);
        attr = next;
    }

    kfree(node->name);
    kfree(node->text);
    kfree(node->value);
    kfree(node->on_click);
    kfree(node->on_change);
    kfree(node->on_input);
    kfree(node->on_submit);
    kfree(node);
}

/* ------------------------------------------------------------------ */
/* Attribute                                                           */
/* ------------------------------------------------------------------ */

const char *node_attribute(const struct node *node, const char *name)
{
    if (!node)
        return NULL;
    for (const struct attribute *a = node->attributes; a; a = a->next)
        if (strcasecmp(a->name, name) == 0)
            return a->value;
    return NULL;
}

void node_set_attribute(struct node *node, const char *name, const char *value)
{
    if (!node || !name)
        return;

    for (struct attribute *a = node->attributes; a; a = a->next) {
        if (strcasecmp(a->name, name) != 0)
            continue;
        kfree(a->value);
        a->value = dup_string(value ? value : "");
        return;
    }

    struct attribute *a = kmalloc(sizeof(*a));

    if (!a)
        return;
    a->name = dup_lower(name);
    a->value = dup_string(value ? value : "");
    a->next = node->attributes;
    node->attributes = a;
}

void node_remove_attribute(struct node *node, const char *name)
{
    if (!node)
        return;

    struct attribute **link = &node->attributes;

    while (*link) {
        if (strcasecmp((*link)->name, name) == 0) {
            struct attribute *dead = *link;

            *link = dead->next;
            kfree(dead->name);
            kfree(dead->value);
            kfree(dead);
            return;
        }
        link = &(*link)->next;
    }
}

bool node_has_class(const struct node *node, const char *class_name)
{
    const char *list = node_attribute(node, "class");

    if (!list || !class_name || !*class_name)
        return false;

    size_t want = strlen(class_name);
    const char *p = list;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;

        const char *start = p;

        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
            p++;
        if ((size_t)(p - start) == want &&
            strncmp(start, class_name, want) == 0)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Suche                                                               */
/* ------------------------------------------------------------------ */

struct node *dom_by_id(struct node *root, const char *id)
{
    if (!root || !id)
        return NULL;

    if (root->kind == NODE_ELEMENT) {
        const char *value = node_attribute(root, "id");

        if (value && strcmp(value, id) == 0)
            return root;
    }
    for (struct node *c = root->first; c; c = c->next) {
        struct node *found = dom_by_id(c, id);

        if (found)
            return found;
    }
    return NULL;
}

static struct node *by_tag_walk(struct node *root, const char *tag,
                                size_t *countdown)
{
    if (root->kind == NODE_ELEMENT && root->name &&
        (strcmp(tag, "*") == 0 || strcmp(root->name, tag) == 0)) {
        if (*countdown == 0)
            return root;
        (*countdown)--;
    }
    for (struct node *c = root->first; c; c = c->next) {
        struct node *found = by_tag_walk(c, tag, countdown);

        if (found)
            return found;
    }
    return NULL;
}

struct node *dom_by_tag(struct node *root, const char *tag, size_t index)
{
    if (!root || !tag)
        return NULL;

    size_t countdown = index;

    return by_tag_walk(root, tag, &countdown);
}

size_t dom_count_tag(struct node *root, const char *tag)
{
    if (!root)
        return 0;

    size_t total = 0;

    if (root->kind == NODE_ELEMENT && root->name &&
        (strcmp(tag, "*") == 0 || strcmp(root->name, tag) == 0))
        total++;
    for (struct node *c = root->first; c; c = c->next)
        total += dom_count_tag(c, tag);
    return total;
}

/* Prueft einen einzelnen Bestandteil wie "div", ".klasse" oder "#kennung". */
static bool matches_simple(const struct node *node, const char *selector)
{
    if (node->kind != NODE_ELEMENT)
        return false;

    char part[96];
    size_t at = 0;
    const char *p = selector;

    /* Der Selektor kann mehrere Bestandteile aneinanderhaengen. */
    while (*p) {
        char kind = 0;

        if (*p == '.' || *p == '#') {
            kind = *p++;
        } else if (*p == '*') {
            p++;
            continue;
        }

        at = 0;
        while (*p && *p != '.' && *p != '#' && at + 1 < sizeof(part))
            part[at++] = *p++;
        part[at] = '\0';
        if (at == 0)
            return false;

        if (kind == '.') {
            if (!node_has_class(node, part))
                return false;
        } else if (kind == '#') {
            const char *id = node_attribute(node, "id");

            if (!id || strcmp(id, part) != 0)
                return false;
        } else {
            if (!node->name || strcasecmp(node->name, part) != 0)
                return false;
        }
    }
    return true;
}

struct collector {
    struct node **out;
    size_t        max;
    size_t        count;
};

/* Prueft einen Selektor mit Nachfahrenbeziehung, also "div p .rot". */
static bool matches_chain(const struct node *node, char parts[][96],
                          int32_t count)
{
    if (count <= 0)
        return false;
    if (!matches_simple(node, parts[count - 1]))
        return false;

    const struct node *current = node->parent;
    int32_t remaining = count - 2;

    while (remaining >= 0) {
        bool found = false;

        while (current) {
            if (matches_simple(current, parts[remaining])) {
                found = true;
                current = current->parent;
                break;
            }
            current = current->parent;
        }
        if (!found)
            return false;
        remaining--;
    }
    return true;
}

static void query_walk(struct node *node, char parts[][96], int32_t count,
                       struct collector *c)
{
    if (c->count >= c->max)
        return;
    if (node->kind == NODE_ELEMENT && matches_chain(node, parts, count))
        c->out[c->count++] = node;
    for (struct node *child = node->first; child; child = child->next)
        query_walk(child, parts, count, c);
}

size_t dom_query(struct node *root, const char *selector,
                 struct node **out, size_t max)
{
    if (!root || !selector || max == 0)
        return 0;

    char parts[8][96];
    int32_t count = 0;
    const char *p = selector;

    while (*p && count < 8) {
        while (*p == ' ' || *p == '\t' || *p == '>' || *p == '\n')
            p++;
        if (!*p)
            break;

        size_t at = 0;

        while (*p && *p != ' ' && *p != '\t' && *p != '>' && *p != '\n' &&
               at + 1 < sizeof(parts[0]))
            parts[count][at++] = *p++;
        parts[count][at] = '\0';
        count++;
    }
    if (count == 0)
        return 0;

    struct collector c = { out, max, 0 };

    query_walk(root, parts, count, &c);
    return c.count;
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static void append_text(const struct node *node, char *out, size_t size,
                        size_t *at, bool raw)
{
    if (node->kind == NODE_TEXT && node->text) {
        size_t length = strlen(node->text);

        for (size_t i = 0; i < length && *at + 1 < size; i++)
            out[(*at)++] = node->text[i];
        out[*at] = '\0';
        return;
    }
    if (node->kind == NODE_COMMENT)
        return;
    if (!raw && node->kind == NODE_ELEMENT && node->name &&
        (strcmp(node->name, "script") == 0 || strcmp(node->name, "style") == 0))
        return;

    for (const struct node *c = node->first; c; c = c->next)
        append_text(c, out, size, at, raw);
}

void dom_text_content(const struct node *node, char *out, size_t size)
{
    size_t at = 0;

    if (size == 0)
        return;
    out[0] = '\0';
    if (node)
        append_text(node, out, size, &at, false);
}

void dom_raw_text(const struct node *node, char *out, size_t size)
{
    size_t at = 0;

    if (size == 0)
        return;
    out[0] = '\0';
    if (node)
        append_text(node, out, size, &at, true);
}

/* ------------------------------------------------------------------ */
/* Dokument                                                            */
/* ------------------------------------------------------------------ */

void document_init(struct document *doc)
{
    memset(doc, 0, sizeof(*doc));
    doc->root = node_create(NODE_DOCUMENT, "#document");
}

void document_free(struct document *doc)
{
    if (!doc)
        return;
    node_free(doc->root);
    memset(doc, 0, sizeof(*doc));
}
