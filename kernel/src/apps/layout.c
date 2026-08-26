/* layout.c - Umbruch nach dem Kastenmodell.
 *
 * Bloecke werden untereinander gesetzt, Inline-Inhalte fliessen in
 * Zeilen. Aufeinandertreffende Aussenabstaende werden zusammengefasst,
 * wie es sich gehoert. Schwebende Kaesten (float) belegen einen Rand,
 * an dem der Text vorbeilaeuft.
 */

#include "layout.h"
#include "kstring.h"
#include "mm.h"
#include "font.h"

#define MAX_FLOATS 16
#define MAX_GROUPS 12

struct float_box {
    int32_t bottom;
    int32_t edge;        /* rechte Kante bei links, linke bei rechts */
    bool    right;
};

/* Der Inhalt eines Kastens, der wie ein Wort in der Zeile steht, ist
 * bereits fertig gesetzt. Wandert der Kasten beim Ausrichten der Zeile,
 * muss sein Inhalt im gleichen Mass mitwandern - nicht neu berechnet
 * werden. Dafuer merken wir uns, welche Stuecke zu welchem Kasten
 * gehoeren. */
struct inline_group {
    size_t owner;
    size_t first, last;
};

struct engine {
    struct layout *out;
    image_lookup   lookup;
    void          *context;

    struct float_box floats[MAX_FLOATS];
    int32_t          float_count;

    /* Zustand der laufenden Zeile */
    bool    in_line;
    size_t  line_first;
    int32_t line_x, line_y;
    int32_t line_left, line_right;
    int32_t line_height;
    int32_t line_ascent;
    bool    need_space;
    enum css_align line_align;

    struct inline_group groups[MAX_GROUPS];
    int32_t             group_count;

    /* Die Ausrichtung gilt fuer die ganze Zeile und kommt vom
     * umschliessenden Block - nicht vom ersten Wort darin. */
    enum css_align block_align;

    int32_t depth;
};

/* Gehoert das Stueck zum Inhalt eines Inline-Kastens? */
static bool inside_group(const struct engine *e, size_t index)
{
    for (int32_t i = 0; i < e->group_count; i++)
        if (index >= e->groups[i].first && index <= e->groups[i].last)
            return true;
    return false;
}

