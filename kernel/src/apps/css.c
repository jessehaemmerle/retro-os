/* css.c - Formatvorlagen: lesen, gewichten, anwenden.
 *
 * Eine Regel besteht aus einem Selektor und einer Liste von
 * Eigenschaften. Beim Anwenden wird fuer jeden Knoten der Baum von oben
 * nach unten durchlaufen; geerbte Eigenschaften kommen vom Elternteil,
 * die uebrigen aus den passenden Regeln, sortiert nach Gewicht. Zuletzt
 * gilt das style-Attribut.
 */

#include "css.h"
#include "kstring.h"
#include "mm.h"

#define MAX_PARTS 6

struct declaration {
    char *name;
    char *value;
    bool  important;
    struct declaration *next;
};

/* Ein Selektor als Kette von Bestandteilen, letzter Teil zuerst geprueft. */
struct selector {
    char    parts[MAX_PARTS][80];
    int32_t count;
    int32_t weight;             /* Kennungen, Klassen und Namen gewichtet */
    struct selector *next;
};

struct rule {
    struct selector    *selectors;
    struct declaration *declarations;
    struct rule        *next;
};

struct stylesheet {
    struct rule *rules;
    struct rule *last;
    uint32_t     order;
};

static bool space_char(char c);
static bool name_has(const char *haystack, const char *needle);

/* Groesse des Sichtfelds fuer Angaben in vw und vh. Sie gilt fuer den
 * laufenden Durchgang und wird von css_apply gesetzt. */
static int32_t viewport_width = 800;
static int32_t viewport_height = 600;

/* Eigene Eigenschaften wie --farbe werden vererbt. Beim Abstieg in den
 * Baum wachsen sie auf einem Stapel; beim Aufstieg fallen sie wieder ab,
 * damit Geschwister nichts voneinander sehen. */
#define MAX_VARS 128

struct css_var {
    char name[64];
    char value[192];
};

struct cascade {
    struct stylesheet *sheet;
    struct css_var     vars[MAX_VARS];
    int32_t            var_count;
};

static const char *var_lookup(const struct cascade *c, const char *name,
                              size_t length)
{
    /* Von hinten suchen: die naeher stehende Festlegung gewinnt. */
    for (int32_t i = c->var_count - 1; i >= 0; i--)
        if (strlen(c->vars[i].name) == length &&
            strncmp(c->vars[i].name, name, length) == 0)
            return c->vars[i].value;
    return NULL;
}

static void var_define(struct cascade *c, const char *name, const char *value)
{
    for (int32_t i = c->var_count - 1; i >= 0; i--) {
        if (strcmp(c->vars[i].name, name) != 0)
            continue;
        strlcpy(c->vars[i].value, value, sizeof(c->vars[i].value));
        return;
    }
    if (c->var_count >= MAX_VARS)
        return;

    struct css_var *v = &c->vars[c->var_count++];

    strlcpy(v->name, name, sizeof(v->name));
    strlcpy(v->value, value, sizeof(v->value));
}

/* Setzt var(--name, Ersatzwert) ein. */
static void var_expand(const struct cascade *c, const char *value, char *out,
                       size_t size, int32_t depth)
{
    size_t at = 0;

    out[0] = '\0';
    if (depth > 6) {
        strlcpy(out, value, size);
        return;
    }

    const char *p = value;

    while (*p && at + 1 < size) {
        if (strncasecmp(p, "var(", 4) != 0) {
            out[at++] = *p++;
            out[at] = '\0';
            continue;
        }

        p += 4;

        /* Bis zur passenden schliessenden Klammer lesen. */
        const char *start = p;
        int32_t nesting = 1;

        while (*p && nesting > 0) {
            if (*p == '(')
                nesting++;
            else if (*p == ')')
                nesting--;
            if (nesting > 0)
                p++;
        }

        size_t inner_length = (size_t)(p - start);

        if (*p == ')')
            p++;

        /* Der Name geht bis zum ersten Komma, danach der Ersatzwert. */
        const char *comma = NULL;
        int32_t level = 0;

        for (size_t i = 0; i < inner_length; i++) {
            if (start[i] == '(')
                level++;
            else if (start[i] == ')')
                level--;
            else if (start[i] == ',' && level == 0) {
                comma = start + i;
                break;
            }
        }

        size_t name_length = comma ? (size_t)(comma - start) : inner_length;

        while (name_length > 0 && space_char(start[name_length - 1]))
            name_length--;

        const char *name = start;

        while (name_length > 0 && space_char(*name)) {
            name++;
            name_length--;
        }

        const char *found = var_lookup(c, name, name_length);
        char ersatz[192];

        if (!found && comma) {
            size_t fallback_length = inner_length -
                                     (size_t)(comma + 1 - start);

            strlcpy(ersatz, "", sizeof(ersatz));
            if (fallback_length + 1 < sizeof(ersatz)) {
                memcpy(ersatz, comma + 1, fallback_length);
                ersatz[fallback_length] = '\0';
            }
            found = ersatz;
        }
        if (!found)
            continue;

        char aufgeloest[192];

        var_expand(c, found, aufgeloest, sizeof(aufgeloest), depth + 1);

        for (const char *q = aufgeloest; *q && at + 1 < size; q++)
            out[at++] = *q;
        out[at] = '\0';
    }
}

/* ------------------------------------------------------------------ */
/* Kleinkram                                                           */
/* ------------------------------------------------------------------ */

static char *dup_range(const char *s, size_t length)
{
    char *copy = kmalloc(length + 1);

    if (!copy)
        return NULL;
    memcpy(copy, s, length);
    copy[length] = '\0';
    return copy;
}

static bool space_char(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

/* Teilzeichenkette suchen - fuer Eigenschaftsnamen wie border-top-width. */
static bool name_has(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);

    for (const char *p = haystack; *p; p++)
        if (strncmp(p, needle, n) == 0)
            return true;
    return false;
}

static void lower_in_place(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s + 32);
}

static int32_t parse_int(const char **p)
{
    bool negative = false;
    int32_t value = 0;

    if (**p == '-') {
        negative = true;
        (*p)++;
    } else if (**p == '+') {
        (*p)++;
    }
    while (**p >= '0' && **p <= '9')
        value = value * 10 + (*(*p)++ - '0');

    /* Nachkommastellen werden gelesen, aber gerundet verworfen. */
    if (**p == '.') {
        (*p)++;
        int32_t first = -1;

        while (**p >= '0' && **p <= '9') {
            if (first < 0)
                first = **p - '0';
            (*p)++;
        }
        if (first >= 5)
            value++;
    }
    return negative ? -value : value;
}

/* ------------------------------------------------------------------ */
/* Farben                                                              */
/* ------------------------------------------------------------------ */

struct color_name {
    const char *name;
    uint32_t    value;
};

