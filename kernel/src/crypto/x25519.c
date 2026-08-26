/* x25519.c - Schluesseltausch ueber die Kurve Curve25519.
 *
 * Beide Seiten waehlen eine geheime Zahl, multiplizieren damit einen
 * festen Punkt der Kurve und tauschen die Ergebnisse. Multipliziert jede
 * Seite das empfangene Ergebnis noch einmal mit ihrer eigenen Zahl, kommt
 * beidesmal dasselbe heraus - und wer nur zuhoert, kann daraus nichts
 * gewinnen.
 *
 * Gerechnet wird modulo 2^255-19. Die Zahlen werden in fuenf Teilen zu je
 * 51 Bit gehalten, damit Produkte in 128 Bit passen und Uebertraege selten
 * ausgeglichen werden muessen. Die Leiter laeuft immer alle 255 Schritte
 * und tauscht die Zwischenwerte ohne Verzweigung - so verraet die Laufzeit
 * nichts ueber den Schluessel.
 */

#include "crypto.h"
#include "kstring.h"

typedef uint64_t felem[5];

#define MASK51 0x7ffffffffffffULL

static void fe_zero(felem out)
{
    for (int i = 0; i < 5; i++)
        out[i] = 0;
}

static void fe_one(felem out)
{
    fe_zero(out);
    out[0] = 1;
}

static void fe_copy(felem out, const felem in)
{
    for (int i = 0; i < 5; i++)
        out[i] = in[i];
}

static void fe_add(felem out, const felem a, const felem b)
{
    for (int i = 0; i < 5; i++)
        out[i] = a[i] + b[i];
}

/* Subtraktion mit vorher aufaddiertem Vielfachen des Moduls, damit nichts
 * negativ wird. */
static void fe_sub(felem out, const felem a, const felem b)
{
    out[0] = a[0] + 0xfffffffffffdaULL - b[0];
    out[1] = a[1] + 0xffffffffffffeULL - b[1];
    out[2] = a[2] + 0xffffffffffffeULL - b[2];
    out[3] = a[3] + 0xffffffffffffeULL - b[3];
    out[4] = a[4] + 0xffffffffffffeULL - b[4];
}

static void fe_carry(felem out, unsigned __int128 t[5])
{
    unsigned __int128 carry;

    carry = t[0] >> 51; t[0] &= MASK51; t[1] += carry;
    carry = t[1] >> 51; t[1] &= MASK51; t[2] += carry;
    carry = t[2] >> 51; t[2] &= MASK51; t[3] += carry;
    carry = t[3] >> 51; t[3] &= MASK51; t[4] += carry;
    carry = t[4] >> 51; t[4] &= MASK51; t[0] += (uint64_t)carry * 19;
    carry = t[0] >> 51; t[0] &= MASK51; t[1] += carry;

    for (int i = 0; i < 5; i++)
        out[i] = (uint64_t)t[i];
}

static void fe_mul(felem out, const felem a, const felem b)
{
    unsigned __int128 t[5];

    /* Die Teile oberhalb von 2^255 wandern mit Faktor 19 nach unten. */
    t[0] = (unsigned __int128)a[0] * b[0] +
           (unsigned __int128)a[1] * (19 * b[4]) +
           (unsigned __int128)a[2] * (19 * b[3]) +
           (unsigned __int128)a[3] * (19 * b[2]) +
           (unsigned __int128)a[4] * (19 * b[1]);
    t[1] = (unsigned __int128)a[0] * b[1] +
           (unsigned __int128)a[1] * b[0] +
           (unsigned __int128)a[2] * (19 * b[4]) +
           (unsigned __int128)a[3] * (19 * b[3]) +
           (unsigned __int128)a[4] * (19 * b[2]);
    t[2] = (unsigned __int128)a[0] * b[2] +
           (unsigned __int128)a[1] * b[1] +
           (unsigned __int128)a[2] * b[0] +
           (unsigned __int128)a[3] * (19 * b[4]) +
           (unsigned __int128)a[4] * (19 * b[3]);
    t[3] = (unsigned __int128)a[0] * b[3] +
           (unsigned __int128)a[1] * b[2] +
           (unsigned __int128)a[2] * b[1] +
           (unsigned __int128)a[3] * b[0] +
           (unsigned __int128)a[4] * (19 * b[4]);
    t[4] = (unsigned __int128)a[0] * b[4] +
           (unsigned __int128)a[1] * b[3] +
           (unsigned __int128)a[2] * b[2] +
           (unsigned __int128)a[3] * b[1] +
           (unsigned __int128)a[4] * b[0];

    fe_carry(out, t);
}

static void fe_square(felem out, const felem a)
{
    fe_mul(out, a, a);
}

static void fe_mul121666(felem out, const felem a)
{
    unsigned __int128 t[5];

    for (int i = 0; i < 5; i++)
        t[i] = (unsigned __int128)a[i] * 121666;

    fe_carry(out, t);
}

/* Vertauscht a und b, wenn swap gesetzt ist - ohne Sprung. */
static void fe_cswap(felem a, felem b, uint64_t swap)
{
    uint64_t mask = 0 - swap;

    for (int i = 0; i < 5; i++) {
        uint64_t diff = mask & (a[i] ^ b[i]);

        a[i] ^= diff;
        b[i] ^= diff;
    }
}