static void move_group(struct engine *e, size_t owner, int32_t dx, int32_t dy)
{
    if (dx == 0 && dy == 0)
        return;
    for (int32_t i = 0; i < e->group_count; i++) {
        if (e->groups[i].owner != owner)
            continue;
        for (size_t k = e->groups[i].first; k <= e->groups[i].last &&
             k < e->out->count; k++) {
            e->out->items[k].rect.x += dx;
            e->out->items[k].rect.y += dy;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Liste der Zeichenstuecke                                            */
/* ------------------------------------------------------------------ */

static struct fragment *emit(struct engine *e)
{
    struct layout *out = e->out;

    if (out->count == out->capacity) {
        size_t want = out->capacity ? out->capacity * 2 : 64;

        if (want > 200000)
            return NULL;

        struct fragment *bigger = krealloc(out->items,
                                           want * sizeof(*bigger));

        if (!bigger)
            return NULL;
        out->items = bigger;
        out->capacity = want;
    }

    struct fragment *f = &out->items[out->count++];

    memset(f, 0, sizeof(*f));
    return f;
}

static char *dup_range(const char *s, size_t length)
{
    char *copy = kmalloc(length + 1);

    if (!copy)
        return NULL;
    memcpy(copy, s, length);
    copy[length] = '\0';
    return copy;
}

/* ------------------------------------------------------------------ */
/* Masse                                                               */
/* ------------------------------------------------------------------ */

static int32_t cell_width(const struct style *st)
{
    return MAX(st->font_size / 2, 1) + st->letter_spacing;
}

static int32_t text_width(const char *s, size_t length, const struct style *st)
{
    return (int32_t)length * cell_width(st);
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* ------------------------------------------------------------------ */
/* Schwebende Kaesten                                                  */
/* ------------------------------------------------------------------ */

static void float_bounds(struct engine *e, int32_t y, int32_t height,
                         int32_t *left, int32_t *right)
{
    for (int32_t i = 0; i < e->float_count; i++) {
        const struct float_box *f = &e->floats[i];

        if (f->bottom <= y)
            continue;
        if (y + height <= 0)
            continue;
        if (f->right)
            *right = MIN(*right, f->edge);
        else
            *left = MAX(*left, f->edge);
    }
}

static int32_t float_clear(struct engine *e, int32_t y, bool left, bool right)
{
    for (int32_t i = 0; i < e->float_count; i++) {
        const struct float_box *f = &e->floats[i];

        if ((f->right && right) || (!f->right && left))
            y = MAX(y, f->bottom);
    }
    return y;
}

/* ------------------------------------------------------------------ */
/* Zeilen                                                              */
/* ------------------------------------------------------------------ */

static void line_begin(struct engine *e, int32_t y, int32_t left,
                       int32_t right, const struct style *st)
{
    e->in_line = true;
    e->line_first = e->out->count;
    e->line_y = y;
    e->line_left = left;
    e->line_right = right;
    e->line_x = left;
    e->line_height = st->line_height;
    e->line_ascent = st->font_size;
    e->line_align = e->block_align;
    e->need_space = false;
    e->group_count = 0;
    UNUSED(st);
}

/* Setzt die Zeile ab: Ausrichtung anwenden und Grundlinie angleichen. */
static int32_t line_end(struct engine *e)
{
    if (!e->in_line)
        return 0;

    e->in_line = false;

    size_t first = e->line_first;
    size_t count = e->out->count;

    if (first >= count)
        return e->line_height;

    int32_t used = e->line_x - e->line_left;
    int32_t space = (e->line_right - e->line_left) - used;
    int32_t shift = 0;

    if (space > 0) {
        if (e->line_align == ALIGN_CENTER)
            shift = space / 2;
        else if (e->line_align == ALIGN_RIGHT)
            shift = space;
    }

    for (size_t i = first; i < count; i++) {
        if (inside_group(e, i))
            continue;           /* wandert mit seinem Kasten */

        struct fragment *f = &e->out->items[i];
        int32_t old_x = f->rect.x, old_y = f->rect.y;

        f->rect.x += shift;

        /* Alles auf eine gemeinsame Grundlinie stellen. */
        if (f->kind == FRAG_TEXT) {
            f->rect.y = e->line_y + e->line_ascent - f->font_size +
                        (e->line_height - e->line_ascent) / 2;
        } else {
            f->rect.y = e->line_y + MAX(e->line_height - f->rect.h, 0);
        }

        move_group(e, i, f->rect.x - old_x, f->rect.y - old_y);
    }
    e->group_count = 0;
    return e->line_height;
}

/* Wechselt in die naechste Zeile und liefert die neue Hoehe. */
static int32_t line_break(struct engine *e, int32_t *y, int32_t left,
                          int32_t right, const struct style *st)
{
    int32_t height = line_end(e);

    *y += height;

    int32_t l = left, r = right;

    float_bounds(e, *y, st->line_height, &l, &r);
    line_begin(e, *y, l, r, st);
    return height;
}

/* ------------------------------------------------------------------ */
/* Inline-Inhalt setzen                                                */
/* ------------------------------------------------------------------ */

static void place_inline_box(struct engine *e, int32_t *y, int32_t left,
                             int32_t right, const struct style *st,
                             int32_t width, int32_t height,
                             struct fragment **slot)
{
    if (!e->in_line)
        line_begin(e, *y, left, right, st);

    if (e->line_x + width > e->line_right && e->line_x > e->line_left)
        line_break(e, y, left, right, st);

    struct fragment *f = emit(e);

    if (!f) {
        *slot = NULL;
        return;
    }
    f->rect = rect_make(e->line_x, e->line_y, width, height);
    e->line_x += width;
    e->line_height = MAX(e->line_height, height);
    e->line_ascent = MAX(e->line_ascent, height);
    e->need_space = false;
    *slot = f;
}

static void fill_text_style(struct fragment *f, const struct style *st)
{
    f->color = st->color;
    f->font_size = st->font_size;
    f->bold = st->bold;
    f->italic = st->italic;
    f->underline = st->underline;
    f->strike = st->strike;
    f->tracking = st->letter_spacing;
}

/* Wandelt Gross- und Kleinschreibung nach Vorgabe. */
static void transform_case(char *s, const struct style *st)
{
    if (!st->uppercase && !st->lowercase)
        return;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if (st->uppercase && c >= 'a' && c <= 'z')
            *s = (char)(c - 32);
        else if (st->lowercase && c >= 'A' && c <= 'Z')
            *s = (char)(c + 32);
    }
}

static void add_text_run(struct engine *e, struct node *node, const char *start,
                         size_t length, const struct style *st, int32_t *y,
                         int32_t left, int32_t right)
{
    if (length == 0)
        return;

    struct fragment *f;

    place_inline_box(e, y, left, right, st,
                     text_width(start, length, st), st->font_size, &f);
    if (!f)
        return;
    f->kind = FRAG_TEXT;
    f->node = node;
    f->text = dup_range(start, length);
    if (f->text)
        transform_case(f->text, st);
    fill_text_style(f, st);
}

/* Setzt Text mit Zusammenfassen von Leerraum und Umbruch an Wortgrenzen. */
static void flow_text(struct engine *e, struct node *node, const char *text,
                      const struct style *st, int32_t *y, int32_t left,
                      int32_t right)
{
    size_t pos = 0;
    size_t length = strlen(text);

    if (st->preformatted) {
        /* Zeilenwechsel bleiben erhalten, Leerzeichen ebenso. */
        while (pos <= length) {
            size_t start = pos;

            while (pos < length && text[pos] != '\n')
                pos++;
            if (pos > start)
                add_text_run(e, node, text + start, pos - start, st, y,
                             left, right);
            if (pos >= length)
                break;
            pos++;
            line_break(e, y, left, right, st);
            e->need_space = false;
        }
        return;
    }

    while (pos < length) {
        if (is_space(text[pos])) {
            while (pos < length && is_space(text[pos]))
                pos++;
            /* Am Zeilenanfang wird der Abstand verschluckt. */
            if (e->in_line && e->line_x > e->line_left)
                e->need_space = true;
            continue;
        }

        size_t start = pos;

        while (pos < length && !is_space(text[pos]))
            pos++;

        size_t word_length = pos - start;
        int32_t advance = text_width(text + start, word_length, st);
        int32_t gap = e->need_space ? cell_width(st) : 0;

        if (!e->in_line)
            line_begin(e, *y, left, right, st);

        if (!st->nowrap && e->line_x + gap + advance > e->line_right &&
            e->line_x > e->line_left) {
            line_break(e, y, left, right, st);
            gap = 0;
            e->need_space = false;
        }

        if (gap > 0) {
            e->line_x += gap;
            e->need_space = false;
        }

        /* Ein einzelnes Wort, das nicht passt, wird hart getrennt. */
        int32_t room = e->line_right - e->line_x;

        if (advance > room && !st->nowrap && room > cell_width(st) * 2) {
            size_t fits = (size_t)(room / cell_width(st));

            if (fits > 0 && fits < word_length) {
                add_text_run(e, node, text + start, fits, st, y, left, right);
                pos = start + fits;
                line_break(e, y, left, right, st);
                e->need_space = false;
                continue;
            }
        }

        add_text_run(e, node, text + start, word_length, st, y, left, right);
        e->line_height = MAX(e->line_height, st->line_height);
        e->line_ascent = MAX(e->line_ascent, st->font_size);
    }
}

/* ------------------------------------------------------------------ */
/* Kasteninhalt                                                        */
/* ------------------------------------------------------------------ */

/* Setzt die Kinder eines Knotens. Bei inline_mode bleibt die laufende
 * Zeile am Ende offen, damit ein Inline-Element den Fluss nicht abreisst. */
static int32_t layout_content(struct engine *e, struct node *node, int32_t x,
                              int32_t width, int32_t y, bool inline_mode);

static int32_t layout_children(struct engine *e, struct node *node,
                               int32_t x, int32_t width, int32_t y)
{
    return layout_content(e, node, x, width, y, false);
}

static bool inline_level(const struct node *node)
{
    if (node->kind == NODE_TEXT)
        return true;
    if (node->kind != NODE_ELEMENT)
        return false;
    return node->style.display == DISPLAY_INLINE ||
           node->style.display == DISPLAY_INLINE_BLOCK;
}

/* Ermittelt die gewuenschte Breite eines Bildes. */
static void image_size(struct node *node, const struct image *img,
                       int32_t available, int32_t *w, int32_t *h)
{
    int32_t natural_w = img ? img->w : 0;
    int32_t natural_h = img ? img->h : 0;
    const struct style *st = &node->style;

    if (natural_w <= 0 || natural_h <= 0) {
        natural_w = 120;
        natural_h = 60;
    }

    int32_t want_w = -1, want_h = -1;

    if (st->width.unit == LEN_PX)
        want_w = st->width.value;
    else if (st->width.unit == LEN_PERCENT)
        want_w = available * st->width.value / 100;
    if (st->height.unit == LEN_PX)
        want_h = st->height.value;

    if (want_w < 0 && want_h < 0) {
        const char *attr = node_attribute(node, "width");

        if (attr) {
            int32_t value = 0;

            for (const char *p = attr; *p >= '0' && *p <= '9'; p++)
                value = value * 10 + (*p - '0');
            if (value > 0)
                want_w = value;
        }
        attr = node_attribute(node, "height");
        if (attr) {
            int32_t value = 0;

            for (const char *p = attr; *p >= '0' && *p <= '9'; p++)
                value = value * 10 + (*p - '0');
            if (value > 0)
                want_h = value;
        }
    }

    if (want_w < 0 && want_h < 0) {
        want_w = natural_w;
        want_h = natural_h;
    } else if (want_w < 0) {
        want_w = natural_w * want_h / MAX(natural_h, 1);
    } else if (want_h < 0) {
        want_h = natural_h * want_w / MAX(natural_w, 1);
    }

    /* Nichts darf breiter werden als der Platz. */
    if (available > 0 && want_w > available) {
        want_h = want_h * available / MAX(want_w, 1);
        want_w = available;
    }

    *w = MAX(want_w, 1);
    *h = MAX(want_h, 1);
}

/* Formularelemente bekommen eine feste Groesse. */
static void widget_size(struct node *node, const struct style *st,
                        int32_t *w, int32_t *h)
{
    const char *type = node_attribute(node, "type");
    const char *value = node->value ? node->value
                                    : node_attribute(node, "value");
    int32_t cw = MAX(st->font_size / 2, 1);

    *h = st->font_size + 10;

    if (node->name && strcmp(node->name, "input") == 0 && type &&
        (strcasecmp(type, "checkbox") == 0 || strcasecmp(type, "radio") == 0)) {
        *w = st->font_size;
        *h = st->font_size;
        return;
    }

    if (node->name && (strcmp(node->name, "button") == 0 ||
                       (type && (strcasecmp(type, "submit") == 0 ||
                                 strcasecmp(type, "button") == 0 ||
                                 strcasecmp(type, "reset") == 0)))) {
        char label[128];

        if (node->name && strcmp(node->name, "button") == 0)
            dom_text_content(node, label, sizeof(label));
        else
            strlcpy(label, value ? value : "Abschicken", sizeof(label));
        *w = MAX((int32_t)strlen(label) * cw + 20, 40);
        return;
    }

    if (node->name && strcmp(node->name, "textarea") == 0) {
        *w = 40 * cw + 12;
        *h = st->font_size * 5;
        return;
    }

    int32_t columns = 22;
    const char *size_attr = node_attribute(node, "size");

    if (size_attr) {
        int32_t value_number = 0;

        for (const char *p = size_attr; *p >= '0' && *p <= '9'; p++)
            value_number = value_number * 10 + (*p - '0');
        if (value_number > 0)
            columns = MIN(value_number, 60);
    }
    *w = columns * cw + 12;
}

static bool is_widget(const struct node *node)
{
    if (node->kind != NODE_ELEMENT || !node->name)
        return false;
    return strcmp(node->name, "input") == 0 ||
           strcmp(node->name, "button") == 0 ||
           strcmp(node->name, "select") == 0 ||
           strcmp(node->name, "textarea") == 0;
}

static void layout_widget(struct engine *e, struct node *node, int32_t *y,
                          int32_t left, int32_t right)
{
    const struct style *st = &node->style;
    const char *type = node_attribute(node, "type");
    int32_t w, h;

    if (type && strcasecmp(type, "hidden") == 0)
        return;

    widget_size(node, st, &w, &h);

    struct fragment *f;

    place_inline_box(e, y, left, right, st, w + 4, h, &f);
    if (!f)
        return;

    f->node = node;
    f->rect.w = w;
    fill_text_style(f, st);

    if (type && (strcasecmp(type, "checkbox") == 0 ||
                 strcasecmp(type, "radio") == 0)) {
        f->kind = FRAG_CHECKBOX;
        f->checked = node->checked;
        return;
    }
    if ((node->name && strcmp(node->name, "button") == 0) ||
        (type && (strcasecmp(type, "submit") == 0 ||
                  strcasecmp(type, "button") == 0 ||
                  strcasecmp(type, "reset") == 0))) {
        f->kind = FRAG_BUTTON;

        char label[128];

        if (strcmp(node->name, "button") == 0)
            dom_text_content(node, label, sizeof(label));
        else {
            const char *value = node->value ? node->value
                                            : node_attribute(node, "value");

            strlcpy(label, value ? value : "Abschicken", sizeof(label));
        }
        f->text = dup_range(label, strlen(label));
        return;
    }

    f->kind = FRAG_FIELD;

    const char *value = node->value ? node->value
                                    : node_attribute(node, "value");

    if (!value || !*value)
        value = node_attribute(node, "placeholder");
    if (value)
        f->text = dup_range(value, strlen(value));
    return;
}

static void layout_image(struct engine *e, struct node *node, int32_t *y,
                         int32_t left, int32_t right)
{
    const char *src = node_attribute(node, "src");
    struct image *img = NULL;

    if (src && e->lookup)
        img = e->lookup(e->context, src);

    int32_t w, h;

    image_size(node, img, right - left, &w, &h);

    struct fragment *f;

    place_inline_box(e, y, left, right, &node->style, w, h, &f);
    if (!f)
        return;
    f->kind = FRAG_IMAGE;
    f->node = node;
    f->image = img;
    f->color = node->style.color;
    f->font_size = node->style.font_size;

    if (!img) {
        /* Ohne Bild wird der Ersatztext gezeigt. */
        const char *alt = node_attribute(node, "alt");

        if (alt && *alt)
            f->text = dup_range(alt, strlen(alt));
    }
}

/* ------------------------------------------------------------------ */
/* Bloecke                                                             */
/* ------------------------------------------------------------------ */

struct block_result {
    int32_t y;              /* Unterkante einschliesslich Aussenabstand */
    int32_t margin_bottom;  /* noch offener Abstand nach unten */
};

static int32_t resolve_width(const struct style *st, int32_t available)
{
    if (st->width.unit == LEN_PX)
        return MIN(st->width.value, available);
    if (st->width.unit == LEN_PERCENT)
        return available * st->width.value / 100;
    return available;
}

/* Setzt einen Blockkasten und gibt die Unterkante zurueck. */
static int32_t layout_block(struct engine *e, struct node *node, int32_t x,
                            int32_t available, int32_t y)
{
    const struct style *st = &node->style;

    if (st->display == DISPLAY_NONE)
        return y;

    if (st->clear_left || st->clear_right)
        y = float_clear(e, y, st->clear_left, st->clear_right);

    int32_t margin_left = st->margin[3];
    int32_t margin_right = st->margin[1];
    int32_t border_x = st->border[3] + st->border[1];
    int32_t padding_x = st->padding[3] + st->padding[1];

    int32_t outer_available = available - margin_left - margin_right;
    int32_t box_width = resolve_width(st, outer_available);

    if (st->width.unit != LEN_AUTO)
        box_width += border_x + padding_x;
    box_width = MAX(box_width, border_x + padding_x);

    /* Ein Block mit fester Breite und "auto" an beiden Seiten steht in
     * der Mitte - so bekommen unzaehlige Seiten ihren Textblock
     * mittig auf die Flaeche. */
    int32_t box_x = x + margin_left;

    if (st->width.unit != LEN_AUTO && box_width < available) {
        if (st->margin_auto_left && st->margin_auto_right)
            box_x = x + (available - box_width) / 2;
        else if (st->margin_auto_left)
            box_x = x + available - box_width;
    }

    int32_t box_y = y + st->margin[0];

    /* Ein schwebender Kasten wird an den Rand geschoben. */
    bool floating = st->float_left || st->float_right;

    if (floating) {
        int32_t l = x, r = x + available;

        float_bounds(e, box_y, 1, &l, &r);
        if (st->float_right)
            box_x = r - box_width;
        else
            box_x = l;
    }

    size_t box_index = e->out->count;
    struct fragment *box = emit(e);

    if (!box)
        return y;
    box->kind = FRAG_BOX;
    box->node = node;
    box->background = st->background;
    box->has_background = st->has_background;
    for (int i = 0; i < 4; i++) {
        box->border[i] = st->border[i];
        box->border_color[i] = st->border_color[i];
    }
    box->color = st->color;
    box->font_size = st->font_size;

    int32_t content_x = box_x + st->border[3] + st->padding[3];
    int32_t content_width = box_width - border_x - padding_x;
    int32_t content_y = box_y + st->border[0] + st->padding[0];

    if (node->name && strcmp(node->name, "hr") == 0) {
        struct fragment *rule = emit(e);

        if (rule) {
            rule->kind = FRAG_RULE;
            rule->node = node;
            rule->rect = rect_make(content_x, content_y,
                                   MAX(content_width, 1), 1);
            rule->color = st->border_color[0] ? st->border_color[0] : 0xBBBBBB;
        }
        content_y += 1;
    } else {
        content_y = layout_children(e, node, content_x, MAX(content_width, 1),
                                    content_y);
    }

    int32_t content_height = content_y - (box_y + st->border[0] +
                                          st->padding[0]);

    if (st->height.unit == LEN_PX)
        content_height = MAX(content_height, st->height.value);

    int32_t box_height = content_height + st->border[0] + st->border[2] +
                         st->padding[0] + st->padding[2];

    e->out->items[box_index].rect = rect_make(box_x, box_y, box_width,
                                              MAX(box_height, 0));

    if (floating) {
        if (e->float_count < MAX_FLOATS) {
            struct float_box *f = &e->floats[e->float_count++];

            f->bottom = box_y + box_height + st->margin[2];
            f->right = st->float_right;
            f->edge = st->float_right ? box_x - st->margin[3]
                                      : box_x + box_width + margin_right;
        }
        return y;   /* schwebende Kaesten schieben den Fluss nicht weiter */
    }

    return box_y + box_height + st->margin[2];
}

/* Setzt einen Kasten, der wie ein Wort in der Zeile steht. */
static void layout_inline_block(struct engine *e, struct node *node,
                                int32_t *y, int32_t left, int32_t right)
{
    const struct style *st = &node->style;
    int32_t available = right - left;
    int32_t box_width = resolve_width(st, available);
    bool auto_width = st->width.unit == LEN_AUTO;

    /* Erst versuchsweise umbrechen, um die Hoehe zu erfahren. */
    struct engine probe = *e;
    struct layout scratch;

    memset(&scratch, 0, sizeof(scratch));
    probe.out = &scratch;
    probe.in_line = false;
    probe.float_count = 0;
    probe.group_count = 0;

    int32_t probe_y = layout_children(&probe, node, 0,
                                      MAX(auto_width ? available : box_width, 1),
                                      0);
    int32_t widest = 0;

    for (size_t i = 0; i < scratch.count; i++) {
        struct fragment *f = &scratch.items[i];

        if (f->kind != FRAG_BOX)
            widest = MAX(widest, f->rect.x + f->rect.w);
    }
    layout_free(&scratch);

    if (auto_width)
        box_width = MIN(MAX(widest, 1), available);

    int32_t border_x = st->border[3] + st->border[1];
    int32_t padding_x = st->padding[3] + st->padding[1];
    int32_t border_y = st->border[0] + st->border[2];
    int32_t padding_y = st->padding[0] + st->padding[2];
    int32_t outer_w = box_width + border_x + padding_x;
    int32_t outer_h = probe_y + border_y + padding_y;

    if (st->height.unit == LEN_PX)
        outer_h = st->height.value + border_y + padding_y;

    struct fragment *placeholder;

    place_inline_box(e, y, left, right, st, outer_w, MAX(outer_h, 1),
                     &placeholder);
    if (!placeholder)
        return;

    size_t index = (size_t)(placeholder - e->out->items);

    e->out->items[index].kind = FRAG_BOX;
    e->out->items[index].node = node;
    e->out->items[index].background = st->background;
    e->out->items[index].has_background = st->has_background;
    for (int i = 0; i < 4; i++) {
        e->out->items[index].border[i] = st->border[i];
        e->out->items[index].border_color[i] = st->border_color[i];
    }

    struct rect area = e->out->items[index].rect;
    bool saved_line = e->in_line;
    size_t saved_first = e->line_first;
    int32_t saved_x = e->line_x, saved_y = e->line_y;
    int32_t saved_left = e->line_left, saved_right = e->line_right;
    int32_t saved_height = e->line_height, saved_ascent = e->line_ascent;
    bool saved_space = e->need_space;
    enum css_align saved_align = e->line_align;
    struct inline_group saved_groups[MAX_GROUPS];
    int32_t saved_group_count = e->group_count;

    memcpy(saved_groups, e->groups, sizeof(saved_groups));

    size_t content_first = e->out->count;

    e->in_line = false;
    e->group_count = 0;
    layout_children(e, node, area.x + st->border[3] + st->padding[3],
                    MAX(box_width, 1),
                    area.y + st->border[0] + st->padding[0]);
    line_end(e);

    memcpy(e->groups, saved_groups, sizeof(saved_groups));
    e->group_count = saved_group_count;

    /* Der Inhalt haengt jetzt am Kasten. */
    if (e->out->count > content_first && e->group_count < MAX_GROUPS) {
        struct inline_group *g = &e->groups[e->group_count++];

        g->owner = index;
        g->first = content_first;
        g->last = e->out->count - 1;
    }

    e->in_line = saved_line;
    e->line_first = saved_first;
    e->line_x = saved_x;
    e->line_y = saved_y;
    e->line_left = saved_left;
    e->line_right = saved_right;
    e->line_height = saved_height;
    e->line_ascent = saved_ascent;
    e->need_space = saved_space;
    e->line_align = saved_align;
}

/* ------------------------------------------------------------------ */
/* Tabellen                                                            */
/* ------------------------------------------------------------------ */

static bool is_table_row(const struct node *node)
{
    return node->kind == NODE_ELEMENT &&
           node->style.display == DISPLAY_TABLE_ROW;
}

static void collect_rows(struct node *node, struct node **rows, size_t max,
                         size_t *count)
{
    for (struct node *c = node->first; c; c = c->next) {
        if (c->kind != NODE_ELEMENT)
            continue;
        if (is_table_row(c)) {
            if (*count < max)
                rows[(*count)++] = c;
        } else if (c->style.display == DISPLAY_BLOCK ||
                   c->style.display == DISPLAY_TABLE) {
            collect_rows(c, rows, max, count);
        }
    }
}

static int32_t layout_table(struct engine *e, struct node *node, int32_t x,
                            int32_t available, int32_t y)
{
    struct node *rows[128];
    size_t row_count = 0;

    collect_rows(node, rows, ARRAY_LEN(rows), &row_count);
    if (row_count == 0)
        return layout_block(e, node, x, available, y);

    /* Spaltenzahl bestimmen. */
    size_t columns = 0;

    for (size_t r = 0; r < row_count; r++) {
        size_t n = 0;

        for (struct node *c = rows[r]->first; c; c = c->next)
            if (c->kind == NODE_ELEMENT &&
                c->style.display == DISPLAY_TABLE_CELL)
                n++;
        columns = MAX(columns, n);
    }
    if (columns == 0)
        return layout_block(e, node, x, available, y);

    const struct style *st = &node->style;
    int32_t table_x = x + st->margin[3];
    int32_t table_y = y + st->margin[0];
    int32_t table_width = resolve_width(st, available - st->margin[3] -
                                            st->margin[1]);
    int32_t column_width = MAX(table_width / (int32_t)columns, 24);

    size_t table_index = e->out->count;
    struct fragment *frame = emit(e);

    if (!frame)
        return y;
    frame->kind = FRAG_BOX;
    frame->node = node;
    frame->background = st->background;
    frame->has_background = st->has_background;

    int32_t row_y = table_y;

    for (size_t r = 0; r < row_count; r++) {
        struct node *row = rows[r];
        size_t row_index = e->out->count;
        struct fragment *row_frame = emit(e);

        if (!row_frame)
            break;
        row_frame->kind = FRAG_BOX;
        row_frame->node = row;
        row_frame->background = row->style.background;
        row_frame->has_background = row->style.has_background;

        int32_t cell_x = table_x;
        int32_t tallest = 0;
        size_t cell_indices[64];
        size_t cell_count = 0;

        for (struct node *cell = row->first; cell; cell = cell->next) {
            if (cell->kind != NODE_ELEMENT ||
                cell->style.display != DISPLAY_TABLE_CELL)
                continue;

            const struct style *cs = &cell->style;
            int32_t span = 1;
            const char *span_attr = node_attribute(cell, "colspan");

            if (span_attr) {
                int32_t value = 0;

                for (const char *p = span_attr; *p >= '0' && *p <= '9'; p++)
                    value = value * 10 + (*p - '0');
                span = CLAMP(value, 1, (int32_t)columns);
            }

            int32_t width = column_width * span;
            size_t index = e->out->count;
            struct fragment *box = emit(e);

            if (!box)
                break;
            box->kind = FRAG_BOX;
            box->node = cell;
            box->background = cs->background;
            box->has_background = cs->has_background;
            for (int i = 0; i < 4; i++) {
                box->border[i] = cs->border[i];
                box->border_color[i] = cs->border_color[i];
            }

            int32_t inner_x = cell_x + cs->border[3] + cs->padding[3];
            int32_t inner_w = width - cs->border[3] - cs->border[1] -
                              cs->padding[3] - cs->padding[1];
            int32_t inner_y = row_y + cs->border[0] + cs->padding[0];
            int32_t end = layout_children(e, cell, inner_x, MAX(inner_w, 8),
                                          inner_y);
            int32_t height = end - inner_y + cs->border[0] + cs->border[2] +
                             cs->padding[0] + cs->padding[2];

            e->out->items[index].rect = rect_make(cell_x, row_y, width,
                                                  MAX(height, 1));
            if (cell_count < ARRAY_LEN(cell_indices))
                cell_indices[cell_count++] = index;
            tallest = MAX(tallest, height);
            cell_x += width;
        }

        /* Alle Zellen der Zeile auf die gleiche Hoehe bringen. */
        for (size_t i = 0; i < cell_count; i++)
            e->out->items[cell_indices[i]].rect.h = tallest;

        e->out->items[row_index].rect = rect_make(table_x, row_y,
                                                  cell_x - table_x,
                                                  MAX(tallest, 1));
        row_y += MAX(tallest, 1);
    }

    e->out->items[table_index].rect = rect_make(table_x, table_y,
                                                table_width,
                                                row_y - table_y);
    return row_y + st->margin[2];
}

/* ------------------------------------------------------------------ */
/* Kinder eines Kastens                                                */
/* ------------------------------------------------------------------ */

static void list_bullet(struct engine *e, struct node *node, int32_t x,
                        int32_t y)
{
    struct fragment *f = emit(e);

    if (!f)
        return;
    f->kind = FRAG_BULLET;
    f->node = node;
    f->color = node->style.color;
    f->font_size = node->style.font_size;

    int32_t size = MAX(node->style.font_size / 4, 3);

    f->rect = rect_make(x - size * 3, y + node->style.font_size / 2 - size / 2,
                        size, size);
}

static int32_t layout_content(struct engine *e, struct node *node, int32_t x,
                              int32_t width, int32_t y, bool inline_mode)
{
    if (e->depth > 48)
        return y;
    e->depth++;

    enum css_align saved_align = e->block_align;

    e->block_align = node->style.align;

    int32_t left = x, right = x + width;
    int32_t previous_margin = 0;
    bool saw_block = false;

    for (struct node *child = node->first; child; child = child->next) {
        if (child->kind == NODE_COMMENT)
            continue;
        if (child->kind == NODE_ELEMENT &&
            child->style.display == DISPLAY_NONE)
            continue;

        if (child->kind == NODE_TEXT) {
            if (child->text && *child->text)
                flow_text(e, child, child->text, &node->style, &y, left, right);
            continue;
        }
        if (child->kind != NODE_ELEMENT)
            continue;

        const struct style *cs = &child->style;

        if (child->name && strcmp(child->name, "br") == 0) {
            if (!e->in_line)
                line_begin(e, y, left, right, cs);
            line_break(e, &y, left, right, cs);
            e->need_space = false;
            continue;
        }
        if (child->name && strcmp(child->name, "img") == 0) {
            layout_image(e, child, &y, left, right);
            continue;
        }
        if (is_widget(child)) {
            layout_widget(e, child, &y, left, right);
            continue;
        }

        if (inline_level(child)) {
            if (cs->display == DISPLAY_INLINE_BLOCK) {
                layout_inline_block(e, child, &y, left, right);
            } else {
                /* Ein durchsichtiger Kasten: Hintergrund und Rahmen
                 * malen wir nur, wenn sie gesetzt sind. */
                size_t before = e->out->count;
                bool decorated = cs->has_background || cs->border[0] ||
                                 cs->border[1] || cs->border[2] || cs->border[3];
                struct fragment *box = NULL;

                if (decorated) {
                    box = emit(e);
                    if (box) {
                        box->kind = FRAG_BOX;
                        box->node = child;
                        box->background = cs->background;
                        box->has_background = cs->has_background;
                        for (int i = 0; i < 4; i++) {
                            box->border[i] = cs->border[i];
                            box->border_color[i] = cs->border_color[i];
                        }
                    }
                }

                int32_t start_x = e->in_line ? e->line_x : left;
                int32_t start_y = e->in_line ? e->line_y : y;

                y = layout_content(e, child, x, width, y, true);

                if (decorated && box) {
                    int32_t end_x = e->in_line ? e->line_x : left;
                    int32_t bottom = (e->in_line ? e->line_y : y) +
                                     (e->in_line ? e->line_height
                                                 : cs->line_height);

                    e->out->items[before].rect =
                        rect_make(start_x, start_y,
                                  MAX(end_x - start_x, 1),
                                  MAX(bottom - start_y, cs->font_size));
                }
                /* Ein Inline-Kasten kann eine Marke fuer Verweise sein. */
                for (size_t i = before; i < e->out->count; i++)
                    if (!e->out->items[i].node)
                        e->out->items[i].node = child;
            }
            continue;
        }

        /* Ab hier: Blockelement. Die laufende Zeile wird abgesetzt. */
        if (e->in_line) {
            y += line_end(e);
            e->need_space = false;
        }

        int32_t collapse = MAX(previous_margin, cs->margin[0]);

        if (saw_block)
            y = y - previous_margin + collapse;

        if (cs->display == DISPLAY_TABLE) {
            y = layout_table(e, child, x, width, y);
            previous_margin = cs->margin[2];
            saw_block = true;
            continue;
        }

        if (cs->display == DISPLAY_LIST_ITEM) {
            int32_t before_y = y + cs->margin[0] + cs->border[0] +
                               cs->padding[0];

            list_bullet(e, child, x + cs->margin[3] + cs->padding[3],
                        before_y);
        }

        y = layout_block(e, child, x, width, y);
        previous_margin = cs->margin[2];
        saw_block = true;
    }

    if (e->in_line && !inline_mode)
        y += line_end(e);

    e->block_align = saved_align;
    e->depth--;
    return y;
}

/* ------------------------------------------------------------------ */
/* Oeffentliche Schnittstelle                                          */
/* ------------------------------------------------------------------ */

void layout_run(struct layout *out, struct node *body, int32_t width,
                image_lookup lookup, void *context)
{
    layout_free(out);
    memset(out, 0, sizeof(*out));
    out->width = width;

    if (!body || width <= 0)
        return;

    struct engine e;

    memset(&e, 0, sizeof(e));
    e.out = out;
    e.lookup = lookup;
    e.context = context;
    e.block_align = body->style.align;

    int32_t bottom = layout_block(&e, body, 0, width, 0);

    for (int32_t i = 0; i < e.float_count; i++)
        bottom = MAX(bottom, e.floats[i].bottom);

    /* Manche Kaesten ragen weiter, etwa durch feste Hoehen. */
    for (size_t i = 0; i < out->count; i++)
        bottom = MAX(bottom, out->items[i].rect.y + out->items[i].rect.h);

    out->height = bottom;

    /* Die Ergebnislagen im Baum ablegen, damit Skripte sie finden. */
    for (size_t i = 0; i < out->count; i++) {
        struct fragment *f = &out->items[i];

        if (!f->node)
            continue;
        if (f->kind == FRAG_BOX || !f->node->has_box) {
            f->node->box = f->rect;
            f->node->has_box = true;
        }
    }
}

void layout_free(struct layout *out)
{
    if (!out || !out->items)
        return;
    for (size_t i = 0; i < out->count; i++)
        kfree(out->items[i].text);
    kfree(out->items);
    out->items = NULL;
    out->count = 0;
    out->capacity = 0;
    out->height = 0;
}

struct node *layout_hit(const struct layout *out, int32_t x, int32_t y)
{
    struct node *found = NULL;

    for (size_t i = 0; i < out->count; i++) {
        const struct fragment *f = &out->items[i];

        if (!f->node)
            continue;
        if (!rect_contains(f->rect, x, y))
            continue;
        /* Spaeter gezeichnete Stuecke liegen oben. */
        found = f->node;
    }
    return found;
}
