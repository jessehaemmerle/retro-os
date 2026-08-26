/* crypto.h - die kryptografischen Bausteine fuer TLS.
 *
 * Alles hier ist von Hand geschrieben, weil RetroOS keine Bibliotheken
 * einbindet. Die Verfahren sind die, die TLS 1.3 vorschreibt bzw. erlaubt:
 *
 *   SHA-256              Streuwertfunktion, Grundlage von allem Weiteren
 *   HMAC / HKDF          daraus abgeleitete Schluessel
 *   ChaCha20-Poly1305    Verschluesselung mit Echtheitsnachweis
 *   AES-128/256-GCM      dasselbe, das von TLS 1.3 vorgeschriebene Verfahren
 *   X25519               Schluesseltausch ueber eine elliptische Kurve
 *   RSA / ECDSA P-256    Pruefung der Unterschriften in Zertifikaten
 *
 * Ein Hinweis in eigener Sache: selbstgeschriebene Kryptografie ist
 * gegenueber Seitenkanalangriffen nicht so sorgfaeltig gehaertet wie
 * gewachsene Bibliotheken. Fuer das Abrufen von Webseiten reicht sie;
 * Geheimnisse, an denen etwas haengt, gehoeren hier nicht hinein.
 */
#ifndef CRYPTO_H
#define CRYPTO_H

#include "retro.h"

/* --- SHA-256 --------------------------------------------------------- */

#define SHA256_SIZE  32
#define SHA256_BLOCK 64

struct sha256 {
    uint32_t state[8];
    uint64_t length;
    uint8_t  buffer[SHA256_BLOCK];
    size_t   used;
};

void sha256_init(struct sha256 *ctx);
void sha256_update(struct sha256 *ctx, const void *data, size_t length);
void sha256_final(struct sha256 *ctx, uint8_t out[SHA256_SIZE]);
void sha256(const void *data, size_t length, uint8_t out[SHA256_SIZE]);

/* --- SHA-384 (fuer Zertifikate, die damit unterschrieben sind) -------- */

#define SHA384_SIZE  48
#define SHA512_SIZE  64
#define SHA512_BLOCK 128

struct sha512 {
    uint64_t state[8];
    uint64_t length_low, length_high;
    uint8_t  buffer[SHA512_BLOCK];
    size_t   used;
    size_t   digest_size;
};

void sha384_init(struct sha512 *ctx);
void sha512_init(struct sha512 *ctx);
void sha512_update(struct sha512 *ctx, const void *data, size_t length);
void sha512_final(struct sha512 *ctx, uint8_t *out);
void sha384(const void *data, size_t length, uint8_t out[SHA384_SIZE]);
void sha512(const void *data, size_t length, uint8_t out[SHA512_SIZE]);

/* --- HMAC und HKDF --------------------------------------------------- */

void hmac_sha256(const uint8_t *key, size_t key_length,
                 const void *data, size_t data_length,
                 uint8_t out[SHA256_SIZE]);

void hkdf_extract(const uint8_t *salt, size_t salt_length,
                  const uint8_t *ikm, size_t ikm_length,
                  uint8_t out[SHA256_SIZE]);
void hkdf_expand(const uint8_t prk[SHA256_SIZE],
                 const uint8_t *info, size_t info_length,
                 uint8_t *out, size_t out_length);

/* TLS-eigene Ableitung: HKDF-Expand mit vorangestelltem "tls13 ". */
void hkdf_expand_label(const uint8_t secret[SHA256_SIZE], const char *label,
                       const uint8_t *context, size_t context_length,
                       uint8_t *out, size_t out_length);

/* --- ChaCha20-Poly1305 ----------------------------------------------- */

void chacha20_block(const uint8_t key[32], const uint8_t nonce[12],
                    uint32_t counter, uint8_t out[64]);
void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12],
                  uint32_t counter, const uint8_t *in, uint8_t *out,
                  size_t length);

void poly1305(const uint8_t key[32], const void *data, size_t length,
              uint8_t out[16]);

bool chacha20poly1305_seal(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_length,
                           const uint8_t *plain, size_t length,
                           uint8_t *cipher, uint8_t tag[16]);
bool chacha20poly1305_open(const uint8_t key[32], const uint8_t nonce[12],
                           const uint8_t *aad, size_t aad_length,
                           const uint8_t *cipher, size_t length,
                           const uint8_t tag[16], uint8_t *plain);

/* --- AES-GCM ---------------------------------------------------------- */

struct aes_key {
    uint32_t round_key[60];
    int      rounds;
};

void aes_set_key(struct aes_key *key, const uint8_t *material, size_t bits);
void aes_encrypt_block(const struct aes_key *key, const uint8_t in[16],
                       uint8_t out[16]);

bool aes_gcm_seal(const struct aes_key *key, const uint8_t nonce[12],
                  const uint8_t *aad, size_t aad_length,
                  const uint8_t *plain, size_t length,
                  uint8_t *cipher, uint8_t tag[16]);
bool aes_gcm_open(const struct aes_key *key, const uint8_t nonce[12],
                  const uint8_t *aad, size_t aad_length,
                  const uint8_t *cipher, size_t length,
                  const uint8_t tag[16], uint8_t *plain);

/* --- X25519 ----------------------------------------------------------- */

void x25519_base(uint8_t out[32], const uint8_t secret[32]);
void x25519(uint8_t out[32], const uint8_t secret[32], const uint8_t point[32]);

/* --- Zufall ----------------------------------------------------------- */

void crypto_random(void *out, size_t length);
void crypto_seed(const void *data, size_t length);

/* --- Vergleich ohne Zeitunterschied ----------------------------------- */

bool crypto_equal(const void *a, const void *b, size_t length);

#endif /* CRYPTO_H */
