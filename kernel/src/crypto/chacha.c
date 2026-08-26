/* chacha.c - ChaCha20 und Poly1305.
 *
 * ChaCha20 erzeugt aus Schluessel, Nonce und Zaehler einen Strom, der mit
 * dem Klartext verrechnet wird. Poly1305 bildet daraus einen Nachweis,
 * dass niemand etwas veraendert hat. Beides kommt ohne Tabellen aus und
 * arbeitet deshalb unabhaengig von den Daten - was Angriffe ueber die
 * Laufzeit erschwert.
 */

#include "crypto.h"
#include "kstring.h"

/* --- ChaCha20 -------------------------------------------------------- */

static uint32_t rotl32(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

#define QUARTERROUND(a, b, c, d)          \
    a += b; d ^= a; d = rotl32(d, 16);    \
    c += d; b ^= c; b = rotl32(b, 12);    \
    a += b; d ^= a; d = rotl32(d, 8);     \
    c += d; b ^= c; b = rotl32(b, 7)

static uint32_t load32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                    uint32_t counter, uint8_t out[64])
{
    uint32_t state[16];
    uint32_t working[16];

    /* "expand 32-byte k" - die feste Kennung am Anfang. */
    state[0] = 0x61707865; state[1] = 0x3320646e;
    state[2] = 0x79622d32; state[3] = 0x6b206574;

    for (int i = 0; i < 8; i++)
        state[4 + i] = load32le(key + i * 4);

    state[12] = counter;
    for (int i = 0; i < 3; i++)
        state[13 + i] = load32le(nonce + i * 4);

    memcpy(working, state, sizeof(state));

    for (int i = 0; i < 10; i++) {
        QUARTERROUND(working[0], working[4], working[8],  working[12]);
        QUARTERROUND(working[1], working[5], working[9],  working[13]);
        QUARTERROUND(working[2], working[6], working[10], working[14]);
        QUARTERROUND(working[3], working[7], working[11], working[15]);
        QUARTERROUND(working[0], working[5], working[10], working[15]);
        QUARTERROUND(working[1], working[6], working[11], working[12]);
        QUARTERROUND(working[2], working[7], working[8],  working[13]);
        QUARTERROUND(working[3], working[4], working[9],  working[14]);
    }

    for (int i = 0; i < 16; i++)
        store32le(out + i * 4, working[i] + state[i]);
}

void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                  uint32_t counter, const uint8_t *in, uint8_t *out,
                  size_t length)
{
    uint8_t block[64];
    size_t done = 0;

    while (done < length) {
        chacha20_block(key, nonce, counter++, block);

        size_t take = MIN((size_t)64, length - done);
        for (size_t i = 0; i < take; i++)
            out[done + i] = (uint8_t)(in[done + i] ^ block[i]);
        done += take;
    }
}

/* --- Poly1305 -------------------------------------------------------- */

/* Gerechnet wird modulo 2^130-5. Die Zahl wird in fuenf Teilen zu je
 * 26 Bit gehalten, damit die Zwischenergebnisse in 64 Bit passen. */
struct poly1305_state {
    uint32_t r[5];
    uint32_t h[5];
    uint32_t pad[4];
};

