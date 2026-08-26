/* p256.c - Unterschriften mit ECDSA auf der Kurve NIST P-256 pruefen.
 *
 * Die Kurve ist y^2 = x^3 - 3x + b ueber einem Koerper mit knapp 2^256
 * Elementen. Gerechnet wird in Jacobi-Koordinaten (X, Y, Z), die fuer den
 * eigentlichen Punkt (X/Z^2, Y/Z^3) stehen - so wird die teure Division
 * erst ganz am Ende einmal noetig statt bei jeder Addition.
 *
 * Beim Pruefen einer Unterschrift sind alle beteiligten Zahlen oeffentlich.
 * Deshalb darf hier ausnahmsweise datenabhaengig verzweigt werden; ein
 * Lauschangriff auf die Zeit gewinnt nichts, was nicht ohnehin bekannt ist.
 */

#include "pki.h"
#include "bignum.h"
#include "crypto.h"
#include "kstring.h"

#define P256_LIMBS 4

/* Die Kurvenparameter, jeweils als vier 64-Bit-Woerter, niederwertig zuerst. */
static const uint64_t p256_p[P256_LIMBS] = {
    0xffffffffffffffffULL, 0x00000000ffffffffULL,
    0x0000000000000000ULL, 0xffffffff00000001ULL,
};
static const uint64_t p256_n[P256_LIMBS] = {
    0xf3b9cac2fc632551ULL, 0xbce6faada7179e84ULL,
    0xffffffffffffffffULL, 0xffffffff00000000ULL,
};
static const uint64_t p256_b[P256_LIMBS] = {
    0x3bce3c3e27d2604bULL, 0x651d06b0cc53b0f6ULL,
    0xb3ebbd55769886bcULL, 0x5ac635d8aa3a93e7ULL,
};
static const uint64_t p256_gx[P256_LIMBS] = {
    0xf4a13945d898c296ULL, 0x77037d812deb33a0ULL,
    0xf8bce6e563a440f2ULL, 0x6b17d1f2e12c4247ULL,
};
static const uint64_t p256_gy[P256_LIMBS] = {
    0xcbb6406837bf51f5ULL, 0x2bce33576b315eceULL,
    0x8ee7eb4a7c0f9e16ULL, 0x4fe342e2fe1a7f9bULL,
};

struct point {
    uint64_t x[P256_LIMBS];
    uint64_t y[P256_LIMBS];
    uint64_t z[P256_LIMBS];      /* z = 0 bedeutet: der unendlich ferne Punkt */
};

static struct mont field;        /* Rechnen modulo p */
static struct mont order;        /* Rechnen modulo n */
static uint64_t   mont_b[P256_LIMBS];
static struct point generator;
static bool       ready;

static void fe_copy(uint64_t *out, const uint64_t *in)
{
    memcpy(out, in, sizeof(uint64_t) * P256_LIMBS);
}

static bool fe_is_zero(const uint64_t *a)
{
    return mont_is_zero(&field, a);
}

static void p256_setup(void)
{
    if (ready)
        return;

    mont_init(&field, p256_p, P256_LIMBS);
    mont_init(&order, p256_n, P256_LIMBS);
    mont_enter(&field, mont_b, p256_b);

    mont_enter(&field, generator.x, p256_gx);
    mont_enter(&field, generator.y, p256_gy);
    fe_copy(generator.z, field.one);

    ready = true;
}

/* Verdoppelt einen Punkt. Nutzt aus, dass a = -3 ist. */
static void point_double(struct point *out, const struct point *a)
{
    uint64_t delta[P256_LIMBS], gamma[P256_LIMBS], beta[P256_LIMBS];
    uint64_t alpha[P256_LIMBS], t1[P256_LIMBS], t2[P256_LIMBS];

    if (fe_is_zero(a->z)) {
        memset(out, 0, sizeof(*out));
        return;
    }

    mont_mul(&field, delta, a->z, a->z);        /* delta = Z^2        */
    mont_mul(&field, gamma, a->y, a->y);        /* gamma = Y^2        */
    mont_mul(&field, beta, a->x, gamma);        /* beta  = X * gamma  */

    mont_sub(&field, t1, a->x, delta);
    mont_add(&field, t2, a->x, delta);
    mont_mul(&field, alpha, t1, t2);
    mont_add(&field, t1, alpha, alpha);
    mont_add(&field, alpha, t1, alpha);         /* alpha = 3(X-d)(X+d) */

    /* X' = alpha^2 - 8*beta */
    mont_mul(&field, t1, alpha, alpha);
    mont_add(&field, t2, beta, beta);
    mont_add(&field, t2, t2, t2);
    mont_add(&field, t2, t2, t2);               /* t2 = 8*beta        */
    mont_sub(&field, out->x, t1, t2);

    /* Z' = (Y+Z)^2 - gamma - delta */
    mont_add(&field, t1, a->y, a->z);
    mont_mul(&field, t1, t1, t1);
    mont_sub(&field, t1, t1, gamma);
    mont_sub(&field, out->z, t1, delta);

    /* Y' = alpha*(4*beta - X') - 8*gamma^2 */
    mont_add(&field, t1, beta, beta);
    mont_add(&field, t1, t1, t1);               /* t1 = 4*beta        */
    mont_sub(&field, t1, t1, out->x);
    mont_mul(&field, t1, alpha, t1);

    mont_mul(&field, t2, gamma, gamma);
    mont_add(&field, t2, t2, t2);
    mont_add(&field, t2, t2, t2);
    mont_add(&field, t2, t2, t2);               /* t2 = 8*gamma^2     */
    mont_sub(&field, out->y, t1, t2);
}

