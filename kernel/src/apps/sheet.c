/* sheet.c - Zellen, Zahlen und Formeln.
 *
 * Der Auswerter ist ein Rekursivabstieg ueber den Text der Formel.
 * Einen Baum baut er nicht: Jede Zelle wird beim Neuberechnen genau
 * einmal gelesen, und dabei gleich gerechnet. Fuer ein Gitter dieser
 * Groesse ist das schnell genug und spart die halbe Datenstruktur.
 *
 * Gerechnet wird in Festkomma. Multiplikation und Division muessen
 * dabei durch 128 Bit, sonst laeuft schon ein Produkt aus zwei
 * vierstelligen Zahlen ueber.
 */

#include "sheet.h"
#include "kstring.h"

#define STATE_FRESH   0
#define STATE_RUNNING 1
#define STATE_DONE    2

/* ------------------------------------------------------------------ */
/* Festkommarechnung                                                   */
/* ------------------------------------------------------------------ */

/* Teilen mit kaufmaennischem Runden - auch fuer negative Zahlen. */
static int64_t div_round(__int128 zaehler, __int128 nenner)
{
    if (nenner == 0)
        return 0;

    bool negative = (zaehler < 0) != (nenner < 0);
    __int128 a = zaehler < 0 ? -zaehler : zaehler;
    __int128 b = nenner < 0 ? -nenner : nenner;
    __int128 result = (a + b / 2) / b;

    return (int64_t)(negative ? -result : result);
}

static sheet_num num_mul(sheet_num a, sheet_num b)
{
    return div_round((__int128)a * b, SHEET_SCALE);
}

static sheet_num num_div(sheet_num a, sheet_num b)
{
    return div_round((__int128)a * SHEET_SCALE, b);
}

/* Ganzzahlige Wurzel aus einem Festkommawert. */
static sheet_num num_sqrt(sheet_num a)
{
    if (a <= 0)
        return 0;

    __int128 target = (__int128)a * SHEET_SCALE;
    __int128 guess = 1;

    /* Erst grob verdoppeln, bis es passt, dann Newton. */
    while (guess * guess < target)
        guess *= 2;

    for (int i = 0; i < 64; i++) {
        __int128 next = (guess + target / guess) / 2;

        if (next == guess)
            break;
        guess = next;
    }
    return (sheet_num)guess;
}

/* ------------------------------------------------------------------ */
/* Zahlen lesen und schreiben                                          */
/* ------------------------------------------------------------------ */

bool sheet_parse_number(const char *text, sheet_num *out)
{
    if (!text)
        return false;

    while (*text == ' ')
        text++;

    bool negative = false;

    if (*text == '-' || *text == '+') {
        negative = *text == '-';
        text++;
    }

    if (!(*text >= '0' && *text <= '9') && *text != '.' && *text != ',')
        return false;

    int64_t whole = 0;
    bool any = false;

    while (*text >= '0' && *text <= '9') {
        if (whole > (INT64_MAX / 20))
            return false;                   /* zu gross fuer uns */
        whole = whole * 10 + (*text - '0');
        text++;
        any = true;
    }

    int64_t fraction = 0;
    int64_t step = SHEET_SCALE / 10;

    if (*text == '.' || *text == ',') {
        text++;
        int64_t rest = 0;
        int digits = 0;

        while (*text >= '0' && *text <= '9') {
            if (digits < 4) {
                fraction += (*text - '0') * step;
                step /= 10;
            } else if (digits == 4) {
                rest = *text - '0';         /* fuer das Runden */
            }
            digits++;
            text++;
            any = true;
        }
        if (rest >= 5)
            fraction++;
    }

    while (*text == ' ')
        text++;
    if (!any || *text)
        return false;

    int64_t value = whole * SHEET_SCALE + fraction;

    if (out)
        *out = negative ? -value : value;
    return true;
}