/* Kehrwert ueber den kleinen Satz von Fermat: a^(p-2). */
static void fe_invert(felem out, const felem z)
{
    felem t0, t1, t2, t3;

    fe_square(t0, z);
    fe_square(t1, t0);
    fe_square(t1, t1);
    fe_mul(t1, z, t1);
    fe_mul(t0, t0, t1);
    fe_square(t2, t0);
    fe_mul(t1, t1, t2);
    fe_square(t2, t1);
    for (int i = 1; i < 5; i++)
        fe_square(t2, t2);
    fe_mul(t1, t2, t1);
    fe_square(t2, t1);
    for (int i = 1; i < 10; i++)
        fe_square(t2, t2);
    fe_mul(t2, t2, t1);
    fe_square(t3, t2);
    for (int i = 1; i < 20; i++)
        fe_square(t3, t3);
    fe_mul(t2, t3, t2);
    fe_square(t2, t2);
    for (int i = 1; i < 10; i++)
        fe_square(t2, t2);
    fe_mul(t1, t2, t1);
    fe_square(t2, t1);
    for (int i = 1; i < 50; i++)
        fe_square(t2, t2);
    fe_mul(t2, t2, t1);
    fe_square(t3, t2);
    for (int i = 1; i < 100; i++)
        fe_square(t3, t3);
    fe_mul(t2, t3, t2);
    fe_square(t2, t2);
    for (int i = 1; i < 50; i++)
        fe_square(t2, t2);
    fe_mul(t1, t2, t1);
    fe_square(t1, t1);
    for (int i = 1; i < 5; i++)
        fe_square(t1, t1);
    fe_mul(out, t1, t0);
}

static void fe_from_bytes(felem out, const uint8_t in[32])
{
    uint64_t words[4];

    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;

        for (int b = 7; b >= 0; b--)
            v = (v << 8) | in[i * 8 + b];
        words[i] = v;
    }

    out[0] = words[0] & MASK51;
    out[1] = ((words[0] >> 51) | (words[1] << 13)) & MASK51;
    out[2] = ((words[1] >> 38) | (words[2] << 26)) & MASK51;
    out[3] = ((words[2] >> 25) | (words[3] << 39)) & MASK51;
    out[4] = (words[3] >> 12) & MASK51;
}

static void fe_to_bytes(uint8_t out[32], const felem in)
{
    felem t;
    uint64_t carry;

    fe_copy(t, in);

    /* Vollstaendig reduzieren. */
    for (int pass = 0; pass < 3; pass++) {
        carry = t[0] >> 51; t[0] &= MASK51; t[1] += carry;
        carry = t[1] >> 51; t[1] &= MASK51; t[2] += carry;
        carry = t[2] >> 51; t[2] &= MASK51; t[3] += carry;
        carry = t[3] >> 51; t[3] &= MASK51; t[4] += carry;
        carry = t[4] >> 51; t[4] &= MASK51; t[0] += carry * 19;
    }

    /* Falls t >= p, p abziehen. */
    uint64_t q = (t[0] + 19) >> 51;
    q = (t[1] + q) >> 51;
    q = (t[2] + q) >> 51;
    q = (t[3] + q) >> 51;
    q = (t[4] + q) >> 51;

    t[0] += 19 * q;
    carry = t[0] >> 51; t[0] &= MASK51; t[1] += carry;
    carry = t[1] >> 51; t[1] &= MASK51; t[2] += carry;
    carry = t[2] >> 51; t[2] &= MASK51; t[3] += carry;
    carry = t[3] >> 51; t[3] &= MASK51; t[4] += carry;
    t[4] &= MASK51;

    uint64_t words[4];

    words[0] = t[0] | (t[1] << 51);
    words[1] = (t[1] >> 13) | (t[2] << 38);
    words[2] = (t[2] >> 26) | (t[3] << 25);
    words[3] = (t[3] >> 39) | (t[4] << 12);

    for (int i = 0; i < 4; i++) {
        for (int b = 0; b < 8; b++)
            out[i * 8 + b] = (uint8_t)(words[i] >> (b * 8));
    }
}

void x25519(uint8_t out[32], const uint8_t secret[32], const uint8_t point[32])
{
    uint8_t clamped[32];
    felem x1, x2, z2, x3, z3;
    felem a, b, c, d, e, aa, bb, da, cb;

    memcpy(clamped, secret, 32);
    clamped[0] &= 248;         /* die untersten drei Bit loeschen  */
    clamped[31] &= 127;        /* das oberste Bit loeschen         */
    clamped[31] |= 64;         /* das zweitoberste Bit setzen      */

    fe_from_bytes(x1, point);
    fe_one(x2);
    fe_zero(z2);
    fe_copy(x3, x1);
    fe_one(z3);

    uint64_t swap = 0;

    for (int pos = 254; pos >= 0; pos--) {
        uint64_t bit = (clamped[pos / 8] >> (pos % 8)) & 1;

        swap ^= bit;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = bit;

        fe_sub(a, x2, z2);
        fe_square(aa, a);
        fe_add(b, x2, z2);
        fe_square(bb, b);
        fe_sub(e, bb, aa);
        fe_add(c, x3, z3);
        fe_sub(d, x3, z3);
        fe_mul(da, d, b);
        fe_mul(cb, c, a);

        fe_add(x3, da, cb);
        fe_square(x3, x3);
        fe_sub(z3, da, cb);
        fe_square(z3, z3);
        fe_mul(z3, z3, x1);

        fe_mul(x2, aa, bb);
        fe_mul121666(z2, e);
        fe_add(z2, z2, aa);
        fe_mul(z2, z2, e);
    }

    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_to_bytes(out, x2);
}

void x25519_base(uint8_t out[32], const uint8_t secret[32])
{
    static const uint8_t base[32] = { 9 };

    x25519(out, secret, base);
}
