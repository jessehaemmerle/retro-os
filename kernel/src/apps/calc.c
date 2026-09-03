/* calc.c - siehe calc.h. */

#include "calc.h"
#include "kstring.h"

typedef signed __int128 wide_t;

void calc_reset(struct calc *c)
{
    memset(c, 0, sizeof(*c));
}

void calc_clear_entry(struct calc *c)
{
    c->entry = 0;
    c->decimals = 0;
    c->point = false;
    c->typing = true;
    c->error = false;
}

int64_t calc_value(const struct calc *c)
{
    return c->typing ? c->entry : c->acc;
}

/* Alles, was gerechnet wird, laeuft hier durch - und alles, was zu
 * gross wird, endet hier als Fehler statt als stillem Ueberlauf. */
static bool fits(wide_t value, int64_t *out)
{
    if (value > (wide_t)CALC_MAX * CALC_SCALE ||
        value < -(wide_t)CALC_MAX * CALC_SCALE)
        return false;
    *out = (int64_t)value;
    return true;
}

/* Setzt den Fehler samt Grund - und nur den ersten, denn der spaetere
 * ist meist eine Folge des frueheren. */
static void fault(struct calc *c, enum calc_fault why)
{
    if (c->error)
        return;
    c->error = true;
    c->fault = (uint8_t)why;
}

static bool apply(char op, int64_t a, int64_t b, int64_t *out)
{
    switch (op) {
    case '+':
        return fits((wide_t)a + b, out);
    case '-':
        return fits((wide_t)a - b, out);
    case '*':
        /* Zwei Festkommazahlen multipliziert ergeben eine mit zwei
         * Nachkommateilen - einer muss wieder heraus. Gerundet wird
         * kaufmaennisch, sonst verschwindet bei jeder Multiplikation
         * ein halbes Millionstel. */
        {
            wide_t product = (wide_t)a * b;
            wide_t half = CALC_SCALE / 2;

            product = product >= 0 ? (product + half) / CALC_SCALE
                                   : (product - half) / CALC_SCALE;
            return fits(product, out);
        }
    case '/':
        if (b == 0)
            return false;   /* der Aufrufer meldet CALC_DIV0 */
        {
            wide_t numerator = (wide_t)a * CALC_SCALE;
            wide_t half = b / 2;

            /* Beim Runden muss das Vorzeichen des Nenners mit hinein,
             * sonst rundet ein negativer Teiler in die falsche
             * Richtung. */
            if ((numerator >= 0) == (b > 0))
                numerator += half;
            else
                numerator -= half;
            return fits(numerator / b, out);
        }
    default:
        *out = b;
        return true;
    }
}

void calc_digit(struct calc *c, int digit)
{
    if (c->error || digit < 0 || digit > 9)
        return;

    if (!c->typing) {
        c->entry = 0;
        c->decimals = 0;
        c->point = false;
        c->typing = true;
    }

    if (c->point) {
        /* Hinter dem Komma ist nach sechs Stellen Schluss - weitere
         * wuerden im Festkomma ohnehin verschwinden. */
        if (c->decimals >= 6)
            return;

        int64_t place = CALC_SCALE;

        for (int i = 0; i <= c->decimals; i++)
            place /= 10;

        c->entry += (c->entry < 0 ? -1 : 1) * (int64_t)digit * place;
        c->decimals++;
        return;
    }

    wide_t next = (wide_t)c->entry * 10;

    next += (wide_t)(c->entry < 0 ? -digit : digit) * CALC_SCALE;

    int64_t value;

    if (!fits(next, &value))
        return;
    c->entry = value;
}

void calc_point(struct calc *c)
{
    if (c->error)
        return;
    if (!c->typing) {
        c->entry = 0;
        c->typing = true;
        c->decimals = 0;
    }
    c->point = true;
}

void calc_backspace(struct calc *c)
{
    if (c->error || !c->typing)
        return;

    if (c->decimals > 0) {
        int64_t place = CALC_SCALE;

        for (int i = 0; i < c->decimals; i++)
            place /= 10;

        c->entry -= c->entry % (place * 10);
        c->decimals--;
        if (c->decimals == 0)
            c->point = true;
        return;
    }

    if (c->point) {
        c->point = false;
        return;
    }

    c->entry = (c->entry / CALC_SCALE / 10) * CALC_SCALE;
}

void calc_sign(struct calc *c)
{
    if (c->error)
        return;
    if (c->typing)
        c->entry = -c->entry;
    else
        c->acc = -c->acc;
}