void sheet_format_number(sheet_num value, char *out, size_t size)
{
    if (size == 0)
        return;

    bool negative = value < 0;
    uint64_t magnitude = (uint64_t)(negative ? -value : value);
    uint64_t whole = magnitude / SHEET_SCALE;
    uint64_t fraction = magnitude % SHEET_SCALE;

    char digits[24];
    size_t n = 0;

    if (fraction) {
        char frac[8];

        /* Vier Stellen, dann die Nullen hinten weg. */
        for (int i = 3; i >= 0; i--) {
            frac[i] = (char)('0' + fraction % 10);
            fraction /= 10;
        }

        int last = 3;

        while (last >= 0 && frac[last] == '0')
            last--;

        for (int i = last; i >= 0; i--)
            digits[n++] = frac[i];
        digits[n++] = ',';
    }

    if (whole == 0) {
        digits[n++] = '0';
    } else {
        while (whole > 0 && n + 1 < sizeof(digits)) {
            digits[n++] = (char)('0' + whole % 10);
            whole /= 10;
        }
    }
    if (negative && n + 1 < sizeof(digits))
        digits[n++] = '-';

    size_t at = 0;

    while (n > 0 && at + 1 < size)
        out[at++] = digits[--n];
    out[at] = '\0';
}

/* ------------------------------------------------------------------ */
/* Bezuege                                                             */
/* ------------------------------------------------------------------ */

bool sheet_parse_ref(const char *text, int *row, int *col)
{
    if (!text)
        return false;

    if (*text == '$')
        text++;

    char letter = *text;

    if (letter >= 'a' && letter <= 'z')
        letter = (char)(letter - 32);
    if (letter < 'A' || letter > 'Z')
        return false;
    text++;

    if (*text == '$')
        text++;
    if (!(*text >= '1' && *text <= '9'))
        return false;

    int number = 0;

    while (*text >= '0' && *text <= '9') {
        number = number * 10 + (*text - '0');
        if (number > SHEET_ROWS)
            return false;
        text++;
    }
    if (*text)
        return false;

    if (row)
        *row = number - 1;
    if (col)
        *col = letter - 'A';
    return true;
}

void sheet_ref_name(int row, int col, char *out, size_t size)
{
    ksnprintf(out, size, "%c%d", 'A' + col, row + 1);
}

const char *sheet_error_text(enum cell_error error)
{
    switch (error) {
    case CELL_ERR_SYNTAX: return "#FORMEL";
    case CELL_ERR_CYCLE:  return "#KREIS";
    case CELL_ERR_REF:    return "#BEZUG";
    case CELL_ERR_DIV0:   return "#DIV/0";
    case CELL_ERR_NAME:   return "#NAME";
    case CELL_ERR_VALUE:  return "#WERT";
    default:              return "";
    }
}

/* ------------------------------------------------------------------ */
/* Auswerten                                                           */
/* ------------------------------------------------------------------ */

struct parser {
    struct sheet    *sh;
    const char      *p;
    enum cell_error  error;
};

/* Ein Argument ist entweder ein Wert oder ein Bereich. */
struct arg {
    bool      is_range;
    sheet_num value;
    int       r1, c1, r2, c2;
};

static sheet_num eval_cell(struct sheet *sh, int row, int col,
                           enum cell_error *error);
static sheet_num parse_expr(struct parser *ps);

static void skip_spaces(struct parser *ps)
{
    while (*ps->p == ' ')
        ps->p++;
}

static void fail(struct parser *ps, enum cell_error error)
{
    if (ps->error == CELL_OK)
        ps->error = error;
}

/* Liest einen Namen: Buchstaben, Ziffern, Punkt und Dollarzeichen. */
static size_t read_name(struct parser *ps, char *out, size_t size)
{
    size_t n = 0;

    while ((*ps->p >= 'A' && *ps->p <= 'Z') ||
           (*ps->p >= 'a' && *ps->p <= 'z') ||
           (*ps->p >= '0' && *ps->p <= '9') ||
           *ps->p == '$' || *ps->p == '_') {
        if (n + 1 < size)
            out[n++] = *ps->p;
        ps->p++;
    }
    out[n] = '\0';
    return n;
}