static const struct color_name color_names[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 },
    { "green", 0x008000 }, { "lime", 0x00FF00 }, { "blue", 0x0000FF },
    { "yellow", 0xFFFF00 }, { "cyan", 0x00FFFF }, { "aqua", 0x00FFFF },
    { "magenta", 0xFF00FF }, { "fuchsia", 0xFF00FF }, { "gray", 0x808080 },
    { "grey", 0x808080 }, { "silver", 0xC0C0C0 }, { "maroon", 0x800000 },
    { "olive", 0x808000 }, { "navy", 0x000080 }, { "purple", 0x800080 },
    { "teal", 0x008080 }, { "orange", 0xFFA500 }, { "pink", 0xFFC0CB },
    { "brown", 0xA52A2A }, { "gold", 0xFFD700 }, { "beige", 0xF5F5DC },
    { "indigo", 0x4B0082 }, { "violet", 0xEE82EE }, { "tan", 0xD2B48C },
    { "salmon", 0xFA8072 }, { "khaki", 0xF0E68C }, { "coral", 0xFF7F50 },
    { "crimson", 0xDC143C }, { "orchid", 0xDA70D6 }, { "plum", 0xDDA0DD },
    { "turquoise", 0x40E0D0 }, { "ivory", 0xFFFFF0 }, { "linen", 0xFAF0E6 },
    { "snow", 0xFFFAFA }, { "azure", 0xF0FFFF }, { "wheat", 0xF5DEB3 },
    { "darkred", 0x8B0000 }, { "darkblue", 0x00008B },
    { "darkgreen", 0x006400 }, { "darkgray", 0xA9A9A9 },
    { "darkgrey", 0xA9A9A9 }, { "darkorange", 0xFF8C00 },
    { "darkviolet", 0x9400D3 }, { "darkcyan", 0x008B8B },
    { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 },
    { "lightblue", 0xADD8E6 }, { "lightgreen", 0x90EE90 },
    { "lightyellow", 0xFFFFE0 }, { "lightpink", 0xFFB6C1 },
    { "lightcyan", 0xE0FFFF }, { "dimgray", 0x696969 },
    { "dimgrey", 0x696969 }, { "slategray", 0x708090 },
    { "steelblue", 0x4682B4 }, { "skyblue", 0x87CEEB },
    { "royalblue", 0x4169E1 }, { "midnightblue", 0x191970 },
    { "forestgreen", 0x228B22 }, { "seagreen", 0x2E8B57 },
    { "limegreen", 0x32CD32 }, { "springgreen", 0x00FF7F },
    { "tomato", 0xFF6347 }, { "firebrick", 0xB22222 },
    { "chocolate", 0xD2691E }, { "sienna", 0xA0522D },
    { "peru", 0xCD853F }, { "goldenrod", 0xDAA520 },
    { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC },
    { "aliceblue", 0xF0F8FF }, { "lavender", 0xE6E6FA },
    { "honeydew", 0xF0FFF0 }, { "mintcream", 0xF5FFFA },
    { "seashell", 0xFFF5EE }, { "cornsilk", 0xFFF8DC },
};

