/* bignum.c - Rechnen mit grossen Zahlen.
 *
 * Fuer RSA wird nur eines gebraucht: eine Zahl mit einem kleinen Exponenten
 * potenzieren, modulo einem grossen Modul. Der uebliche Weg dafuer ist die
 * Montgomery-Multiplikation. Ihr Trick besteht darin, alle Zahlen mit einem
 * Faktor R = 2^(64*k) zu multiplizieren; in dieser Darstellung laesst sich
 * die Reduktion modulo n durch Verschieben erledigen, und man braucht
 * keine einzige Division.
 */

#include "bignum.h"
#include "kstring.h"

void bn_zero(struct bignum *a)
{
    memset(a->limb, 0, sizeof(a->limb));
    a->used = 0;
}

static void bn_trim(struct bignum *a)
{
    a->used = BN_MAX_LIMBS;
    while (a->used > 0 && a->limb[a->used - 1] == 0)
        a->used--;
}

bool bn_from_bytes(struct bignum *a, const uint8_t *data, size_t length)
{
    /* Fuehrende Nullen ueberspringen. */
    while (length > 0 && data[0] == 0) {
        data++;
        length--;
    }

    if (length > BN_MAX_BYTES)
        return false;

    bn_zero(a);

    /* Die Bytes kommen mit dem hoechstwertigen zuerst. */
    for (size_t i = 0; i < length; i++) {
        size_t position = length - 1 - i;

        a->limb[i / 8] |= (uint64_t)data[position] << ((i % 8) * 8);
    }

    bn_trim(a);
    return true;
}

void bn_to_bytes(const struct bignum *a, uint8_t *out, size_t length)
{
    memset(out, 0, length);

    for (size_t i = 0; i < length; i++) {
        size_t position = length - 1 - i;
        size_t limb = i / 8;

        if (limb < BN_MAX_LIMBS)
            out[position] = (uint8_t)(a->limb[limb] >> ((i % 8) * 8));
    }
}

int bn_compare(const struct bignum *a, const struct bignum *b)
{
    for (size_t i = BN_MAX_LIMBS; i > 0; i--) {
        uint64_t x = a->limb[i - 1];
        uint64_t y = b->limb[i - 1];

        if (x != y)
            return x > y ? 1 : -1;
    }
    return 0;
}

bool bn_is_zero(const struct bignum *a)
{
    for (size_t i = 0; i < BN_MAX_LIMBS; i++) {
        if (a->limb[i])
            return false;
    }
    return true;
}

size_t bn_bits(const struct bignum *a)
{
    for (size_t i = BN_MAX_LIMBS; i > 0; i--) {
        if (a->limb[i - 1] == 0)
            continue;

        size_t bits = (i - 1) * 64;
        uint64_t word = a->limb[i - 1];

        while (word) {
            bits++;
            word >>= 1;
        }
        return bits;
    }
    return 0;
}

/* out = a - b, unter der Annahme a >= b; liefert den Uebertrag. */
static uint64_t bn_sub_raw(uint64_t *out, const uint64_t *a, const uint64_t *b,
                           size_t limbs)
{
    uint64_t borrow = 0;

    for (size_t i = 0; i < limbs; i++) {
        uint64_t left = a[i];
        uint64_t right = b[i];
        uint64_t diff = left - right - borrow;

        borrow = (left < right + borrow) || (right + borrow < right);
        out[i] = diff;
    }
    return borrow;
}

/* Der Kehrwert von -n modulo 2^64, mit dem Newton-Verfahren. */
static uint64_t mont_n0inv(uint64_t n0)
{
    uint64_t inverse = 1;

    for (int i = 0; i < 6; i++)             /* verdoppelt jedesmal die Bits */
        inverse *= 2 - n0 * inverse;

    return 0 - inverse;
}

/* Montgomery-Multiplikation nach dem CIOS-Verfahren:
 * out = a * b * R^-1 mod n. */