static void poly1305_blocks(struct poly1305_state *st, const uint8_t *data,
                            size_t length, bool final_block)
{
    uint32_t hibit = final_block ? 0 : (1UL << 24);

    uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2];
    uint32_t r3 = st->r[3], r4 = st->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;

    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2];
    uint32_t h3 = st->h[3], h4 = st->h[4];

    while (length >= 16) {
        h0 += (load32le(data + 0)) & 0x3ffffff;
        h1 += (load32le(data + 3) >> 2) & 0x3ffffff;
        h2 += (load32le(data + 6) >> 4) & 0x3ffffff;
        h3 += (load32le(data + 9) >> 6) & 0x3ffffff;
        h4 += (load32le(data + 12) >> 8) | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 +
                      (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 +
                      (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 +
                      (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 +
                      (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 +
                      (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint32_t carry = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += carry; carry = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += carry; carry = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += carry; carry = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += carry; carry = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += carry * 5; carry = h0 >> 26; h0 &= 0x3ffffff;
        h1 += carry;

        data += 16;
        length -= 16;
    }

    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2;
    st->h[3] = h3; st->h[4] = h4;
}

void poly1305(const uint8_t key[32], const void *data, size_t length,
              uint8_t out[16])
{
    struct poly1305_state st;
    const uint8_t *p = data;

    /* r wird nach Vorschrift beschnitten. */
    st.r[0] = (load32le(key + 0)) & 0x3ffffff;
    st.r[1] = (load32le(key + 3) >> 2) & 0x3ffff03;
    st.r[2] = (load32le(key + 6) >> 4) & 0x3ffc0ff;
    st.r[3] = (load32le(key + 9) >> 6) & 0x3f03fff;
    st.r[4] = (load32le(key + 12) >> 8) & 0x00fffff;

    memset(st.h, 0, sizeof(st.h));
    for (int i = 0; i < 4; i++)
        st.pad[i] = load32le(key + 16 + i * 4);

    size_t whole = length & ~(size_t)15;

    if (whole)
        poly1305_blocks(&st, p, whole, false);

    size_t rest = length - whole;
    if (rest) {
        uint8_t block[16];

        memset(block, 0, sizeof(block));
        memcpy(block, p + whole, rest);
        block[rest] = 1;
        poly1305_blocks(&st, block, 16, true);
    }

    /* Uebertraege ausgleichen. */
    uint32_t h0 = st.h[0], h1 = st.h[1], h2 = st.h[2];
    uint32_t h3 = st.h[3], h4 = st.h[4];
    uint32_t carry;

    carry = h1 >> 26; h1 &= 0x3ffffff;
    h2 += carry; carry = h2 >> 26; h2 &= 0x3ffffff;
    h3 += carry; carry = h3 >> 26; h3 &= 0x3ffffff;
    h4 += carry; carry = h4 >> 26; h4 &= 0x3ffffff;
    h0 += carry * 5; carry = h0 >> 26; h0 &= 0x3ffffff;
    h1 += carry;

    /* Falls das Ergebnis >= 2^130-5 ist, wird der Rest genommen. */
    uint32_t g0 = h0 + 5; carry = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + carry; carry = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + carry; carry = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + carry; carry = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + carry - (1UL << 26);

    uint32_t mask = (g4 >> 31) - 1;   /* 0xFFFFFFFF, wenn g >= 0 */

    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* Zurueck in vier 32-Bit-Woerter und den Schluesselrest addieren. */
    uint64_t f;
    uint32_t w0 = (h0) | (h1 << 26);
    uint32_t w1 = (h1 >> 6) | (h2 << 20);
    uint32_t w2 = (h2 >> 12) | (h3 << 14);
    uint32_t w3 = (h3 >> 18) | (h4 << 8);

    f = (uint64_t)w0 + st.pad[0]; w0 = (uint32_t)f;
    f = (uint64_t)w1 + st.pad[1] + (f >> 32); w1 = (uint32_t)f;
    f = (uint64_t)w2 + st.pad[2] + (f >> 32); w2 = (uint32_t)f;
    f = (uint64_t)w3 + st.pad[3] + (f >> 32); w3 = (uint32_t)f;

    store32le(out + 0, w0);
    store32le(out + 4, w1);
    store32le(out + 8, w2);
    store32le(out + 12, w3);
}

/* --- Die Verbindung beider Verfahren --------------------------------- */

static void poly1305_key(const uint8_t key[32], const uint8_t nonce[12],
                         uint8_t out[32])
{
    uint8_t block[64];

    /* Der Schluessel fuer den Nachweis kommt aus Block 0 des Stroms. */
    chacha20_block(key, nonce, 0, block);
    memcpy(out, block, 32);
}

/* Der Nachweis geht ueber Zusatzdaten und Geheimtext, beide auf ein
 * Vielfaches von 16 aufgefuellt, dann beide Laengen. */
static void poly1305_aead(const uint8_t poly_key[32],
                          const uint8_t *aad, size_t aad_length,
                          const uint8_t *cipher, size_t cipher_length,
                          uint8_t tag[16])
{
    uint8_t buffer[16384 + 64];
    size_t pos = 0;

    if (aad_length + cipher_length + 32 > sizeof(buffer)) {
        memset(tag, 0, 16);
        return;
    }

    memcpy(buffer + pos, aad, aad_length);
    pos += aad_length;
    while (pos % 16)
        buffer[pos++] = 0;

    memcpy(buffer + pos, cipher, cipher_length);
    pos += cipher_length;
    while (pos % 16)
        buffer[pos++] = 0;

    for (int i = 0; i < 8; i++)
        buffer[pos++] = (uint8_t)(aad_length >> (i * 8));
    for (int i = 0; i < 8; i++)
        buffer[pos++] = (uint8_t)(cipher_length >> (i * 8));

    poly1305(poly_key, buffer, pos, tag);
}

bool chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_length,
                           const uint8_t *plain, size_t length,
                           uint8_t *cipher, uint8_t tag[16])
{
    uint8_t poly_key[32];

    poly1305_key(key, nonce, poly_key);
    chacha20_xor(key, nonce, 1, plain, cipher, length);
    poly1305_aead(poly_key, aad, aad_length, cipher, length, tag);
    return true;
}

bool chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_length,
                           const uint8_t *cipher, size_t length,
                           const uint8_t tag[16], uint8_t *plain)
{
    uint8_t poly_key[32];
    uint8_t expected[16];

    poly1305_key(key, nonce, poly_key);
    poly1305_aead(poly_key, aad, aad_length, cipher, length, expected);

    if (!crypto_equal(expected, tag, 16))
        return false;

    chacha20_xor(key, nonce, 1, cipher, plain, length);
    return true;
}