static bool name_is(const char *name, const char *a, const char *b)
{
    return strcasecmp(name, a) == 0 || (b && strcasecmp(name, b) == 0);
}

/* Holt den Wert einer Zelle - der Weg, auf dem sich Formeln
 * gegenseitig benutzen. */
static sheet_num cell_value(struct parser *ps, int row, int col)
{
    if (row < 0 || row >= SHEET_ROWS || col < 0 || col >= SHEET_COLS) {
        fail(ps, CELL_ERR_REF);
        return 0;
    }

    enum cell_error error = CELL_OK;
    sheet_num value = eval_cell(ps->sh, row, col, &error);

    if (error != CELL_OK)
        fail(ps, error);
    return value;
}

/* Laeuft ueber einen Bereich und ruft fuer jede Zelle die Funktion. */
typedef void (*fold_fn)(sheet_num value, bool filled, void *context);

static void fold_arg(struct parser *ps, const struct arg *arg,
                     fold_fn step, void *context)
{
    if (!arg->is_range) {
        step(arg->value, true, context);
        return;
    }

    for (int r = arg->r1; r <= arg->r2; r++) {
        for (int c = arg->c1; c <= arg->c2; c++) {
            const struct cell *cell = &ps->sh->cells[r][c];
            bool filled = cell->kind == CELL_NUMBER ||
                          cell->kind == CELL_FORMULA;

            step(filled ? cell_value(ps, r, c) : 0, filled, context);
        }
    }
}

struct fold_state {
    sheet_num sum;
    sheet_num best;
    size_t    count;      /* nur Zahlen           */
    size_t    seen;       /* auch leere Zellen    */
    bool      first;
};

static void fold_sum(sheet_num value, bool filled, void *context)
{
    struct fold_state *st = context;

    st->seen++;
    if (!filled)
        return;
    st->sum += value;
    st->count++;
}

static void fold_min(sheet_num value, bool filled, void *context)
{
    struct fold_state *st = context;

    if (!filled)
        return;
    if (st->first || value < st->best) {
        st->best = value;
        st->first = false;
    }
    st->count++;
}

static void fold_max(sheet_num value, bool filled, void *context)
{
    struct fold_state *st = context;

    if (!filled)
        return;
    if (st->first || value > st->best) {
        st->best = value;
        st->first = false;
    }
    st->count++;
}

