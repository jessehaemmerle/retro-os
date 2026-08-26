/* rsa.c - Unterschriften mit RSA pruefen.
 *
 * Gerechnet wird nur mit dem oeffentlichen Teil: die Unterschrift wird mit
 * dem oeffentlichen Exponenten potenziert, und heraus kommt eine
 * aufbereitete Form des Streuwerts. Stimmt sie mit dem ueberein, was wir
 * selbst aus den Daten gebildet haben, ist die Unterschrift echt.
 *
 * Zwei Aufbereitungen sind gebraeuchlich: die aeltere PKCS#1 v1.5 (feste
 * Auffuellung) und die neuere PSS (mit Zufallsanteil). TLS 1.3 verlangt
 * fuer den Handschlag PSS; Zertifikate benutzen beides.
 */

#include "pki.h"
#include "bignum.h"
#include "crypto.h"
#include "kstring.h"

/* Die feste Kennung, die PKCS#1 v1.5 vor den Streuwert setzt. */
static const uint8_t prefix_sha256[] = {
    0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
    0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20,
};
static const uint8_t prefix_sha384[] = {
    0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
    0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30,
};
static const uint8_t prefix_sha512[] = {
    0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65,
    0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40,
};

/* Bitlaenge des Moduls - PSS braucht sie fuer die Randbedingungen. */
static size_t bn_bits_of_modulus(const struct public_key *key)
{
    struct bignum n;

    if (!bn_from_bytes(&n, key->modulus, key->modulus_length))
        return 0;
    return bn_bits(&n);
}

/* Potenziert die Unterschrift und liefert die aufbereitete Form. */
static bool rsa_public(const struct public_key *key, const uint8_t *signature,
                       size_t signature_length, uint8_t *out, size_t out_length)
{
    struct bignum s, n, result;

    if (key->type != KEY_RSA || !key->modulus)
        return false;
    if (signature_length != out_length)
        return false;

    if (!bn_from_bytes(&s, signature, signature_length))
        return false;
    if (!bn_from_bytes(&n, key->modulus, key->modulus_length))
        return false;
    if (bn_compare(&s, &n) >= 0)
        return false;

    if (!bn_modexp(&result, &s, key->exponent, &n))
        return false;

    bn_to_bytes(&result, out, out_length);
    return true;
}

bool rsa_verify_pkcs1(const struct public_key *key,
                      const uint8_t *hash, size_t hash_length,
                      const uint8_t *signature, size_t signature_length)
{
    uint8_t em[BN_MAX_BYTES];
    const uint8_t *prefix;
    size_t prefix_length;

    if (signature_length > sizeof(em) || signature_length < 64)
        return false;
    if (!rsa_public(key, signature, signature_length, em, signature_length))
        return false;

    switch (hash_length) {
    case 32: prefix = prefix_sha256; prefix_length = sizeof(prefix_sha256); break;
    case 48: prefix = prefix_sha384; prefix_length = sizeof(prefix_sha384); break;
    case 64: prefix = prefix_sha512; prefix_length = sizeof(prefix_sha512); break;
    default: return false;
    }

    /* Erwartet: 0x00 0x01 0xFF...0xFF 0x00 Kennung Streuwert */
    size_t tail = prefix_length + hash_length;

    if (signature_length < tail + 11)
        return false;
    if (em[0] != 0x00 || em[1] != 0x01)
        return false;

    size_t i = 2;
    while (i < signature_length - tail - 1 && em[i] == 0xFF)
        i++;

    if (i < 10 || em[i] != 0x00)
        return false;
    i++;

    if (signature_length - i != tail)
        return false;
    if (memcmp(em + i, prefix, prefix_length) != 0)
        return false;

    return crypto_equal(em + i + prefix_length, hash, hash_length);
}

/* Die Maskenfunktion MGF1: erzeugt aus einem Startwert beliebig viele
 * Bytes, indem ein Zaehler mitgestreut wird. */
static void mgf1(const uint8_t *seed, size_t seed_length, uint8_t *out,
                 size_t out_length, size_t hash_length)
{
    uint8_t block[SHA512_SIZE];
    uint32_t counter = 0;
    size_t produced = 0;

    while (produced < out_length) {
        uint8_t counter_bytes[4] = {
            (uint8_t)(counter >> 24), (uint8_t)(counter >> 16),
            (uint8_t)(counter >> 8), (uint8_t)counter,
        };

        if (hash_length == 32) {
            struct sha256 ctx;

            sha256_init(&ctx);
            sha256_update(&ctx, seed, seed_length);
            sha256_update(&ctx, counter_bytes, 4);
            sha256_final(&ctx, block);
        } else {
            struct sha512 ctx;

            sha384_init(&ctx);
            sha512_update(&ctx, seed, seed_length);
            sha512_update(&ctx, counter_bytes, 4);
            sha512_final(&ctx, block);
        }

        size_t take = MIN(hash_length, out_length - produced);
        memcpy(out + produced, block, take);
        produced += take;
        counter++;
    }
}

bool rsa_verify_pss(const struct public_key *key,
                    const uint8_t *hash, size_t hash_length,
                    const uint8_t *signature, size_t signature_length)
{
    uint8_t em[BN_MAX_BYTES];
    uint8_t db_mask[BN_MAX_BYTES];
    uint8_t check[SHA512_SIZE];

    if (signature_length > sizeof(em))
        return false;
    if (hash_length != 32 && hash_length != 48)
        return false;
    if (!rsa_public(key, signature, signature_length, em, signature_length))
        return false;

    size_t em_length = signature_length;
    size_t em_bits = bn_bits_of_modulus(key) - 1;
    size_t used_bits = em_bits % 8;

    if (em[em_length - 1] != 0xBC)
        return false;

    /* Die obersten Bits muessen Null sein. */
    if (used_bits && (em[0] >> used_bits) != 0)
        return false;

    size_t db_length = em_length - hash_length - 1;
    const uint8_t *h = em + db_length;

    mgf1(h, hash_length, db_mask, db_length, hash_length);

    uint8_t db[BN_MAX_BYTES];
    for (size_t i = 0; i < db_length; i++)
        db[i] = (uint8_t)(em[i] ^ db_mask[i]);

    if (used_bits)
        db[0] &= (uint8_t)(0xFF >> (8 - used_bits));

    /* Vor dem Salz stehen Nullen und genau eine Eins. */
    size_t i = 0;
    while (i < db_length && db[i] == 0)
        i++;
    if (i >= db_length || db[i] != 0x01)
        return false;
    i++;

    size_t salt_length = db_length - i;
    const uint8_t *salt = db + i;

    /* Der Streuwert wird ueber acht Nullbytes, den Streuwert der
     * Nachricht und das Salz gebildet. */
    static const uint8_t zeros[8] = { 0 };

    if (hash_length == 32) {
        struct sha256 ctx;

        sha256_init(&ctx);
        sha256_update(&ctx, zeros, 8);
        sha256_update(&ctx, hash, hash_length);
        sha256_update(&ctx, salt, salt_length);
        sha256_final(&ctx, check);
    } else {
        struct sha512 ctx;

        sha384_init(&ctx);
        sha512_update(&ctx, zeros, 8);
        sha512_update(&ctx, hash, hash_length);
        sha512_update(&ctx, salt, salt_length);
        sha512_final(&ctx, check);
    }

    return crypto_equal(check, h, hash_length);
}