void calc_op(struct calc *c, char op)
{
    if (c->error)
        return;

    if (c->typing) {
        /* Steht schon eine Rechnung an, wird sie jetzt faellig - das
         * ist die Kette 2 + 3 + 4, bei der nach jedem Zeichen ein
         * Zwischenergebnis dasteht. */
        int64_t result;

        if (!apply(c->op, c->acc, c->entry, &result)) {
            fault(c, c->op == '/' && c->entry == 0 ? CALC_DIV0 : CALC_RANGE);
            return;
        }
        c->acc = result;
    }

    c->op = op;
    c->typing = false;
    c->point = false;
    c->decimals = 0;
}

void calc_equals(struct calc *c)
{
    if (c->error)
        return;

    int64_t result;

    int64_t right = c->typing ? c->entry : c->acc;

    if (!apply(c->op, c->acc, right, &result)) {
        fault(c, c->op == '/' && right == 0 ? CALC_DIV0 : CALC_RANGE);
        return;
    }

    c->acc = result;
    c->op = 0;
    c->typing = false;
    c->point = false;
    c->decimals = 0;
}

void calc_percent(struct calc *c)
{
    if (c->error || !c->typing)
        return;

    int64_t hundredth = c->entry;

    /* Bei "+" und "-" ist ein Prozent ein Hundertstel dessen, was
     * davorsteht: "200 + 10 %" sind 220. Bei "*" und "/" - und ohne
     * alles - ist es schlicht ein Hundertstel: "200 * 10 %" sind 20.
     * Beides ist bei Taschenrechnern so ueblich, und beides
     * ueberrascht, wenn man es andersherum macht. */
    if (c->op == '+' || c->op == '-') {
        if (!apply('*', c->entry, c->acc, &hundredth)) {
            fault(c, CALC_RANGE);
            return;
        }
    }

    if (!apply('/', hundredth, CALC_SCALE * 100, &hundredth)) {
        fault(c, CALC_RANGE);
        return;
    }
    c->entry = hundredth;
    c->decimals = 6;
    c->point = false;
}

void calc_sqrt(struct calc *c)
{
    int64_t value = calc_value(c);

    if (c->error)
        return;
    if (value < 0) {
        fault(c, CALC_DOMAIN);
        return;
    }

    /* Ganzzahliges Heron-Verfahren auf der skalierten Zahl: Gesucht
     * ist die Wurzel aus value*SCALE, denn sqrt(v/S)*S = sqrt(v*S).
     * Das Verfahren liefert die abgerundete Wurzel; gerundet wird
     * danach von Hand, sonst gaebe die Wurzel aus zwei 1,414213 statt
     * 1,414214. */
    wide_t target = (wide_t)value * CALC_SCALE;
    wide_t guess = 0;

    if (target > 0) {
        wide_t x = target;
        wide_t y = (x + 1) / 2;

        while (y < x) {
            x = y;
            y = (x + target / x) / 2;
        }
        guess = x;

        /* Naeher an der naechsten ganzen Zahl? Dann die nehmen. */
        if (target - guess * guess > guess)
            guess++;
    }

    int64_t result;

    if (!fits(guess, &result)) {
        fault(c, CALC_RANGE);
        return;
    }

    c->entry = result;
    c->typing = true;
    c->decimals = 6;
    c->point = false;
}

void calc_format(int64_t value, char *out, size_t size)
{
    if (!out || !size)
        return;

    bool negative = value < 0;
    uint64_t magnitude = (uint64_t)(negative ? -value : value);
    uint64_t whole = magnitude / CALC_SCALE;
    uint64_t fraction = magnitude % CALC_SCALE;

    char digits[8];
    size_t at = 0;

    /* Nullen am Ende der Nachkommastellen weglassen: 2,5 und nicht
     * 2,500000. */
    while (fraction > 0 && at < sizeof(digits) - 1) {
        uint64_t place = CALC_SCALE / 10;

        digits[at++] = (char)('0' + (fraction / place));
        fraction %= place;
        if (fraction == 0)
            break;
        fraction *= 10;
    }
    digits[at] = '\0';

    if (at == 0)
        ksnprintf(out, size, "%s%llu", negative ? "-" : "",
                  (unsigned long long)whole);
    else
        ksnprintf(out, size, "%s%llu,%s", negative ? "-" : "",
                  (unsigned long long)whole, digits);
}

void calc_display(const struct calc *c, char *out, size_t size)
{
    if (c->error) {
        switch (c->fault) {
        case CALC_DIV0:   strlcpy(out, "Nicht durch null", size); break;
        case CALC_DOMAIN: strlcpy(out, "Keine Wurzel", size);     break;
        default:          strlcpy(out, "Zu gross", size);         break;
        }
        return;
    }

    calc_format(calc_value(c), out, size);

    /* Ein getipptes Komma ohne Ziffern dahinter soll man sehen -
     * sonst wirkt die Taste, als haette sie nichts getan. */
    if (c->typing && c->point && c->decimals == 0) {
        size_t len = strlen(out);

        if (len + 2 <= size) {
            out[len] = ',';
            out[len + 1] = '\0';
        }
    }
}