static void mont_mul_raw(uint64_t *out, const uint64_t *a, const uint64_t *b,
                         const uint64_t *n, size_t limbs, uint64_t n0inv)
{
    uint64_t t[BN_MAX_LIMBS + 2];

    memset(t, 0, sizeof(uint64_t) * (limbs + 2));

    for (size_t i = 0; i < limbs; i++) {
        /* t = t + a[i] * b */
        unsigned __int128 carry = 0;

        for (size_t j = 0; j < limbs; j++) {
            unsigned __int128 sum = (unsigned __int128)a[i] * b[j] + t[j] + carry;

            t[j] = (uint64_t)sum;
            carry = sum >> 64;
        }
        unsigned __int128 sum = (unsigned __int128)t[limbs] + carry;
        t[limbs] = (uint64_t)sum;
        t[limbs + 1] = (uint64_t)(sum >> 64);

        /* m so waehlen, dass die unterste Stelle verschwindet */
        uint64_t m = t[0] * n0inv;

        carry = (unsigned __int128)m * n[0] + t[0];
        carry >>= 64;

        for (size_t j = 1; j < limbs; j++) {
            unsigned __int128 s = (unsigned __int128)m * n[j] + t[j] + carry;

            t[j - 1] = (uint64_t)s;
            carry = s >> 64;
        }
        unsigned __int128 s = (unsigned __int128)t[limbs] + carry;
        t[limbs - 1] = (uint64_t)s;
        t[limbs] = t[limbs + 1] + (uint64_t)(s >> 64);
    }

    /* Zum Schluss noch einmal reduzieren, falls t >= n. */
    uint64_t scratch[BN_MAX_LIMBS];
    uint64_t borrow = bn_sub_raw(scratch, t, n, limbs);

    if (t[limbs] != 0 || borrow == 0)
        memcpy(out, scratch, sizeof(uint64_t) * limbs);
    else
        memcpy(out, t, sizeof(uint64_t) * limbs);
}

/* Verdoppelt a modulo n. */
static void bn_double_mod(uint64_t *a, const uint64_t *n, size_t limbs)
{
    uint64_t carry = 0;

    for (size_t i = 0; i < limbs; i++) {
        uint64_t next = a[i] >> 63;

        a[i] = (a[i] << 1) | carry;
        carry = next;
    }

    uint64_t scratch[BN_MAX_LIMBS];
    uint64_t borrow = bn_sub_raw(scratch, a, n, limbs);

    if (carry || borrow == 0)
        memcpy(a, scratch, sizeof(uint64_t) * limbs);
}

bool bn_modexp(struct bignum *out, const struct bignum *base,
               uint64_t exponent, const struct bignum *modulus)
{
    size_t limbs = modulus->used;

    if (limbs == 0 || (modulus->limb[0] & 1) == 0)
        return false;                      /* Modul muss ungerade sein */
    if (limbs > BN_MAX_LIMBS)
        return false;

    uint64_t n0inv = mont_n0inv(modulus->limb[0]);

    /* R^2 mod n, gewonnen durch fortgesetztes Verdoppeln von 1. */
    uint64_t r2[BN_MAX_LIMBS];

    memset(r2, 0, sizeof(uint64_t) * limbs);
    r2[0] = 1;
    for (size_t i = 0; i < 2 * limbs * 64; i++)
        bn_double_mod(r2, modulus->limb, limbs);

    /* Die Grundzahl in den Montgomery-Bereich holen. */
    uint64_t x[BN_MAX_LIMBS], result[BN_MAX_LIMBS], one[BN_MAX_LIMBS];

    memcpy(x, base->limb, sizeof(uint64_t) * limbs);
    mont_mul_raw(x, x, r2, modulus->limb, limbs, n0inv);

    /* Ergebnis mit 1 im Montgomery-Bereich vorbelegen (= R mod n). */
    memset(one, 0, sizeof(uint64_t) * limbs);
    one[0] = 1;
    memcpy(result, one, sizeof(uint64_t) * limbs);
    mont_mul_raw(result, result, r2, modulus->limb, limbs, n0inv);

    /* Quadrieren und Multiplizieren, vom hoechsten Bit des Exponenten an. */
    int highest = 63;
    while (highest > 0 && !((exponent >> highest) & 1))
        highest--;

    for (int bit = highest; bit >= 0; bit--) {
        mont_mul_raw(result, result, result, modulus->limb, limbs, n0inv);
        if ((exponent >> bit) & 1)
            mont_mul_raw(result, result, x, modulus->limb, limbs, n0inv);
    }

    /* Zurueck aus dem Montgomery-Bereich: mit 1 multiplizieren. */
    mont_mul_raw(result, result, one, modulus->limb, limbs, n0inv);

    bn_zero(out);
    memcpy(out->limb, result, sizeof(uint64_t) * limbs);
    bn_trim(out);
    return true;
}

