/* bignum.h - grosse ganze Zahlen fuer RSA.
 *
 * Feste Groesse statt beliebig wachsender Zahlen: RSA-Schluessel sind
 * hoechstens 4096 Bit lang, und feste Groessen ersparen jede
 * Speicherverwaltung im Kernel.
 */
#ifndef BIGNUM_H
#define BIGNUM_H

#include "retro.h"

#define BN_MAX_LIMBS 64                    /* 64 * 64 Bit = 4096 Bit */
#define BN_MAX_BYTES (BN_MAX_LIMBS * 8)

struct bignum {
    uint64_t limb[BN_MAX_LIMBS];           /* limb[0] ist das niederwertigste */
    size_t   used;
};

void bn_zero(struct bignum *a);
bool bn_from_bytes(struct bignum *a, const uint8_t *data, size_t length);
void bn_to_bytes(const struct bignum *a, uint8_t *out, size_t length);

int  bn_compare(const struct bignum *a, const struct bignum *b);
bool bn_is_zero(const struct bignum *a);
size_t bn_bits(const struct bignum *a);

/* Modulare Potenzierung: out = base^exponent mod modulus.
 * Gerechnet wird im Montgomery-Bereich, damit keine Division noetig ist. */
bool bn_modexp(struct bignum *out, const struct bignum *base,
               uint64_t exponent, const struct bignum *modulus);

/* --- Montgomery-Bereich zum Weiterverwenden ---------------------------
 *
 * Wer mehrere Rechnungen mit demselben Modul anstellt (etwa die Punkte
 * einer elliptischen Kurve), richtet den Bereich einmal ein und rechnet
 * dann darin. Alle Werte sind dabei mit R multipliziert.
 */
struct mont {
    uint64_t modulus[BN_MAX_LIMBS];
    uint64_t r2[BN_MAX_LIMBS];        /* R^2 mod n, zum Hineinholen */
    uint64_t one[BN_MAX_LIMBS];       /* die Eins im Bereich        */
    uint64_t n0inv;
    size_t   limbs;
};

bool mont_init(struct mont *m, const uint64_t *modulus, size_t limbs);
void mont_mul(const struct mont *m, uint64_t *out, const uint64_t *a,
              const uint64_t *b);
void mont_enter(const struct mont *m, uint64_t *out, const uint64_t *a);
void mont_leave(const struct mont *m, uint64_t *out, const uint64_t *a);
void mont_add(const struct mont *m, uint64_t *out, const uint64_t *a,
              const uint64_t *b);
void mont_sub(const struct mont *m, uint64_t *out, const uint64_t *a,
              const uint64_t *b);
/* out = a^exponent mod n, Exponent als Bytefolge (hoechstwertig zuerst). */
void mont_exp(const struct mont *m, uint64_t *out, const uint64_t *a,
              const uint8_t *exponent, size_t exponent_length);
bool mont_is_zero(const struct mont *m, const uint64_t *a);

#endif /* BIGNUM_H */