static void point_add(struct point *out, const struct point *a,
                      const struct point *b)
{
    uint64_t z1z1[P256_LIMBS], z2z2[P256_LIMBS];
    uint64_t u1[P256_LIMBS], u2[P256_LIMBS];
    uint64_t s1[P256_LIMBS], s2[P256_LIMBS];
    uint64_t h[P256_LIMBS], r[P256_LIMBS];
    uint64_t hh[P256_LIMBS], hhh[P256_LIMBS], v[P256_LIMBS];
    uint64_t t1[P256_LIMBS], t2[P256_LIMBS];

    if (fe_is_zero(a->z)) {
        *out = *b;
        return;
    }
    if (fe_is_zero(b->z)) {
        *out = *a;
        return;
    }

    mont_mul(&field, z1z1, a->z, a->z);
    mont_mul(&field, z2z2, b->z, b->z);
    mont_mul(&field, u1, a->x, z2z2);
    mont_mul(&field, u2, b->x, z1z1);

    mont_mul(&field, t1, b->z, z2z2);
    mont_mul(&field, s1, a->y, t1);
    mont_mul(&field, t2, a->z, z1z1);
    mont_mul(&field, s2, b->y, t2);

    mont_sub(&field, h, u2, u1);
    mont_sub(&field, r, s2, s1);

    if (fe_is_zero(h)) {
        if (fe_is_zero(r)) {
            point_double(out, a);
            return;
        }
        memset(out, 0, sizeof(*out));    /* entgegengesetzte Punkte */
        return;
    }

    mont_mul(&field, hh, h, h);
    mont_mul(&field, hhh, hh, h);
    mont_mul(&field, v, u1, hh);

    /* X' = r^2 - hhh - 2v */
    mont_mul(&field, t1, r, r);
    mont_sub(&field, t1, t1, hhh);
    mont_add(&field, t2, v, v);
    mont_sub(&field, out->x, t1, t2);

    /* Y' = r*(v - X') - s1*hhh */
    mont_sub(&field, t1, v, out->x);
    mont_mul(&field, t1, r, t1);
    mont_mul(&field, t2, s1, hhh);
    mont_sub(&field, out->y, t1, t2);

    /* Z' = Z1 * Z2 * h */
    mont_mul(&field, t1, a->z, b->z);
    mont_mul(&field, out->z, t1, h);
}

/* Berechnet scalar * point, von oben nach unten. */
static void point_multiply(struct point *out, const uint64_t *scalar,
                           const struct point *point)
{
    struct point result;

    memset(&result, 0, sizeof(result));

    bool started = false;

    for (int limb = P256_LIMBS - 1; limb >= 0; limb--) {
        for (int bit = 63; bit >= 0; bit--) {
            if (started)
                point_double(&result, &result);

            if ((scalar[limb] >> bit) & 1) {
                if (started)
                    point_add(&result, &result, point);
                else
                    result = *point;
                started = true;
            }
        }
    }

    *out = result;
}

/* Kehrwert modulo n ueber den kleinen Satz von Fermat: a^(n-2). */
static void order_invert(uint64_t *out, const uint64_t *a)
{
    static const uint8_t n_minus_2[32] = {
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xbc, 0xe6, 0xfa, 0xad, 0xa7, 0x17, 0x9e, 0x84,
        0xf3, 0xb9, 0xca, 0xc2, 0xfc, 0x63, 0x25, 0x4f,
    };
    uint64_t base[P256_LIMBS];

    mont_enter(&order, base, a);
    mont_exp(&order, base, base, n_minus_2, sizeof(n_minus_2));
    mont_leave(&order, out, base);
}

static void order_mul(uint64_t *out, const uint64_t *a, const uint64_t *b)
{
    uint64_t ma[P256_LIMBS], mb[P256_LIMBS];

    mont_enter(&order, ma, a);
    mont_enter(&order, mb, b);
    mont_mul(&order, ma, ma, mb);
    mont_leave(&order, out, ma);
}

static bool load_be(uint64_t *out, const uint8_t *data, size_t length)
{
    if (length > P256_LIMBS * 8)
        return false;

    memset(out, 0, sizeof(uint64_t) * P256_LIMBS);
    for (size_t i = 0; i < length; i++) {
        size_t position = length - 1 - i;

        out[i / 8] |= (uint64_t)data[position] << ((i % 8) * 8);
    }
    return true;
}