static int32_t hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool css_parse_color(const char *text, uint32_t *out)
{
    while (text && space_char(*text))
        text++;
    if (!text || !*text)
        return false;

    if (*text == '#') {
        const char *p = text + 1;
        int32_t digits[8];
        int32_t count = 0;

        while (count < 8 && hex_digit(p[count]) >= 0) {
            digits[count] = hex_digit(p[count]);
            count++;
        }
        if (count == 3 || count == 4) {
            *out = (uint32_t)((digits[0] * 17) << 16 |
                              (digits[1] * 17) << 8 | (digits[2] * 17));
            return true;
        }
        if (count == 6 || count == 8) {
            *out = (uint32_t)((digits[0] * 16 + digits[1]) << 16 |
                              (digits[2] * 16 + digits[3]) << 8 |
                              (digits[4] * 16 + digits[5]));
            return true;
        }
        return false;
    }

    if (strncasecmp(text, "rgb", 3) == 0) {
        const char *p = strchr(text, '(');

        if (!p)
            return false;
        p++;

        int32_t channel[3] = { 0, 0, 0 };

        for (int i = 0; i < 3; i++) {
            while (space_char(*p) || *p == ',')
                p++;

            int32_t value = parse_int(&p);

            if (*p == '%')
                value = value * 255 / 100, p++;
            channel[i] = CLAMP(value, 0, 255);
        }
        *out = ((uint32_t)channel[0] << 16) | ((uint32_t)channel[1] << 8) |
               (uint32_t)channel[2];
        return true;
    }

    char name[32];
    size_t at = 0;

    while (text[at] && !space_char(text[at]) && at + 1 < sizeof(name)) {
        name[at] = text[at];
        at++;
    }
    name[at] = '\0';
    lower_in_place(name);

    if (strcmp(name, "transparent") == 0)
        return false;

    for (size_t i = 0; i < ARRAY_LEN(color_names); i++) {
        if (strcmp(color_names[i].name, name) == 0) {
            *out = color_names[i].value;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Rechenausdruecke: calc(), min(), max() und clamp()                  */
/* ------------------------------------------------------------------ */

/* Gerechnet wird in Sechzehnteln eines Pixels - fein genug fuer
 * Prozentangaben und Vielfache der Schriftgroesse, und weit weg von
 * einem Ueberlauf. */
#define CALC_ONE 16

struct calc {
    const char *text;
    size_t      pos, length;
    int32_t     base;        /* Schriftgroesse fuer em   */
    int32_t     reference;   /* Bezugsmass fuer Prozente */
    bool        failed;
};

static int32_t calc_expression(struct calc *c);

static void calc_space(struct calc *c)
{
    while (c->pos < c->length && space_char(c->text[c->pos]))
        c->pos++;
}

static bool calc_word(struct calc *c, const char *word)
{
    size_t length = strlen(word);

    if (c->pos + length > c->length)
        return false;
    if (strncasecmp(c->text + c->pos, word, length) != 0)
        return false;
    c->pos += length;
    return true;
}

/* Liest eine Zahl mit Einheit; ohne Einheit ist es ein reiner Faktor. */
static int32_t calc_number(struct calc *c, bool *scalar)
{
    bool negative = false;

    if (c->pos < c->length && (c->text[c->pos] == '-' ||
                               c->text[c->pos] == '+'))
        negative = c->text[c->pos++] == '-';

    int64_t whole = 0, frac = 0, scale = 1;
    bool any = false;

    while (c->pos < c->length && c->text[c->pos] >= '0' &&
           c->text[c->pos] <= '9') {
        whole = whole * 10 + (c->text[c->pos++] - '0');
        any = true;
    }
    if (c->pos < c->length && c->text[c->pos] == '.') {
        c->pos++;
        while (c->pos < c->length && c->text[c->pos] >= '0' &&
               c->text[c->pos] <= '9' && scale < 100000) {
            frac = frac * 10 + (c->text[c->pos++] - '0');
            scale *= 10;
            any = true;
        }
        while (c->pos < c->length && c->text[c->pos] >= '0' &&
               c->text[c->pos] <= '9')
            c->pos++;
    }
    if (!any) {
        c->failed = true;
        return 0;
    }

    int64_t wert = whole * CALC_ONE + (frac * CALC_ONE) / scale;

    /* wert zaehlt bereits in Sechzehnteln; eine Einheit ist ein
     * Vielfaches davon, kein weiterer Bruch. */
    *scalar = false;
    if (calc_word(c, "px")) {
        /* nichts zu tun */
    } else if (calc_word(c, "rem") || calc_word(c, "em")) {
        wert = wert * c->base;
    } else if (calc_word(c, "pt")) {
        wert = wert * 4 / 3;
    } else if (calc_word(c, "vw")) {
        wert = wert * viewport_width / 100;
    } else if (calc_word(c, "vh")) {
        wert = wert * viewport_height / 100;
    } else if (calc_word(c, "vmin")) {
        wert = wert * MIN(viewport_width, viewport_height) / 100;
    } else if (calc_word(c, "vmax")) {
        wert = wert * MAX(viewport_width, viewport_height) / 100;
    } else if (calc_word(c, "ch") || calc_word(c, "ex")) {
        wert = wert * c->base / 2;
    } else if (calc_word(c, "cm")) {
        wert = wert * 38;
    } else if (calc_word(c, "mm")) {
        wert = wert * 4;
    } else if (calc_word(c, "in")) {
        wert = wert * 96;
    } else if (c->pos < c->length && c->text[c->pos] == '%') {
        c->pos++;
        wert = wert * c->reference / 100;
    } else {
        *scalar = true;
    }

    return negative ? (int32_t)-wert : (int32_t)wert;
}

/* Sammelt die durch Komma getrennten Teile von min(), max() und clamp(). */
static int32_t calc_arguments(struct calc *c, int32_t *out, int32_t max)
{
    int32_t count = 0;

    for (;;) {
        if (count < max)
            out[count] = calc_expression(c);
        else
            calc_expression(c);
        count++;
        calc_space(c);
        if (c->pos < c->length && c->text[c->pos] == ',') {
            c->pos++;
            continue;
        }
        break;
    }
    if (c->pos < c->length && c->text[c->pos] == ')')
        c->pos++;
    else
        c->failed = true;
    return count;
}

static int32_t calc_factor(struct calc *c, bool *scalar)
{
    calc_space(c);
    *scalar = false;
    if (c->pos >= c->length) {
        c->failed = true;
        return 0;
    }

    if (calc_word(c, "calc(")) {
        int32_t wert = calc_expression(c);

        calc_space(c);
        if (c->pos < c->length && c->text[c->pos] == ')')
            c->pos++;
        else
            c->failed = true;
        return wert;
    }
    if (calc_word(c, "min(") || calc_word(c, "max(")) {
        bool nimm_kleinstes = strncasecmp(c->text + c->pos - 4, "min(", 4) == 0;
        int32_t werte[8];
        int32_t count = calc_arguments(c, werte, ARRAY_LEN(werte));
        int32_t best = werte[0];

        for (int32_t i = 1; i < count && i < (int32_t)ARRAY_LEN(werte); i++)
            if (nimm_kleinstes ? werte[i] < best : werte[i] > best)
                best = werte[i];
        return best;
    }
    if (calc_word(c, "clamp(")) {
        int32_t werte[3] = { 0, 0, 0 };

        calc_arguments(c, werte, 3);
        return CLAMP(werte[1], werte[0], werte[2]);
    }
    if (c->text[c->pos] == '(') {
        c->pos++;

        int32_t wert = calc_expression(c);

        calc_space(c);
        if (c->pos < c->length && c->text[c->pos] == ')')
            c->pos++;
        else
            c->failed = true;
        return wert;
    }
    return calc_number(c, scalar);
}

static int32_t calc_term(struct calc *c)
{
    bool scalar_links;
    int32_t links = calc_factor(c, &scalar_links);

    for (;;) {
        calc_space(c);
        if (c->pos >= c->length)
            break;

        char op = c->text[c->pos];

        if (op != '*' && op != '/')
            break;
        c->pos++;

        bool scalar_rechts;
        int32_t rechts = calc_factor(c, &scalar_rechts);

        if (op == '*') {
            /* Einer der beiden Teile ist ein reiner Faktor. */
            if (scalar_rechts)
                links = (int32_t)(((int64_t)links * rechts) / CALC_ONE);
            else
                links = (int32_t)(((int64_t)links * rechts) / CALC_ONE);
        } else {
            if (rechts == 0)
                c->failed = true;
            else
                links = (int32_t)(((int64_t)links * CALC_ONE) / rechts);
        }
        scalar_links = scalar_links && scalar_rechts;
    }
    return links;
}

static int32_t calc_expression(struct calc *c)
{
    int32_t wert = calc_term(c);

    for (;;) {
        calc_space(c);
        if (c->pos >= c->length)
            break;

        char op = c->text[c->pos];

        if (op != '+' && op != '-')
            break;
        /* Vor und hinter dem Zeichen muss Platz sein - sonst waere es
         * das Vorzeichen der naechsten Zahl. */
        c->pos++;

        int32_t rechts = calc_term(c);

        wert = op == '+' ? wert + rechts : wert - rechts;
    }
    return wert;
}

/* Wertet einen Ausdruck aus; das Ergebnis sind ganze Pixel. */
static bool calc_pixels(const char *text, int32_t base, int32_t reference,
                        int32_t *out)
{
    while (text && space_char(*text))
        text++;
    if (!text)
        return false;
    if (strncasecmp(text, "calc(", 5) != 0 &&
        strncasecmp(text, "min(", 4) != 0 &&
        strncasecmp(text, "max(", 4) != 0 &&
        strncasecmp(text, "clamp(", 6) != 0)
        return false;

    struct calc c = { text, 0, strlen(text), base, reference, false };
    bool scalar;
    int32_t wert = calc_factor(&c, &scalar);

    if (c.failed)
        return false;
    *out = (wert + CALC_ONE / 2) / CALC_ONE;
    return true;
}

/* ------------------------------------------------------------------ */
/* Laengen                                                             */
/* ------------------------------------------------------------------ */

/* Rechnet eine Angabe in Pixel um. base ist die Schriftgroesse des
 * Elternteils, reference die Bezugsbreite fuer Prozentwerte. */
static int32_t length_px(const char *text, int32_t base, int32_t reference,
                         int32_t fallback)
{
    while (text && space_char(*text))
        text++;
    if (!text || !*text)
        return fallback;

    if (strncasecmp(text, "auto", 4) == 0 ||
        strncasecmp(text, "inherit", 7) == 0)
        return fallback;

    int32_t gerechnet;

    if (calc_pixels(text, base, reference, &gerechnet))
        return gerechnet;

    const char *p = text;
    int32_t value = parse_int(&p);

    while (space_char(*p))
        p++;

    if (strncasecmp(p, "px", 2) == 0 || *p == '\0' || *p == ';')
        return value;
    if (*p == '%')
        return reference > 0 ? value * reference / 100 : fallback;
    if (strncasecmp(p, "em", 2) == 0 || strncasecmp(p, "rem", 3) == 0)
        return value * base;
    if (strncasecmp(p, "pt", 2) == 0)
        return value * 4 / 3;
    if (strncasecmp(p, "pc", 2) == 0)
        return value * 16;
    if (strncasecmp(p, "vmin", 4) == 0)
        return value * MIN(viewport_width, viewport_height) / 100;
    if (strncasecmp(p, "vmax", 4) == 0)
        return value * MAX(viewport_width, viewport_height) / 100;
    if (strncasecmp(p, "vw", 2) == 0)
        return value * viewport_width / 100;
    if (strncasecmp(p, "vh", 2) == 0)
        return value * viewport_height / 100;
    if (strncasecmp(p, "cm", 2) == 0)
        return value * 38;
    if (strncasecmp(p, "mm", 2) == 0)
        return value * 4;
    if (strncasecmp(p, "in", 2) == 0)
        return value * 96;
    return value;
}

/* Liest Nachkommastellen mit, etwa fuer 1.5em - in Achtel-Pixeln. */
static int32_t length_px_fine(const char *text, int32_t base, int32_t fallback)
{
    while (text && space_char(*text))
        text++;
    if (!text || !*text)
        return fallback;

    int32_t gerechnet;

    if (calc_pixels(text, base, base, &gerechnet))
        return gerechnet;

    bool negative = false;
    const char *p = text;

    if (*p == '-') {
        negative = true;
        p++;
    } else if (*p == '+') {
        p++;
    }

    int32_t whole = 0, frac = 0, scale = 1;

    while (*p >= '0' && *p <= '9')
        whole = whole * 10 + (*p++ - '0');
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9' && scale < 1000) {
            frac = frac * 10 + (*p++ - '0');
            scale *= 10;
        }
    }
    while (space_char(*p))
        p++;

    int64_t thousandths = (int64_t)whole * 1000 +
                          (scale > 1 ? (int64_t)frac * 1000 / scale : 0);
    int64_t result;

    if (strncasecmp(p, "em", 2) == 0 || strncasecmp(p, "rem", 3) == 0)
        result = thousandths * base / 1000;
    else if (*p == '%')
        result = thousandths * base / 100000;
    else if (strncasecmp(p, "pt", 2) == 0)
        result = thousandths * 4 / 3000;
    else if (strncasecmp(p, "vw", 2) == 0)
        result = thousandths * viewport_width / 100000;
    else if (strncasecmp(p, "vh", 2) == 0)
        result = thousandths * viewport_height / 100000;
    else if (*p == '\0' || *p == ';')
        result = thousandths * base / 1000;   /* blosse Zahl: Vielfaches */
    else
        result = thousandths / 1000;

    return negative ? (int32_t)-result : (int32_t)result;
}

/* Zerlegt eine Kurzschreibweise wie "4px 8px" in vier Werte. */
static void parse_box(const char *text, int32_t base, int32_t reference,
                      int32_t *out)
{
    char parts[4][40];
    int32_t count = 0;
    const char *p = text;

    while (*p && count < 4) {
        while (space_char(*p))
            p++;
        if (!*p)
            break;

        size_t at = 0;

        while (*p && !space_char(*p) && at + 1 < sizeof(parts[0]))
            parts[count][at++] = *p++;
        parts[count][at] = '\0';
        count++;
    }
    if (count == 0)
        return;

    int32_t v[4];

    for (int32_t i = 0; i < count; i++)
        v[i] = length_px(parts[i], base, reference, 0);

    switch (count) {
    case 1: out[0] = out[1] = out[2] = out[3] = v[0]; break;
    case 2: out[0] = out[2] = v[0]; out[1] = out[3] = v[1]; break;
    case 3: out[0] = v[0]; out[1] = out[3] = v[1]; out[2] = v[2]; break;
    default: out[0] = v[0]; out[1] = v[1]; out[2] = v[2]; out[3] = v[3]; break;
    }
}

static struct length parse_length(const char *text, int32_t base)
{
    struct length l = { LEN_AUTO, 0 };

    while (text && space_char(*text))
        text++;
    if (!text || !*text || strncasecmp(text, "auto", 4) == 0)
        return l;

    int32_t gerechnet;

    if (calc_pixels(text, base, 0, &gerechnet)) {
        l.unit = LEN_PX;
        l.value = gerechnet;
        return l;
    }

    const char *p = text;
    int32_t value = parse_int(&p);

    while (space_char(*p))
        p++;
    if (*p == '%') {
        l.unit = LEN_PERCENT;
        l.value = value;
    } else {
        l.unit = LEN_PX;
        l.value = length_px(text, base, viewport_width, value);
    }
    return l;
}

/* ------------------------------------------------------------------ */
/* Eigenschaften auf einen Stil anwenden                               */
/* ------------------------------------------------------------------ */

static bool word_is(const char *value, const char *want)
{
    while (space_char(*value))
        value++;

    size_t length = strlen(want);

    if (strncasecmp(value, want, length) != 0)
        return false;

    char after = value[length];

    return after == '\0' || space_char(after) || after == ';' || after == ',';
}

/* Sucht in einer Wertliste nach einem Wort, etwa "underline" in
 * "underline dotted red". */
static bool has_word(const char *value, const char *want)
{
    size_t length = strlen(want);

    while (*value) {
        while (space_char(*value))
            value++;

        const char *start = value;

        while (*value && !space_char(*value))
            value++;
        if ((size_t)(value - start) == length &&
            strncasecmp(start, want, length) == 0)
            return true;
    }
    return false;
}

static void set_property(struct style *st, const char *name, const char *value,
                         const struct style *parent)
{
    int32_t base = parent ? parent->font_size : 16;
    uint32_t color;

    if (strcmp(name, "color") == 0) {
        if (css_parse_color(value, &color))
            st->color = color;
    } else if (strcmp(name, "background-color") == 0 ||
               strcmp(name, "background") == 0) {
        if (css_parse_color(value, &color)) {
            st->background = color;
            st->has_background = true;
        } else if (word_is(value, "none") || word_is(value, "transparent")) {
            st->has_background = false;
        }
    } else if (strcmp(name, "font-size") == 0) {
        if (word_is(value, "small"))
            st->font_size = 13;
        else if (word_is(value, "x-small"))
            st->font_size = 11;
        else if (word_is(value, "large"))
            st->font_size = 20;
        else if (word_is(value, "x-large"))
            st->font_size = 24;
        else if (word_is(value, "xx-large"))
            st->font_size = 32;
        else if (word_is(value, "medium"))
            st->font_size = 16;
        else
            st->font_size = CLAMP(length_px_fine(value, base, base), 6, 96);
    } else if (strcmp(name, "font-weight") == 0) {
        if (word_is(value, "bold") || word_is(value, "bolder") ||
            word_is(value, "600") || word_is(value, "700") ||
            word_is(value, "800") || word_is(value, "900"))
            st->bold = true;
        else if (word_is(value, "normal") || word_is(value, "400") ||
                 word_is(value, "300") || word_is(value, "lighter"))
            st->bold = false;
    } else if (strcmp(name, "font-style") == 0) {
        st->italic = word_is(value, "italic") || word_is(value, "oblique");
    } else if (strcmp(name, "font-family") == 0) {
        st->monospace = has_word(value, "monospace") ||
                        has_word(value, "courier") ||
                        has_word(value, "consolas") ||
                        has_word(value, "menlo") ||
                        has_word(value, "monaco");
    } else if (strcmp(name, "font") == 0) {
        /* Kurzschreibweise: Groesse und Fettung herausfischen. */
        if (has_word(value, "bold"))
            st->bold = true;
        if (has_word(value, "italic"))
            st->italic = true;
        st->monospace = has_word(value, "monospace");
    } else if (strcmp(name, "text-decoration") == 0 ||
               strcmp(name, "text-decoration-line") == 0) {
        st->underline = has_word(value, "underline");
        st->strike = has_word(value, "line-through");
    } else if (strcmp(name, "text-transform") == 0) {
        st->uppercase = word_is(value, "uppercase");
        st->lowercase = word_is(value, "lowercase");
    } else if (strcmp(name, "text-align") == 0) {
        if (word_is(value, "center"))
            st->align = ALIGN_CENTER;
        else if (word_is(value, "right"))
            st->align = ALIGN_RIGHT;
        else if (word_is(value, "justify"))
            st->align = ALIGN_JUSTIFY;
        else
            st->align = ALIGN_LEFT;
    } else if (strcmp(name, "display") == 0) {
        if (word_is(value, "none"))
            st->display = DISPLAY_NONE;
        else if (word_is(value, "block") || word_is(value, "flex") ||
                 word_is(value, "grid"))
            st->display = DISPLAY_BLOCK;
        else if (word_is(value, "inline-block"))
            st->display = DISPLAY_INLINE_BLOCK;
        else if (word_is(value, "list-item"))
            st->display = DISPLAY_LIST_ITEM;
        else if (word_is(value, "table"))
            st->display = DISPLAY_TABLE;
        else if (word_is(value, "table-row"))
            st->display = DISPLAY_TABLE_ROW;
        else if (word_is(value, "table-cell"))
            st->display = DISPLAY_TABLE_CELL;
        else
            st->display = DISPLAY_INLINE;
    } else if (strcmp(name, "visibility") == 0) {
        st->hidden = word_is(value, "hidden") || word_is(value, "collapse");
    } else if (strcmp(name, "opacity") == 0) {
        st->opacity = CLAMP(length_px_fine(value, 255, 255), 0, 255);
    } else if (strcmp(name, "white-space") == 0) {
        st->preformatted = word_is(value, "pre") || word_is(value, "pre-wrap") ||
                           word_is(value, "pre-line");
        st->nowrap = word_is(value, "nowrap") || word_is(value, "pre");
    } else if (strcmp(name, "margin") == 0) {
        parse_box(value, base, viewport_width, st->margin);

        /* "0 auto" heisst: waagerecht mittig. */
        char parts[4][40];
        int32_t count = 0;
        const char *p = value;

        while (*p && count < 4) {
            while (space_char(*p))
                p++;
            if (!*p)
                break;

            size_t at = 0;

            while (*p && !space_char(*p) && at + 1 < sizeof(parts[0]))
                parts[count][at++] = *p++;
            parts[count][at] = '\0';
            count++;
        }
        if (count >= 2) {
            bool automatic = strncasecmp(parts[1], "auto", 4) == 0;

            st->margin_auto_left = automatic;
            st->margin_auto_right = automatic;
            if (count >= 4)
                st->margin_auto_left = strncasecmp(parts[3], "auto", 4) == 0;
        } else if (count == 1) {
            bool automatic = strncasecmp(parts[0], "auto", 4) == 0;

            st->margin_auto_left = automatic;
            st->margin_auto_right = automatic;
        }
    } else if (strcmp(name, "margin-left") == 0 &&
               strncasecmp(value, "auto", 4) == 0) {
        st->margin_auto_left = true;
        st->margin[3] = 0;
    } else if (strcmp(name, "margin-right") == 0 &&
               strncasecmp(value, "auto", 4) == 0) {
        st->margin_auto_right = true;
        st->margin[1] = 0;
    } else if (strcmp(name, "margin-top") == 0) {
        st->margin[0] = length_px(value, base, 0, 0);
    } else if (strcmp(name, "margin-right") == 0) {
        st->margin[1] = length_px(value, base, 0, 0);
    } else if (strcmp(name, "margin-bottom") == 0) {
        st->margin[2] = length_px(value, base, 0, 0);
    } else if (strcmp(name, "margin-left") == 0) {
        st->margin[3] = length_px(value, base, 0, 0);
    } else if (strcmp(name, "padding") == 0) {
        parse_box(value, base, 0, st->padding);
    } else if (strcmp(name, "padding-top") == 0) {
        st->padding[0] = length_px(value, base, 0, 0);
    } else if (strcmp(name, "padding-right") == 0) {
        st->padding[1] = length_px(value, base, 0, 0);
    } else if (strcmp(name, "padding-bottom") == 0) {
        st->padding[2] = length_px(value, base, 0, 0);
    } else if (strcmp(name, "padding-left") == 0) {
        st->padding[3] = length_px(value, base, 0, 0);
    } else if (strncmp(name, "border", 6) == 0) {
        /* Randbreite und Randfarbe, gegebenenfalls nur fuer eine Seite. */
        int32_t side = -1;

        if (strncmp(name + 6, "-top", 4) == 0)
            side = 0;
        else if (strncmp(name + 6, "-right", 6) == 0)
            side = 1;
        else if (strncmp(name + 6, "-bottom", 7) == 0)
            side = 2;
        else if (strncmp(name + 6, "-left", 5) == 0)
            side = 3;

        if (strcmp(name, "border-radius") == 0) {
            st->border_radius = length_px(value, base, 0, 0);
            st->rounded_forced = true;
            return;
        }
        if (name_has(name, "width")) {
            int32_t width = length_px(value, base, 0, 1);

            if (side < 0)
                parse_box(value, base, 0, st->border);
            else
                st->border[side] = width;
            return;
        }
        if (name_has(name, "color")) {
            if (css_parse_color(value, &color)) {
                if (side < 0) {
                    for (int i = 0; i < 4; i++)
                        st->border_color[i] = color;
                } else {
                    st->border_color[side] = color;
                }
            }
            return;
        }
        if (name_has(name, "style")) {
            bool none = word_is(value, "none") || word_is(value, "hidden");

            if (none) {
                if (side < 0) {
                    for (int i = 0; i < 4; i++)
                        st->border[i] = 0;
                } else {
                    st->border[side] = 0;
                }
            }
            return;
        }

        /* Kurzschreibweise "1px solid #ccc" */
        bool none = has_word(value, "none");
        int32_t width = none ? 0 : 1;
        const char *p = value;

        while (*p && space_char(*p))
            p++;
        if (*p >= '0' && *p <= '9')
            width = length_px(value, base, 0, 1);

        uint32_t line = st->color;

        {
            /* Die Farbe steht irgendwo in der Liste. */
            const char *q = value;

            while (*q) {
                while (space_char(*q))
                    q++;

                const char *start = q;

                while (*q && !space_char(*q))
                    q++;

                char word[40];
                size_t wlen = MIN((size_t)(q - start), sizeof(word) - 1);

                memcpy(word, start, wlen);
                word[wlen] = '\0';
                if (css_parse_color(word, &color)) {
                    line = color;
                    break;
                }
            }
        }

        if (side < 0) {
            for (int i = 0; i < 4; i++) {
                st->border[i] = width;
                st->border_color[i] = line;
            }
        } else {
            st->border[side] = width;
            st->border_color[side] = line;
        }
    } else if (strcmp(name, "width") == 0) {
        st->width = parse_length(value, base);
    } else if (strcmp(name, "height") == 0) {
        st->height = parse_length(value, base);
    } else if (strcmp(name, "max-width") == 0) {
        struct length l = parse_length(value, base);

        if (st->width.unit == LEN_AUTO)
            st->width = l;
    } else if (strcmp(name, "line-height") == 0) {
        st->line_height = CLAMP(length_px_fine(value, st->font_size,
                                               st->line_height), 6, 200);
    } else if (strcmp(name, "letter-spacing") == 0) {
        st->letter_spacing = CLAMP(length_px(value, base, 0, 0), -4, 32);
    } else if (strcmp(name, "float") == 0) {
        st->float_left = word_is(value, "left");
        st->float_right = word_is(value, "right");
    } else if (strcmp(name, "clear") == 0) {
        st->clear_left = word_is(value, "left") || word_is(value, "both");
        st->clear_right = word_is(value, "right") || word_is(value, "both");
    } else if (strcmp(name, "position") == 0) {
        st->absolute = word_is(value, "absolute");
        st->fixed = word_is(value, "fixed");
        st->relative = word_is(value, "relative");
    } else if (strcmp(name, "left") == 0) {
        st->left = parse_length(value, base);
    } else if (strcmp(name, "top") == 0) {
        st->top = parse_length(value, base);
    } else if (strcmp(name, "right") == 0) {
        st->right = parse_length(value, base);
    } else if (strcmp(name, "bottom") == 0) {
        st->bottom = parse_length(value, base);
    } else if (strcmp(name, "z-index") == 0) {
        const char *p = value;

        st->z_index = parse_int(&p);
    }
}

/* ------------------------------------------------------------------ */
/* Regeln lesen                                                        */
/* ------------------------------------------------------------------ */

static struct declaration *parse_declarations(const char *text, size_t length)
{
    struct declaration *head = NULL, *tail = NULL;
    size_t pos = 0;

    while (pos < length) {
        while (pos < length && (space_char(text[pos]) || text[pos] == ';'))
            pos++;
        if (pos >= length)
            break;

        size_t name_start = pos;

        while (pos < length && text[pos] != ':' && text[pos] != ';' &&
               text[pos] != '}')
            pos++;
        if (pos >= length || text[pos] != ':') {
            while (pos < length && text[pos] != ';')
                pos++;
            continue;
        }

        size_t name_end = pos++;

        while (name_end > name_start && space_char(text[name_end - 1]))
            name_end--;

        size_t value_start = pos;
        int32_t nesting = 0;

        while (pos < length &&
               (nesting > 0 || (text[pos] != ';' && text[pos] != '}'))) {
            if (text[pos] == '(')
                nesting++;
            else if (text[pos] == ')')
                nesting--;
            pos++;
        }

        size_t value_end = pos;

        while (value_end > value_start && space_char(text[value_end - 1]))
            value_end--;

        if (name_end <= name_start || value_end <= value_start)
            continue;

        struct declaration *d = kmalloc(sizeof(*d));

        if (!d)
            break;
        d->name = dup_range(text + name_start, name_end - name_start);
        d->value = dup_range(text + value_start, value_end - value_start);
        d->next = NULL;
        d->important = false;
        if (d->name)
            lower_in_place(d->name);

        /* Ein nachgestelltes !important abschneiden und merken. */
        if (d->value) {
            char *bang = NULL;

            for (char *q = d->value; *q; q++)
                if (*q == '!')
                    bang = q;
            if (bang && strncasecmp(bang + 1, "important", 9) == 0) {
                d->important = true;
                while (bang > d->value && space_char(bang[-1]))
                    bang--;
                *bang = '\0';
            }
        }

        if (tail)
            tail->next = d;
        else
            head = d;
        tail = d;
    }
    return head;
}

static void free_declarations(struct declaration *list)
{
    while (list) {
        struct declaration *next = list->next;

        kfree(list->name);
        kfree(list->value);
        kfree(list);
        list = next;
    }
}

/* Zerlegt einen Selektor und berechnet sein Gewicht. */
static struct selector *parse_selector(const char *text, size_t length)
{
    struct selector *s = kmalloc(sizeof(*s));

    if (!s)
        return NULL;
    memset(s, 0, sizeof(*s));

    size_t pos = 0;
    bool unusable = false;

    while (pos < length && s->count < MAX_PARTS) {
        while (pos < length && (space_char(text[pos]) || text[pos] == '>' ||
                                text[pos] == '+' || text[pos] == '~'))
            pos++;
        if (pos >= length)
            break;

        size_t at = 0;

        while (pos < length && !space_char(text[pos]) && text[pos] != '>' &&
               text[pos] != '+' && text[pos] != '~') {
            char c = text[pos];

            /* Klammerausdruecke und Pseudoelemente uebergehen wir. */
            if (c == '[') {
                while (pos < length && text[pos] != ']')
                    pos++;
                pos++;
                continue;
            }
            if (c == ':') {
                /* ":root" meint das oberste Element. Alles andere -
                 * :hover, :focus, ::before - haengt an einem Zustand,
                 * den die Seite beim Laden nicht hat; solche Regeln
                 * werden verworfen statt staendig angewandt. */
                size_t start = pos;

                while (pos < length && text[pos] == ':')
                    pos++;
                while (pos < length && !space_char(text[pos]) &&
                       text[pos] != '>' && text[pos] != ',' &&
                       text[pos] != '.' && text[pos] != '#' &&
                       text[pos] != ':')
                    pos++;

                size_t plen = pos - start;

                if (plen == 5 && strncasecmp(text + start, ":root", 5) == 0 &&
                    at == 0) {
                    memcpy(s->parts[s->count], ":root", 5);
                    at = 5;
                } else {
                    unusable = true;
                }
                continue;
            }
            if (at + 1 < sizeof(s->parts[0]))
                s->parts[s->count][at++] = c;
            pos++;
        }
        s->parts[s->count][at] = '\0';
        if (at == 0)
            continue;

        lower_in_place(s->parts[s->count]);

        for (size_t i = 0; i < at; i++) {
            if (s->parts[s->count][i] == '#')
                s->weight += 10000;
            else if (s->parts[s->count][i] == '.')
                s->weight += 100;
        }
        if (s->parts[s->count][0] != '#' && s->parts[s->count][0] != '.' &&
            s->parts[s->count][0] != '*')
            s->weight += 1;
        s->count++;
    }

    if (s->count == 0 || unusable) {
        kfree(s);
        return NULL;
    }
    return s;
}

/* Ueberspringt eine at-Regel wie @media, uebernimmt aber deren Inhalt,
 * damit einfache Bildschirmabfragen nicht die halbe Seite verschlucken. */
static size_t skip_at_rule(struct stylesheet *sheet, const char *text,
                           size_t pos, size_t length);

static void add_rules(struct stylesheet *sheet, const char *text, size_t length)
{
    size_t pos = 0;

    while (pos < length) {
        while (pos < length && space_char(text[pos]))
            pos++;
        if (pos >= length)
            break;

        if (text[pos] == '@') {
            pos = skip_at_rule(sheet, text, pos, length);
            continue;
        }
        if (text[pos] == '}') {
            pos++;
            continue;
        }

        size_t selector_start = pos;

        while (pos < length && text[pos] != '{' && text[pos] != '}')
            pos++;
        if (pos >= length || text[pos] != '{')
            break;

        size_t selector_end = pos++;
        size_t body_start = pos;
        int32_t nesting = 1;

        while (pos < length && nesting > 0) {
            if (text[pos] == '{')
                nesting++;
            else if (text[pos] == '}')
                nesting--;
            pos++;
        }

        size_t body_end = nesting == 0 ? pos - 1 : length;

        struct rule *rule = kmalloc(sizeof(*rule));

        if (!rule)
            break;
        rule->selectors = NULL;
        rule->next = NULL;
        rule->declarations = parse_declarations(text + body_start,
                                                body_end - body_start);

        /* Mehrere Selektoren durch Komma getrennt. */
        size_t at = selector_start;

        while (at < selector_end) {
            size_t start = at;

            while (at < selector_end && text[at] != ',')
                at++;

            struct selector *s = parse_selector(text + start, at - start);

            if (s) {
                s->next = rule->selectors;
                rule->selectors = s;
            }
            if (at < selector_end)
                at++;
        }

        if (!rule->selectors || !rule->declarations) {
            struct declaration *d = rule->declarations;

            while (d) {
                struct declaration *next = d->next;

                kfree(d->name);
                kfree(d->value);
                kfree(d);
                d = next;
            }

            struct selector *s = rule->selectors;

            while (s) {
                struct selector *next = s->next;

                kfree(s);
                s = next;
            }
            kfree(rule);
            continue;
        }

        if (sheet->last)
            sheet->last->next = rule;
        else
            sheet->rules = rule;
        sheet->last = rule;
    }
}

static size_t skip_at_rule(struct stylesheet *sheet, const char *text,
                           size_t pos, size_t length)
{
    size_t name_start = pos + 1;
    size_t name_end = name_start;

    while (name_end < length && !space_char(text[name_end]) &&
           text[name_end] != '{' && text[name_end] != ';')
        name_end++;

    bool take_body = (name_end - name_start == 5 &&
                      strncasecmp(text + name_start, "media", 5) == 0) ||
                     (name_end - name_start == 8 &&
                      strncasecmp(text + name_start, "supports", 8) == 0);

    while (pos < length && text[pos] != '{' && text[pos] != ';')
        pos++;
    if (pos >= length)
        return length;
    if (text[pos] == ';')
        return pos + 1;

    size_t body_start = ++pos;
    int32_t nesting = 1;

    while (pos < length && nesting > 0) {
        if (text[pos] == '{')
            nesting++;
        else if (text[pos] == '}')
            nesting--;
        pos++;
    }

    size_t body_end = nesting == 0 ? pos - 1 : length;

    if (take_body)
        add_rules(sheet, text + body_start, body_end - body_start);
    return pos;
}

/* ------------------------------------------------------------------ */
/* Eingebaute Vorgaben                                                 */
/* ------------------------------------------------------------------ */

static const char default_sheet[] =
    "html,body{display:block;margin:0;padding:0}"
    "div,section,article,aside,nav,header,footer,main,figure,figcaption,"
    "address,fieldset,form,dl,dt,dd,hr,pre,center{display:block}"
    "p{display:block;margin:12px 0}"
    "h1{display:block;font-size:32px;font-weight:bold;margin:20px 0 12px 0}"
    "h2{display:block;font-size:26px;font-weight:bold;margin:18px 0 10px 0}"
    "h3{display:block;font-size:22px;font-weight:bold;margin:16px 0 8px 0}"
    "h4{display:block;font-size:18px;font-weight:bold;margin:14px 0 8px 0}"
    "h5{display:block;font-size:16px;font-weight:bold;margin:12px 0 6px 0}"
    "h6{display:block;font-size:14px;font-weight:bold;margin:12px 0 6px 0}"
    "b,strong{font-weight:bold}"
    "i,em,cite,var,dfn{font-style:italic}"
    "u,ins{text-decoration:underline}"
    "s,strike,del{text-decoration:line-through}"
    "small{font-size:13px}"
    "big{font-size:20px}"
    "code,kbd,samp,tt{font-family:monospace}"
    "pre{font-family:monospace;white-space:pre;margin:12px 0}"
    "a{color:#0645ad;text-decoration:underline}"
    "ul,ol{display:block;margin:12px 0;padding-left:32px}"
    "li{display:list-item;margin:3px 0}"
    "dd{margin-left:32px}"
    "dt{font-weight:bold}"
    "blockquote{display:block;margin:12px 24px;padding-left:12px;"
    "border-left:3px solid #cccccc;color:#555555}"
    "hr{display:block;margin:12px 0;border-top:1px solid #bbbbbb}"
    "table{display:table;margin:8px 0}"
    "tr{display:table-row}"
    "td,th{display:table-cell;padding:4px 8px;border:1px solid #cccccc}"
    "th{font-weight:bold;background-color:#eeeeee}"
    "thead,tbody,tfoot{display:block}"
    "caption{display:block;text-align:center;font-weight:bold;margin:4px 0}"
    "head,script,style,title,meta,link,base,noscript{display:none}"
    "img{display:inline-block}"
    "button,input,select,textarea{display:inline-block;font-size:15px;"
    "padding:3px 6px;border:1px solid #808080;background-color:#e8e8e8}"
    "textarea{font-family:monospace}"
    "mark{background-color:#ffff66}"
    "sub,sup{font-size:12px}"
    "abbr{text-decoration:underline}"
    "center{text-align:center}"
    "figure{margin:12px 24px}"
    "label{display:inline}"
    "span,a,b,i,em,strong,code,small,big,u,s,label,abbr,cite,q,sub,sup,"
    "mark,time,var,kbd,samp,tt,ins,del,br,img{display:inline}"
    "br{display:inline}";

/* ------------------------------------------------------------------ */
/* Regelwerk                                                           */
/* ------------------------------------------------------------------ */

struct stylesheet *css_create(void)
{
    struct stylesheet *sheet = kmalloc(sizeof(*sheet));

    if (!sheet)
        return NULL;
    memset(sheet, 0, sizeof(*sheet));
    add_rules(sheet, default_sheet, sizeof(default_sheet) - 1);
    return sheet;
}

void css_add(struct stylesheet *sheet, const char *text, size_t length)
{
    if (sheet && text && length)
        add_rules(sheet, text, length);
}

void css_free(struct stylesheet *sheet)
{
    if (!sheet)
        return;

    struct rule *rule = sheet->rules;

    while (rule) {
        struct rule *next = rule->next;
        struct declaration *d = rule->declarations;

        while (d) {
            struct declaration *dnext = d->next;

            kfree(d->name);
            kfree(d->value);
            kfree(d);
            d = dnext;
        }

        struct selector *s = rule->selectors;

        while (s) {
            struct selector *snext = s->next;

            kfree(s);
            s = snext;
        }
        kfree(rule);
        rule = next;
    }
    kfree(sheet);
}

/* ------------------------------------------------------------------ */
/* Zuordnung                                                           */
/* ------------------------------------------------------------------ */

static bool part_matches(const struct node *node, const char *part)
{
    if (node->kind != NODE_ELEMENT || !node->name)
        return false;

    /* ":root" ist das oberste Element des Dokuments. */
    if (part[0] == ':') {
        if (strcmp(part, ":root") != 0)
            return false;
        return node->parent && node->parent->kind == NODE_DOCUMENT;
    }

    const char *p = part;
    char buffer[80];

    while (*p) {
        char kind = 0;

        if (*p == '.' || *p == '#') {
            kind = *p++;
        } else if (*p == '*') {
            p++;
            continue;
        }

        size_t at = 0;

        while (*p && *p != '.' && *p != '#' && at + 1 < sizeof(buffer))
            buffer[at++] = *p++;
        buffer[at] = '\0';
        if (at == 0)
            return false;

        if (kind == '.') {
            if (!node_has_class(node, buffer))
                return false;
        } else if (kind == '#') {
            const char *id = node_attribute(node, "id");

            if (!id || strcmp(id, buffer) != 0)
                return false;
        } else {
            if (strcasecmp(node->name, buffer) != 0)
                return false;
        }
    }
    return true;
}

static bool selector_matches(const struct node *node, const struct selector *s)
{
    if (!part_matches(node, s->parts[s->count - 1]))
        return false;

    const struct node *current = node->parent;

    for (int32_t i = s->count - 2; i >= 0; i--) {
        bool found = false;

        while (current) {
            if (part_matches(current, s->parts[i])) {
                found = true;
                current = current->parent;
                break;
            }
            current = current->parent;
        }
        if (!found)
            return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Anwenden                                                            */
/* ------------------------------------------------------------------ */

void css_apply_inline(struct style *style, const char *text,
                      const struct style *parent)
{
    struct declaration *list = parse_declarations(text, strlen(text));

    for (struct declaration *d = list; d; d = d->next)
        set_property(style, d->name, d->value, parent);

    free_declarations(list);
}

/* Die vererbbaren Eigenschaften kommen vom Elternteil, der Rest wird
 * zurueckgesetzt. */
static void inherit(struct style *st, const struct style *parent)
{
    memset(st, 0, sizeof(*st));

    st->color = parent->color;
    st->font_size = parent->font_size;
    st->bold = parent->bold;
    st->italic = parent->italic;
    st->underline = parent->underline;
    st->strike = parent->strike;
    st->monospace = parent->monospace;
    st->preformatted = parent->preformatted;
    st->uppercase = parent->uppercase;
    st->lowercase = parent->lowercase;
    st->align = parent->align;
    st->line_height = parent->line_height;
    st->letter_spacing = parent->letter_spacing;
    st->hidden = parent->hidden;
    st->nowrap = parent->nowrap;
    st->opacity = parent->opacity;
    st->display = DISPLAY_INLINE;
    st->width.unit = LEN_AUTO;
    st->height.unit = LEN_AUTO;
    st->left.unit = LEN_AUTO;
    st->top.unit = LEN_AUTO;
    st->right.unit = LEN_AUTO;
    st->bottom.unit = LEN_AUTO;
}

/* Altertuemliche Attribute, die viele Seiten noch tragen. */
static void legacy_attributes(struct node *node, struct style *st)
{
    const char *value;
    uint32_t color;

    if ((value = node_attribute(node, "color")) && css_parse_color(value, &color))
        st->color = color;
    if ((value = node_attribute(node, "bgcolor")) &&
        css_parse_color(value, &color)) {
        st->background = color;
        st->has_background = true;
    }
    if ((value = node_attribute(node, "align"))) {
        if (strcasecmp(value, "center") == 0)
            st->align = ALIGN_CENTER;
        else if (strcasecmp(value, "right") == 0)
            st->align = ALIGN_RIGHT;
        else if (strcasecmp(value, "left") == 0)
            st->align = ALIGN_LEFT;
    }
    if ((value = node_attribute(node, "border")) && node->name &&
        strcmp(node->name, "table") == 0) {
        const char *p = value;
        int32_t width = parse_int(&p);

        for (int i = 0; i < 4; i++)
            st->border[i] = width;
    }
    if (node->name && strcmp(node->name, "font") == 0 &&
        (value = node_attribute(node, "size"))) {
        const char *p = value;
        int32_t size = parse_int(&p);

        if (value[0] == '+' || value[0] == '-')
            st->font_size = CLAMP(st->font_size + size * 3, 8, 48);
        else if (size >= 1 && size <= 7)
            st->font_size = 8 + size * 4;
    }
}

struct match {
    struct rule *rule;
    int32_t      weight;
    uint32_t     order;
};

/* Legt die eigenen Eigenschaften einer Regelliste ab. */
static void collect_vars(struct cascade *c, struct declaration *list)
{
    for (struct declaration *d = list; d; d = d->next) {
        if (strncmp(d->name, "--", 2) != 0)
            continue;

        char wert[192];

        var_expand(c, d->value, wert, sizeof(wert), 0);
        var_define(c, d->name, wert);
    }
}

/* Wendet eine Regelliste an und loest dabei var() auf. */
static void apply_declarations(struct cascade *c, struct style *style,
                               struct declaration *list,
                               const struct style *parent, bool only_important)
{
    for (struct declaration *d = list; d; d = d->next) {
        if (strncmp(d->name, "--", 2) == 0)
            continue;
        if (only_important && !d->important)
            continue;

        /* Nur wenn wirklich var() vorkommt, wird umkopiert. */
        if (name_has(d->value, "var(")) {
            char wert[192];

            var_expand(c, d->value, wert, sizeof(wert), 0);
            set_property(style, d->name, wert, parent);
        } else {
            set_property(style, d->name, d->value, parent);
        }
    }
}

static void apply_node(struct cascade *c, struct node *node,
                       const struct style *parent)
{
    if (node->kind != NODE_ELEMENT) {
        inherit(&node->style, parent);
        node->styled = true;
        for (struct node *child = node->first; child; child = child->next)
            apply_node(c, child, &node->style);
        return;
    }

    inherit(&node->style, parent);

    /* Die passenden Regeln in der Reihenfolge steigenden Gewichts
     * anwenden. Bei gleichem Gewicht gewinnt die spaetere Regel. */
    struct match matches[96];
    size_t count = 0;
    uint32_t order = 0;

    for (struct rule *rule = c->sheet->rules; rule;
         rule = rule->next, order++) {
        int32_t best = -1;

        for (struct selector *s = rule->selectors; s; s = s->next)
            if (selector_matches(node, s) && s->weight > best)
                best = s->weight;

        if (best < 0)
            continue;
        if (count < ARRAY_LEN(matches)) {
            matches[count].rule = rule;
            matches[count].weight = best;
            matches[count].order = order;
            count++;
        }
    }

    /* Einfaches Einfuegesortieren - die Listen sind kurz. */
    for (size_t i = 1; i < count; i++) {
        struct match key = matches[i];
        size_t j = i;

        while (j > 0 && (matches[j - 1].weight > key.weight ||
                         (matches[j - 1].weight == key.weight &&
                          matches[j - 1].order > key.order))) {
            matches[j] = matches[j - 1];
            j--;
        }
        matches[j] = key;
    }

    /* Eigene Eigenschaften zuerst - der Rest darf sie schon benutzen. */
    int32_t mark = c->var_count;

    for (size_t i = 0; i < count; i++)
        collect_vars(c, matches[i].rule->declarations);

    const char *inline_style = node_attribute(node, "style");
    struct declaration *inline_list = NULL;

    if (inline_style) {
        inline_list = parse_declarations(inline_style, strlen(inline_style));
        collect_vars(c, inline_list);
    }

    for (size_t i = 0; i < count; i++)
        apply_declarations(c, &node->style, matches[i].rule->declarations,
                           parent, false);

    legacy_attributes(node, &node->style);

    if (inline_list)
        apply_declarations(c, &node->style, inline_list, parent, false);

    /* Wichtige Eigenschaften zum Schluss noch einmal. */
    for (size_t i = 0; i < count; i++)
        apply_declarations(c, &node->style, matches[i].rule->declarations,
                           parent, true);
    if (inline_list) {
        apply_declarations(c, &node->style, inline_list, parent, true);
        free_declarations(inline_list);
    }

    if (node->style.line_height < node->style.font_size)
        node->style.line_height = node->style.font_size * 5 / 4;

    node->styled = true;

    if (node->style.display != DISPLAY_NONE)
        for (struct node *child = node->first; child; child = child->next)
            apply_node(c, child, &node->style);

    /* Was dieser Knoten festgelegt hat, gilt fuer Geschwister nicht. */
    c->var_count = mark;
}

void css_apply(struct stylesheet *sheet, struct node *root, int32_t base_size,
               int32_t viewport_w, int32_t viewport_h)
{
    if (!sheet || !root)
        return;

    viewport_width = viewport_w > 0 ? viewport_w : 800;
    viewport_height = viewport_h > 0 ? viewport_h : 600;

    struct style initial;

    memset(&initial, 0, sizeof(initial));
    initial.color = 0x1A1A1A;
    initial.font_size = base_size;
    initial.line_height = base_size * 5 / 4;
    initial.display = DISPLAY_BLOCK;
    initial.align = ALIGN_LEFT;
    initial.opacity = 255;

    struct cascade *c = kmalloc(sizeof(*c));

    if (!c)
        return;
    memset(c, 0, sizeof(*c));
    c->sheet = sheet;

    apply_node(c, root, &initial);
    kfree(c);
}
