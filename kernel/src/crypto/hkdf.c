/* hkdf.c - HMAC und die Schluesselableitung von TLS 1.3.
 *
 * HMAC bindet einen Schluessel an eine Streuwertfunktion: der Schluessel
 * wird zweimal eingemischt, einmal vor und einmal nach der Nachricht.
 * HKDF macht daraus ein Verfahren, das aus einem gemeinsamen Geheimnis
 * beliebig viele Schluessel gewinnt - genau das braucht TLS nach dem
 * Schluesseltausch.
 */

#include "crypto.h"
#include "kstring.h"

void hmac_sha256(const uint8_t *key, size_t key_length,
                 const void *data, size_t data_length,
                 uint8_t out[SHA256_SIZE])
{
    uint8_t block[SHA256_BLOCK];
    uint8_t inner[SHA256_SIZE];
    struct sha256 ctx;

    memset(block, 0, sizeof(block));

    /* Zu lange Schluessel werden erst gestreut. */
    if (key_length > SHA256_BLOCK)
        sha256(key, key_length, block);
    else
        memcpy(block, key, key_length);

    uint8_t pad[SHA256_BLOCK];

    for (size_t i = 0; i < SHA256_BLOCK; i++)
        pad[i] = (uint8_t)(block[i] ^ 0x36);

    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof(pad));
    sha256_update(&ctx, data, data_length);
    sha256_final(&ctx, inner);

    for (size_t i = 0; i < SHA256_BLOCK; i++)
        pad[i] = (uint8_t)(block[i] ^ 0x5C);

    sha256_init(&ctx);
    sha256_update(&ctx, pad, sizeof(pad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, out);
}

void hkdf_extract(const uint8_t *salt, size_t salt_length,
                  const uint8_t *ikm, size_t ikm_length,
                  uint8_t out[SHA256_SIZE])
{
    static const uint8_t zero[SHA256_SIZE] = { 0 };

    if (!salt || salt_length == 0) {
        salt = zero;
        salt_length = sizeof(zero);
    }
    hmac_sha256(salt, salt_length, ikm, ikm_length, out);
}

void hkdf_expand(const uint8_t prk[SHA256_SIZE],
                 const uint8_t *info, size_t info_length,
                 uint8_t *out, size_t out_length)
{
    uint8_t block[SHA256_SIZE];
    size_t produced = 0;
    uint8_t counter = 1;
    size_t block_length = 0;

    while (produced < out_length) {
        struct sha256 ctx;
        uint8_t pad[SHA256_BLOCK];
        uint8_t inner[SHA256_SIZE];
        uint8_t key[SHA256_BLOCK];

        /* HMAC von Hand, weil die Nachricht aus drei Teilen besteht. */
        memset(key, 0, sizeof(key));
        memcpy(key, prk, SHA256_SIZE);

        for (size_t i = 0; i < SHA256_BLOCK; i++)
            pad[i] = (uint8_t)(key[i] ^ 0x36);

        sha256_init(&ctx);
        sha256_update(&ctx, pad, sizeof(pad));
        if (block_length)
            sha256_update(&ctx, block, block_length);
        sha256_update(&ctx, info, info_length);
        sha256_update(&ctx, &counter, 1);
        sha256_final(&ctx, inner);

        for (size_t i = 0; i < SHA256_BLOCK; i++)
            pad[i] = (uint8_t)(key[i] ^ 0x5C);

        sha256_init(&ctx);
        sha256_update(&ctx, pad, sizeof(pad));
        sha256_update(&ctx, inner, sizeof(inner));
        sha256_final(&ctx, block);

        block_length = SHA256_SIZE;

        size_t take = MIN(SHA256_SIZE, out_length - produced);
        memcpy(out + produced, block, take);
        produced += take;
        counter++;
    }
}

void hkdf_expand_label(const uint8_t secret[SHA256_SIZE], const char *label,
                       const uint8_t *context, size_t context_length,
                       uint8_t *out, size_t out_length)
{
    uint8_t info[512];
    size_t pos = 0;
    size_t label_length = strlen(label);

    /* Aufbau nach RFC 8446: Laenge, "tls13 " + Bezeichnung, Zusatz. */
    info[pos++] = (uint8_t)(out_length >> 8);
    info[pos++] = (uint8_t)(out_length & 0xFF);

    info[pos++] = (uint8_t)(6 + label_length);
    memcpy(info + pos, "tls13 ", 6);
    pos += 6;
    memcpy(info + pos, label, label_length);
    pos += label_length;

    info[pos++] = (uint8_t)context_length;
    if (context_length) {
        memcpy(info + pos, context, context_length);
        pos += context_length;
    }

    hkdf_expand(secret, info, pos, out, out_length);
}

bool crypto_equal(const void *a, const void *b, size_t length)
{
    const uint8_t *x = a, *y = b;
    uint8_t diff = 0;

    /* Immer alles vergleichen - die Laufzeit soll nichts verraten. */
    for (size_t i = 0; i < length; i++)
        diff |= (uint8_t)(x[i] ^ y[i]);

    return diff == 0;
}