static bool less_than(const uint64_t *a, const uint64_t *b)
{
    for (int i = P256_LIMBS - 1; i >= 0; i--) {
        if (a[i] != b[i])
            return a[i] < b[i];
    }
    return false;
}

/* Liest r und s aus der DER-Form der Unterschrift. */
static bool parse_signature(const uint8_t *signature, size_t length,
                            uint64_t *r, uint64_t *s)
{
    struct der reader, inner;
    struct der_value sequence, value;

    der_start(&reader, signature, length);
    if (!der_next(&reader, &sequence) || sequence.tag != 0x30)
        return false;
    if (!der_enter(&sequence, &inner))
        return false;

    if (!der_next(&inner, &value) || value.tag != 0x02)
        return false;

    const uint8_t *data = value.content;
    size_t data_length = value.length;

    while (data_length > 1 && data[0] == 0) {
        data++;
        data_length--;
    }
    if (!load_be(r, data, data_length))
        return false;

    if (!der_next(&inner, &value) || value.tag != 0x02)
        return false;

    data = value.content;
    data_length = value.length;
    while (data_length > 1 && data[0] == 0) {
        data++;
        data_length--;
    }
    return load_be(s, data, data_length);
}

bool ecdsa_p256_verify(const uint8_t point[65],
                       const uint8_t *hash, size_t hash_length,
                       const uint8_t *signature, size_t signature_length)
{
    uint64_t r[P256_LIMBS], s[P256_LIMBS], e[P256_LIMBS];
    uint64_t w[P256_LIMBS], u1[P256_LIMBS], u2[P256_LIMBS];
    struct point q, a, b, result;

    p256_setup();

    if (point[0] != 0x04)
        return false;                     /* nur unkomprimierte Punkte */
    if (!parse_signature(signature, signature_length, r, s))
        return false;

    /* r und s muessen im Bereich 1 .. n-1 liegen. */
    if (mont_is_zero(&order, r) || mont_is_zero(&order, s))
        return false;
    if (!less_than(r, p256_n) || !less_than(s, p256_n))
        return false;

    /* Der Streuwert wird als Zahl gelesen; ist er laenger als die
     * Gruppenordnung, zaehlen nur die vorderen Bits. */
    size_t take = MIN(hash_length, (size_t)32);
    if (!load_be(e, hash, take))
        return false;

    /* Den oeffentlichen Punkt einlesen und in den Rechenbereich holen. */
    uint64_t qx[P256_LIMBS], qy[P256_LIMBS];

    if (!load_be(qx, point + 1, 32) || !load_be(qy, point + 33, 32))
        return false;
    if (!less_than(qx, p256_p) || !less_than(qy, p256_p))
        return false;

    mont_enter(&field, q.x, qx);
    mont_enter(&field, q.y, qy);
    fe_copy(q.z, field.one);

    /* Liegt der Punkt ueberhaupt auf der Kurve? y^2 = x^3 - 3x + b */
    {
        uint64_t left[P256_LIMBS], right[P256_LIMBS], t[P256_LIMBS];

        mont_mul(&field, left, q.y, q.y);
        mont_mul(&field, right, q.x, q.x);
        mont_mul(&field, right, right, q.x);
        mont_add(&field, t, q.x, q.x);
        mont_add(&field, t, t, q.x);
        mont_sub(&field, right, right, t);
        mont_add(&field, right, right, mont_b);

        if (memcmp(left, right, sizeof(left)) != 0)
            return false;
    }

    order_invert(w, s);
    order_mul(u1, e, w);
    order_mul(u2, r, w);

    point_multiply(&a, u1, &generator);
    point_multiply(&b, u2, &q);
    point_add(&result, &a, &b);

    if (fe_is_zero(result.z))
        return false;

    /* Zurueck zu gewoehnlichen Koordinaten: x = X / Z^2. */
    static const uint8_t p_minus_2[32] = {
        0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfd,
    };
    uint64_t zinv[P256_LIMBS], x[P256_LIMBS];

    mont_exp(&field, zinv, result.z, p_minus_2, sizeof(p_minus_2));
    mont_mul(&field, zinv, zinv, zinv);
    mont_mul(&field, x, result.x, zinv);
    mont_leave(&field, x, x);

    /* Vergleich mit r geschieht modulo n. */
    if (!less_than(x, p256_n)) {
        uint64_t reduced[P256_LIMBS];
        uint64_t borrow = 0;

        for (int i = 0; i < P256_LIMBS; i++) {
            uint64_t left = x[i];
            uint64_t right = p256_n[i];
            uint64_t diff = left - right - borrow;

            borrow = (left < right + borrow) || (right + borrow < right);
            reduced[i] = diff;
        }
        if (!borrow)
            memcpy(x, reduced, sizeof(x));
    }

    return memcmp(x, r, sizeof(x)) == 0;
}