/* --- Montgomery-Bereich zum Weiterverwenden -------------------------- */

bool mont_init(struct mont *m, const uint64_t *modulus, size_t limbs)
{
    if (limbs == 0 || limbs > BN_MAX_LIMBS || (modulus[0] & 1) == 0)
        return false;

    memset(m, 0, sizeof(*m));
    memcpy(m->modulus, modulus, sizeof(uint64_t) * limbs);
    m->limbs = limbs;
    m->n0inv = mont_n0inv(modulus[0]);

    /* R^2 mod n durch fortgesetztes Verdoppeln. */
    m->r2[0] = 1;
    for (size_t i = 0; i < 2 * limbs * 64; i++)
        bn_double_mod(m->r2, m->modulus, limbs);

    /* Die Eins im Bereich ist R mod n. */
    uint64_t one[BN_MAX_LIMBS];

    memset(one, 0, sizeof(uint64_t) * limbs);
    one[0] = 1;
    mont_mul_raw(m->one, one, m->r2, m->modulus, limbs, m->n0inv);

    return true;
}

void mont_mul(const struct mont *m, uint64_t *out, const uint64_t *a,
              const uint64_t *b)
{
    mont_mul_raw(out, a, b, m->modulus, m->limbs, m->n0inv);
}

void mont_enter(const struct mont *m, uint64_t *out, const uint64_t *a)
{
    mont_mul_raw(out, a, m->r2, m->modulus, m->limbs, m->n0inv);
}

void mont_leave(const struct mont *m, uint64_t *out, const uint64_t *a)
{
    uint64_t one[BN_MAX_LIMBS];

    memset(one, 0, sizeof(uint64_t) * m->limbs);
    one[0] = 1;
    mont_mul_raw(out, a, one, m->modulus, m->limbs, m->n0inv);
}

void mont_add(const struct mont *m, uint64_t *out, const uint64_t *a,
              const uint64_t *b)
{
    uint64_t carry = 0;

    for (size_t i = 0; i < m->limbs; i++) {
        unsigned __int128 sum = (unsigned __int128)a[i] + b[i] + carry;

        out[i] = (uint64_t)sum;
        carry = (uint64_t)(sum >> 64);
    }

    uint64_t scratch[BN_MAX_LIMBS];
    uint64_t borrow = bn_sub_raw(scratch, out, m->modulus, m->limbs);

    if (carry || borrow == 0)
        memcpy(out, scratch, sizeof(uint64_t) * m->limbs);
}

void mont_sub(const struct mont *m, uint64_t *out, const uint64_t *a,
              const uint64_t *b)
{
    uint64_t scratch[BN_MAX_LIMBS];
    uint64_t borrow = bn_sub_raw(scratch, a, b, m->limbs);

    if (borrow) {
        /* Unter Null gerutscht - das Modul wieder addieren. */
        uint64_t carry = 0;

        for (size_t i = 0; i < m->limbs; i++) {
            unsigned __int128 sum =
                (unsigned __int128)scratch[i] + m->modulus[i] + carry;

            scratch[i] = (uint64_t)sum;
            carry = (uint64_t)(sum >> 64);
        }
    }
    memcpy(out, scratch, sizeof(uint64_t) * m->limbs);
}

bool mont_is_zero(const struct mont *m, const uint64_t *a)
{
    uint64_t bits = 0;

    for (size_t i = 0; i < m->limbs; i++)
        bits |= a[i];
    return bits == 0;
}

void mont_exp(const struct mont *m, uint64_t *out, const uint64_t *a,
              const uint8_t *exponent, size_t exponent_length)
{
    uint64_t result[BN_MAX_LIMBS];
    uint64_t base[BN_MAX_LIMBS];

    memcpy(result, m->one, sizeof(uint64_t) * m->limbs);
    memcpy(base, a, sizeof(uint64_t) * m->limbs);

    /* Vom hoechsten Bit an: quadrieren, bei gesetztem Bit multiplizieren. */
    bool started = false;

    for (size_t i = 0; i < exponent_length; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            if (started)
                mont_mul(m, result, result, result);

            if ((exponent[i] >> bit) & 1) {
                if (started)
                    mont_mul(m, result, result, base);
                else
                    memcpy(result, base, sizeof(uint64_t) * m->limbs);
                started = true;
            }
        }
    }

    memcpy(out, result, sizeof(uint64_t) * m->limbs);
}
