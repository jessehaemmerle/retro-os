/* pki.h - Unterschriften pruefen und Zertifikate lesen.
 *
 * Beim Verbindungsaufbau schickt der Server eine Kette von Zertifikaten.
 * Jedes davon ist von seinem Vorgaenger unterschrieben; das oberste stammt
 * von einer Stelle, der wir von vornherein vertrauen. Hier steht, wie
 * diese Kette gelesen und geprueft wird.
 */
#ifndef PKI_H
#define PKI_H

#include "retro.h"

/* --- ASN.1 (DER) ------------------------------------------------------ */

struct der {
    const uint8_t *data;
    size_t         length;
    size_t         position;
};

struct der_value {
    uint8_t        tag;
    const uint8_t *content;
    size_t         length;
    const uint8_t *full;        /* einschliesslich Kennung und Laenge */
    size_t         full_length;
};

void der_start(struct der *reader, const uint8_t *data, size_t length);
bool der_next(struct der *reader, struct der_value *out);
/* Steigt in eine Folge (SEQUENCE, SET) hinein. */
bool der_enter(const struct der_value *value, struct der *out);

/* --- Unterschriftsverfahren ------------------------------------------- */

enum signature_algorithm {
    SIG_UNKNOWN = 0,
    SIG_RSA_PKCS1_SHA256,
    SIG_RSA_PKCS1_SHA384,
    SIG_RSA_PKCS1_SHA512,
    SIG_RSA_PSS_SHA256,
    SIG_RSA_PSS_SHA384,
    SIG_ECDSA_P256_SHA256,
    SIG_ECDSA_P384_SHA384,
};

enum public_key_type {
    KEY_UNKNOWN = 0,
    KEY_RSA,
    KEY_EC_P256,
    KEY_EC_P384,
};

struct public_key {
    enum public_key_type type;

    /* RSA */
    const uint8_t *modulus;
    size_t         modulus_length;
    uint64_t       exponent;

    /* Elliptische Kurve: der unkomprimierte Punkt (0x04 || X || Y) */
    const uint8_t *point;
    size_t         point_length;
};

/* --- RSA -------------------------------------------------------------- */

bool rsa_verify_pkcs1(const struct public_key *key,
                      const uint8_t *hash, size_t hash_length,
                      const uint8_t *signature, size_t signature_length);
bool rsa_verify_pss(const struct public_key *key,
                    const uint8_t *hash, size_t hash_length,
                    const uint8_t *signature, size_t signature_length);

/* --- ECDSA ------------------------------------------------------------ */

/* Unterschrift in DER-Form (SEQUENCE { r INTEGER, s INTEGER }). */
bool ecdsa_p256_verify(const uint8_t point[65],
                       const uint8_t *hash, size_t hash_length,
                       const uint8_t *signature, size_t signature_length);

/* --- Zertifikate ------------------------------------------------------ */

#define CERT_NAME_MAX 128

struct certificate {
    const uint8_t *raw;
    size_t         raw_length;

    const uint8_t *tbs;            /* der unterschriebene Teil */
    size_t         tbs_length;

    const uint8_t *issuer;         /* rohe DER-Form zum Vergleichen */
    size_t         issuer_length;
    const uint8_t *subject;
    size_t         subject_length;

    struct public_key key;

    enum signature_algorithm signature_algorithm;
    const uint8_t *signature;
    size_t         signature_length;

    bool is_ca;
    bool has_basic_constraints;

    /* Gueltigkeit als Zahl JJJJMMTTHHMMSS, zum einfachen Vergleichen. */
    uint64_t not_before;
    uint64_t not_after;

    char common_name[CERT_NAME_MAX];
};

bool certificate_parse(struct certificate *cert, const uint8_t *data,
                       size_t length);
/* Prueft, ob der Name (oder ein Platzhalter darin) zum Rechnernamen passt. */
bool certificate_matches_host(const struct certificate *cert, const char *host);
/* Prueft die Unterschrift von "cert" mit dem Schluessel von "issuer". */
bool certificate_verify_signature(const struct certificate *cert,
                                  const struct public_key *issuer_key);

/* --- Wurzelzertifikate ------------------------------------------------ */

void trust_store_init(void);
/* Nur fuer den Pruefstand: Sammlung zur Laufzeit setzen. */
void trust_store_set(const uint8_t *data, size_t length);
size_t trust_store_count(void);
const struct certificate *trust_store_find_by_index(size_t index);
/* Sucht ein Wurzelzertifikat, dessen Inhaber zum Aussteller passt. */
const struct certificate *trust_store_find(const uint8_t *issuer,
                                           size_t issuer_length);

/* Prueft eine ganze Kette. Der erste Eintrag ist das Zertifikat des
 * Servers. Bei einem Fehler steht der Grund in "reason". */
bool certificate_verify_chain(struct certificate *chain, size_t count,
                              const char *host, uint64_t now,
                              char *reason, size_t reason_size);

#endif /* PKI_H */