/* Liest die Argumentliste hinter einer Funktion. */
static size_t parse_args(struct parser *ps, struct arg *args, size_t max)
{
    size_t count = 0;

    skip_spaces(ps);
    if (*ps->p == ')') {
        ps->p++;
        return 0;
    }

    for (;;) {
        struct arg arg;

        memset(&arg, 0, sizeof(arg));

        /* Ein Bereich sieht am Anfang aus wie ein Bezug - erst der
         * Doppelpunkt entscheidet. */
        const char *save = ps->p;
        char name[16];

        skip_spaces(ps);
        if (read_name(ps, name, sizeof(name)) > 0 && *ps->p == ':') {
            int r1, c1, r2, c2;
            char second[16];

            ps->p++;
            read_name(ps, second, sizeof(second));

            if (sheet_parse_ref(name, &r1, &c1) &&
                sheet_parse_ref(second, &r2, &c2)) {
                arg.is_range = true;
                arg.r1 = MIN(r1, r2);
                arg.r2 = MAX(r1, r2);
                arg.c1 = MIN(c1, c2);
                arg.c2 = MAX(c1, c2);
            } else {
                fail(ps, CELL_ERR_REF);
            }
        } else {
            ps->p = save;
            arg.value = parse_expr(ps);
        }

        if (count < max)
            args[count] = arg;
        count++;

        skip_spaces(ps);
        if (*ps->p == ';' || *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (*ps->p == ')') {
            ps->p++;
            break;
        }
        fail(ps, CELL_ERR_SYNTAX);
        break;
    }
    return count;
}

static sheet_num call_function(struct parser *ps, const char *name)
{
    struct arg args[8];
    size_t count = parse_args(ps, args, ARRAY_LEN(args));

    if (count > ARRAY_LEN(args)) {
        fail(ps, CELL_ERR_SYNTAX);
        return 0;
    }

    struct fold_state st;

    memset(&st, 0, sizeof(st));
    st.first = true;

    if (name_is(name, "SUMME", "SUM")) {
        for (size_t i = 0; i < count; i++)
            fold_arg(ps, &args[i], fold_sum, &st);
        return st.sum;
    }

    if (name_is(name, "MITTELWERT", "AVERAGE")) {
        for (size_t i = 0; i < count; i++)
            fold_arg(ps, &args[i], fold_sum, &st);
        if (st.count == 0) {
            fail(ps, CELL_ERR_DIV0);
            return 0;
        }
        return div_round((__int128)st.sum, (__int128)st.count);
    }

    if (name_is(name, "ANZAHL", "COUNT")) {
        for (size_t i = 0; i < count; i++)
            fold_arg(ps, &args[i], fold_sum, &st);
        return (sheet_num)st.count * SHEET_SCALE;
    }

    if (name_is(name, "MIN", NULL)) {
        for (size_t i = 0; i < count; i++)
            fold_arg(ps, &args[i], fold_min, &st);
        return st.first ? 0 : st.best;
    }

    if (name_is(name, "MAX", NULL)) {
        for (size_t i = 0; i < count; i++)
            fold_arg(ps, &args[i], fold_max, &st);
        return st.first ? 0 : st.best;
    }

    /* Ab hier will jede Funktion einfache Werte, keine Bereiche. */
    for (size_t i = 0; i < count && i < ARRAY_LEN(args); i++) {
        if (args[i].is_range) {
            fail(ps, CELL_ERR_VALUE);
            return 0;
        }
    }

    if (name_is(name, "ABS", NULL)) {
        if (count != 1) {
            fail(ps, CELL_ERR_SYNTAX);
            return 0;
        }
        return args[0].value < 0 ? -args[0].value : args[0].value;
    }

    if (name_is(name, "WURZEL", "SQRT")) {
        if (count != 1) {
            fail(ps, CELL_ERR_SYNTAX);
            return 0;
        }
        if (args[0].value < 0) {
            fail(ps, CELL_ERR_VALUE);
            return 0;
        }
        return num_sqrt(args[0].value);
    }

    if (name_is(name, "RUNDEN", "ROUND")) {
        if (count < 1 || count > 2) {
            fail(ps, CELL_ERR_SYNTAX);
            return 0;
        }

        int64_t places = count == 2 ? args[1].value / SHEET_SCALE : 0;

        if (places < 0)
            places = 0;
        if (places > 4)
            places = 4;

        int64_t step = SHEET_SCALE;

        for (int i = 0; i < places; i++)
            step /= 10;

        return div_round((__int128)args[0].value, step) * step;
    }

    if (name_is(name, "WENN", "IF")) {
        if (count != 3) {
            fail(ps, CELL_ERR_SYNTAX);
            return 0;
        }
        return args[0].value != 0 ? args[1].value : args[2].value;
    }

    fail(ps, CELL_ERR_NAME);
    return 0;
}

static sheet_num parse_primary(struct parser *ps)
{
    skip_spaces(ps);

    if (*ps->p == '(') {
        ps->p++;
        sheet_num value = parse_expr(ps);

        skip_spaces(ps);
        if (*ps->p == ')')
            ps->p++;
        else
            fail(ps, CELL_ERR_SYNTAX);
        return value;
    }

    if ((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.' || *ps->p == ',') {
        char number[32];
        size_t n = 0;

        while (((*ps->p >= '0' && *ps->p <= '9') || *ps->p == '.' ||
                *ps->p == ',') && n + 1 < sizeof(number)) {
            /* Ein Komma trennt auch Argumente - als Dezimalzeichen
             * gilt es nur zwischen Ziffern. */
            if (*ps->p == ',' &&
                !(ps->p[1] >= '0' && ps->p[1] <= '9' && n > 0))
                break;
            number[n++] = *ps->p++;
        }
        number[n] = '\0';

        sheet_num value = 0;

        if (!sheet_parse_number(number, &value))
            fail(ps, CELL_ERR_SYNTAX);
        return value;
    }

    char name[16];

    if (read_name(ps, name, sizeof(name)) == 0) {
        fail(ps, CELL_ERR_SYNTAX);
        return 0;
    }

    skip_spaces(ps);
    if (*ps->p == '(') {
        ps->p++;
        return call_function(ps, name);
    }

    int row, col;

    if (sheet_parse_ref(name, &row, &col)) {
        /* Ein Bereich ergibt nur als Argument einer Funktion einen
         * Sinn - allein stehend hat er keinen Wert. Das muss vor dem
         * Nachschlagen auffallen, sonst wuerde eine Zelle, die in
         * ihrem eigenen Bereich liegt, als Kreisbezug gelten. */
        if (*ps->p == ':') {
            fail(ps, CELL_ERR_VALUE);
            return 0;
        }
        return cell_value(ps, row, col);
    }

    fail(ps, CELL_ERR_NAME);
    return 0;
}

static sheet_num parse_unary(struct parser *ps)
{
    skip_spaces(ps);

    if (*ps->p == '-') {
        ps->p++;
        return -parse_unary(ps);
    }
    if (*ps->p == '+') {
        ps->p++;
        return parse_unary(ps);
    }
    return parse_primary(ps);
}

static sheet_num parse_power(struct parser *ps)
{
    sheet_num base = parse_unary(ps);

    skip_spaces(ps);
    if (*ps->p != '^')
        return base;

    ps->p++;

    sheet_num exponent = parse_power(ps);   /* rechts vor links */

    if (exponent % SHEET_SCALE != 0) {
        /* Ohne Logarithmus geht nur der ganzzahlige Fall. */
        fail(ps, CELL_ERR_VALUE);
        return 0;
    }

    int64_t times = exponent / SHEET_SCALE;
    bool invert = times < 0;

    if (invert)
        times = -times;
    if (times > 64) {
        fail(ps, CELL_ERR_VALUE);
        return 0;
    }

    sheet_num result = SHEET_SCALE;

    for (int64_t i = 0; i < times; i++)
        result = num_mul(result, base);

    if (invert) {
        if (result == 0) {
            fail(ps, CELL_ERR_DIV0);
            return 0;
        }
        result = num_div(SHEET_SCALE, result);
    }
    return result;
}

static sheet_num parse_product(struct parser *ps)
{
    sheet_num value = parse_power(ps);

    for (;;) {
        skip_spaces(ps);

        char op = *ps->p;

        if (op != '*' && op != '/')
            return value;
        ps->p++;

        sheet_num right = parse_power(ps);

        if (op == '*') {
            value = num_mul(value, right);
        } else if (right == 0) {
            fail(ps, CELL_ERR_DIV0);
            return 0;
        } else {
            value = num_div(value, right);
        }
    }
}

static sheet_num parse_sum(struct parser *ps)
{
    sheet_num value = parse_product(ps);

    for (;;) {
        skip_spaces(ps);

        char op = *ps->p;

        if (op != '+' && op != '-')
            return value;
        ps->p++;

        sheet_num right = parse_product(ps);

        value = op == '+' ? value + right : value - right;
    }
}

static sheet_num parse_expr(struct parser *ps)
{
    sheet_num left = parse_sum(ps);

    skip_spaces(ps);

    const char *op = ps->p;
    int width = 0;

    if (op[0] == '<' && op[1] == '>') width = 2;
    else if (op[0] == '<' && op[1] == '=') width = 2;
    else if (op[0] == '>' && op[1] == '=') width = 2;
    else if (op[0] == '=' || op[0] == '<' || op[0] == '>') width = 1;

    if (width == 0)
        return left;

    ps->p += width;

    sheet_num right = parse_sum(ps);
    bool yes = false;

    if (width == 2 && op[1] == '>')      yes = left != right;
    else if (width == 2 && op[0] == '<') yes = left <= right;
    else if (width == 2)                 yes = left >= right;
    else if (op[0] == '=')               yes = left == right;
    else if (op[0] == '<')               yes = left < right;
    else                                 yes = left > right;

    return yes ? SHEET_SCALE : 0;
}

/* ------------------------------------------------------------------ */
/* Zellen                                                              */
/* ------------------------------------------------------------------ */

static sheet_num eval_cell(struct sheet *sh, int row, int col,
                           enum cell_error *error)
{
    struct cell *cell = &sh->cells[row][col];

    if (cell->kind != CELL_FORMULA) {
        if (error)
            *error = CELL_OK;
        return cell->kind == CELL_NUMBER ? cell->value : 0;
    }

    if (cell->state == STATE_DONE) {
        if (error)
            *error = cell->error;
        return cell->value;
    }
    if (cell->state == STATE_RUNNING) {
        /* Die Zelle braucht sich selbst. */
        cell->error = CELL_ERR_CYCLE;
        cell->value = 0;
        cell->state = STATE_DONE;
        if (error)
            *error = CELL_ERR_CYCLE;
        return 0;
    }

    cell->state = STATE_RUNNING;

    struct parser ps = { sh, cell->text + 1, CELL_OK };
    sheet_num value = parse_expr(&ps);

    skip_spaces(&ps);
    if (*ps.p)
        fail(&ps, CELL_ERR_SYNTAX);

    /* Ein Kreisbezug hat die Zelle schon fertiggeschrieben. */
    if (cell->state == STATE_DONE) {
        if (error)
            *error = cell->error;
        return cell->value;
    }

    cell->error = (uint8_t)ps.error;
    cell->value = ps.error == CELL_OK ? value : 0;
    cell->state = STATE_DONE;

    if (error)
        *error = ps.error;
    return cell->value;
}

void sheet_clear(struct sheet *sh)
{
    memset(sh, 0, sizeof(*sh));
}

void sheet_set(struct sheet *sh, int row, int col, const char *text)
{
    if (row < 0 || row >= SHEET_ROWS || col < 0 || col >= SHEET_COLS)
        return;

    struct cell *cell = &sh->cells[row][col];

    strlcpy(cell->text, text ? text : "", sizeof(cell->text));
    cell->error = CELL_OK;
    cell->value = 0;

    const char *p = cell->text;

    while (*p == ' ')
        p++;

    if (!*p)
        cell->kind = CELL_EMPTY;
    else if (*p == '=')
        cell->kind = CELL_FORMULA;
    else if (sheet_parse_number(cell->text, &cell->value))
        cell->kind = CELL_NUMBER;
    else
        cell->kind = CELL_TEXT;
}

void sheet_recalc(struct sheet *sh)
{
    for (int r = 0; r < SHEET_ROWS; r++) {
        for (int c = 0; c < SHEET_COLS; c++)
            sh->cells[r][c].state = STATE_FRESH;
    }

    for (int r = 0; r < SHEET_ROWS; r++) {
        for (int c = 0; c < SHEET_COLS; c++) {
            if (sh->cells[r][c].kind == CELL_FORMULA)
                eval_cell(sh, r, c, NULL);
        }
    }
}

const struct cell *sheet_cell(const struct sheet *sh, int row, int col)
{
    if (row < 0 || row >= SHEET_ROWS || col < 0 || col >= SHEET_COLS)
        return NULL;
    return &sh->cells[row][col];
}

void sheet_display(const struct sheet *sh, int row, int col,
                   char *out, size_t size)
{
    const struct cell *cell = sheet_cell(sh, row, col);

    if (!cell || cell->kind == CELL_EMPTY) {
        if (size)
            out[0] = '\0';
        return;
    }

    if (cell->kind == CELL_TEXT) {
        strlcpy(out, cell->text, size);
        return;
    }
    if (cell->kind == CELL_FORMULA && cell->error != CELL_OK) {
        strlcpy(out, sheet_error_text((enum cell_error)cell->error), size);
        return;
    }
    sheet_format_number(cell->value, out, size);
}

bool sheet_is_numeric(const struct sheet *sh, int row, int col)
{
    const struct cell *cell = sheet_cell(sh, row, col);

    return cell && (cell->kind == CELL_NUMBER || cell->kind == CELL_FORMULA);
}

size_t sheet_used(const struct sheet *sh)
{
    size_t n = 0;

    for (int r = 0; r < SHEET_ROWS; r++) {
        for (int c = 0; c < SHEET_COLS; c++) {
            if (sh->cells[r][c].kind != CELL_EMPTY)
                n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Dateien                                                             */
/* ------------------------------------------------------------------ */

/* Bis wohin die Tabelle wirklich reicht - alles dahinter ist leer und
 * muss nicht geschrieben werden. */
static void extent(const struct sheet *sh, int *rows, int *cols)
{
    *rows = 0;
    *cols = 0;

    for (int r = 0; r < SHEET_ROWS; r++) {
        for (int c = 0; c < SHEET_COLS; c++) {
            if (sh->cells[r][c].kind == CELL_EMPTY)
                continue;
            if (r + 1 > *rows)
                *rows = r + 1;
            if (c + 1 > *cols)
                *cols = c + 1;
        }
    }
}

static size_t put(char *out, size_t size, size_t at, const char *text)
{
    while (*text && at + 1 < size)
        out[at++] = *text++;
    return at;
}

size_t sheet_to_csv(const struct sheet *sh, char *out, size_t size)
{
    int rows, cols;
    size_t at = 0;

    extent(sh, &rows, &cols);

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            const char *text = sh->cells[r][c].text;
            bool quote = false;

            for (const char *p = text; *p; p++) {
                if (*p == ';' || *p == '"' || *p == '\n')
                    quote = true;
            }

            if (quote) {
                at = put(out, size, at, "\"");
                for (const char *p = text; *p && at + 1 < size; p++) {
                    if (*p == '"')
                        at = put(out, size, at, "\"");
                    out[at++] = *p;
                }
                at = put(out, size, at, "\"");
            } else {
                at = put(out, size, at, text);
            }

            if (c + 1 < cols)
                at = put(out, size, at, ";");
        }
        at = put(out, size, at, "\n");
    }

    if (size)
        out[MIN(at, size - 1)] = '\0';
    return at;
}

void sheet_from_csv(struct sheet *sh, const char *text, size_t length)
{
    sheet_clear(sh);

    int row = 0, col = 0;
    size_t i = 0;

    while (i < length && row < SHEET_ROWS) {
        char field[SHEET_TEXT_MAX];
        size_t n = 0;

        if (text[i] == '"') {
            i++;
            while (i < length) {
                if (text[i] == '"') {
                    if (i + 1 < length && text[i + 1] == '"') {
                        if (n + 1 < sizeof(field))
                            field[n++] = '"';
                        i += 2;
                        continue;
                    }
                    i++;
                    break;
                }
                if (n + 1 < sizeof(field))
                    field[n++] = text[i];
                i++;
            }
        } else {
            while (i < length && text[i] != ';' && text[i] != '\n' &&
                   text[i] != '\r') {
                if (n + 1 < sizeof(field))
                    field[n++] = text[i];
                i++;
            }
        }
        field[n] = '\0';

        if (col < SHEET_COLS && n > 0)
            sheet_set(sh, row, col, field);

        if (i < length && text[i] == ';') {
            col++;
            i++;
            continue;
        }

        /* Zeilenende - auch das der anderen Sorte. */
        if (i < length && text[i] == '\r')
            i++;
        if (i < length && text[i] == '\n')
            i++;
        row++;
        col = 0;
    }

    sheet_recalc(sh);
}
