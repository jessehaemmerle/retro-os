/* x509.c - Zertifikate lesen und Ketten pruefen.
 *
 * Ein Zertifikat ist eine unterschriebene Aussage: "dieser oeffentliche
 * Schluessel gehoert zu diesem Namen". Unterschrieben hat es eine andere
 * Stelle, deren Zertifikat wiederum unterschrieben ist - bis hinauf zu
 * einer Wurzel, der wir von vornherein vertrauen.
 *
 * Geprueft wird darum von unten nach oben:
 *   1. passt der Name zum aufgerufenen Rechner?
 *   2. ist jedes Zertifikat zeitlich gueltig?
 *   3. stimmt jede Unterschrift mit dem Schluessel des naechsthoeheren?
 *   4. endet die Kette bei einer Wurzel, die wir kennen?
 * Faellt einer der Punkte durch, kommt keine Verbindung zustande.
 */

#include "pki.h"
#include "crypto.h"
#include "kstring.h"

#define SAN_MAX 24

/* --- Objektkennungen -------------------------------------------------- */

static const uint8_t oid_rsa[]        = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01 };
static const uint8_t oid_rsa_pss[]    = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0a };
static const uint8_t oid_sha256_rsa[] = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0b };
static const uint8_t oid_sha384_rsa[] = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0c };
static const uint8_t oid_sha512_rsa[] = { 0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x0d };
static const uint8_t oid_ec_key[]     = { 0x2a,0x86,0x48,0xce,0x3d,0x02,0x01 };
static const uint8_t oid_p256[]       = { 0x2a,0x86,0x48,0xce,0x3d,0x03,0x01,0x07 };
static const uint8_t oid_p384[]       = { 0x2b,0x81,0x04,0x00,0x22 };
static const uint8_t oid_ecdsa_256[]  = { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x02 };
static const uint8_t oid_ecdsa_384[]  = { 0x2a,0x86,0x48,0xce,0x3d,0x04,0x03,0x03 };
static const uint8_t oid_common_name[] = { 0x55,0x04,0x03 };
static const uint8_t oid_basic_constraints[] = { 0x55,0x1d,0x13 };
static const uint8_t oid_subject_alt_name[]  = { 0x55,0x1d,0x11 };

static bool oid_is(const struct der_value *value, const uint8_t *oid,
                   size_t length)
{
    return value->tag == 0x06 && value->length == length &&
           memcmp(value->content, oid, length) == 0;
}

#define OID_IS(v, name) oid_is((v), name, sizeof(name))

/* --- Zeitangaben ------------------------------------------------------ */

static uint64_t digits(const uint8_t *p, int count)
{
    uint64_t value = 0;

    for (int i = 0; i < count; i++) {
        if (p[i] < '0' || p[i] > '9')
            return 0;
        value = value * 10 + (uint64_t)(p[i] - '0');
    }
    return value;
}

/* Wandelt UTCTime oder GeneralizedTime in JJJJMMTTHHMMSS. */
static uint64_t parse_time(const struct der_value *value)
{
    const uint8_t *p = value->content;

    if (value->tag == 0x17 && value->length >= 13) {
        uint64_t year = digits(p, 2);

        /* Zweistellige Jahre: 50 und darueber meinen das letzte Jahrhundert. */
        year += (year >= 50) ? 1900 : 2000;
        return year * 10000000000ULL + digits(p + 2, 2) * 100000000ULL +
               digits(p + 4, 2) * 1000000ULL + digits(p + 6, 2) * 10000ULL +
               digits(p + 8, 2) * 100ULL + digits(p + 10, 2);
    }

    if (value->tag == 0x18 && value->length >= 15) {
        return digits(p, 4) * 10000000000ULL + digits(p + 4, 2) * 100000000ULL +
               digits(p + 6, 2) * 1000000ULL + digits(p + 8, 2) * 10000ULL +
               digits(p + 10, 2) * 100ULL + digits(p + 12, 2);
    }
    return 0;
}

/* --- Bestandteile ----------------------------------------------------- */

static enum signature_algorithm parse_signature_algorithm(
    const struct der_value *sequence)
{
    struct der inner;
    struct der_value oid;

    if (!der_enter(sequence, &inner) || !der_next(&inner, &oid))
        return SIG_UNKNOWN;

    if (OID_IS(&oid, oid_sha256_rsa)) return SIG_RSA_PKCS1_SHA256;
    if (OID_IS(&oid, oid_sha384_rsa)) return SIG_RSA_PKCS1_SHA384;
    if (OID_IS(&oid, oid_sha512_rsa)) return SIG_RSA_PKCS1_SHA512;
    if (OID_IS(&oid, oid_ecdsa_256))  return SIG_ECDSA_P256_SHA256;
    if (OID_IS(&oid, oid_ecdsa_384))  return SIG_ECDSA_P384_SHA384;

    if (OID_IS(&oid, oid_rsa_pss)) {
        /* Welche Streuwertfunktion, steht in den Parametern; ohne Angabe
         * gilt SHA-1, was hier nicht mehr akzeptiert wird. */
        struct der_value params;

        if (der_next(&inner, &params)) {
            /* Grob nachsehen, ob SHA-384 erwaehnt wird. */
            static const uint8_t sha384_oid[] = {
                0x60,0x86,0x48,0x01,0x65,0x03,0x04,0x02,0x02
            };

            for (size_t i = 0; i + sizeof(sha384_oid) <= params.length; i++) {
                if (memcmp(params.content + i, sha384_oid,
                           sizeof(sha384_oid)) == 0)
                    return SIG_RSA_PSS_SHA384;
            }
        }
        return SIG_RSA_PSS_SHA256;
    }
    return SIG_UNKNOWN;
}

static bool parse_public_key(struct certificate *cert,
                             const struct der_value *spki)
{
    struct der outer, algorithm;
    struct der_value algorithm_value, key_value, oid, params;

    if (!der_enter(spki, &outer))
        return false;
    if (!der_next(&outer, &algorithm_value) || algorithm_value.tag != 0x30)
        return false;
    if (!der_next(&outer, &key_value) || key_value.tag != 0x03)
        return false;

    if (!der_enter(&algorithm_value, &algorithm))
        return false;
    if (!der_next(&algorithm, &oid))
        return false;

    /* Das erste Byte einer BIT STRING sagt, wieviele Bits am Ende ungenutzt
     * sind - bei Schluesseln immer null. */
    const uint8_t *bits = key_value.content + 1;
    size_t bits_length = key_value.length - 1;

    if (OID_IS(&oid, oid_rsa)) {
        struct der key_reader, numbers;
        struct der_value sequence, modulus, exponent;

        der_start(&key_reader, bits, bits_length);
        if (!der_next(&key_reader, &sequence) || sequence.tag != 0x30)
            return false;
        if (!der_enter(&sequence, &numbers))
            return false;
        if (!der_next(&numbers, &modulus) || modulus.tag != 0x02)
            return false;
        if (!der_next(&numbers, &exponent) || exponent.tag != 0x02)
            return false;

        const uint8_t *n = modulus.content;
        size_t n_length = modulus.length;

        while (n_length > 1 && n[0] == 0) {      /* fuehrende Null weg */
            n++;
            n_length--;
        }

        uint64_t e = 0;
        for (size_t i = 0; i < exponent.length && i < 8; i++)
            e = (e << 8) | exponent.content[i];

        if (e < 3 || n_length < 128 || n_length > 512)
            return false;

        cert->key.type = KEY_RSA;
        cert->key.modulus = n;
        cert->key.modulus_length = n_length;
        cert->key.exponent = e;
        return true;
    }

    if (OID_IS(&oid, oid_ec_key)) {
        if (!der_next(&algorithm, &params))
            return false;

        if (OID_IS(&params, oid_p256))
            cert->key.type = KEY_EC_P256;
        else if (OID_IS(&params, oid_p384))
            cert->key.type = KEY_EC_P384;
        else
            return false;

        cert->key.point = bits;
        cert->key.point_length = bits_length;
        return true;
    }

    return false;
}

/* Sucht den allgemeinen Namen (CN) im Namensfeld. */
static void parse_common_name(struct certificate *cert,
                              const struct der_value *name)
{
    struct der rdn_sequence;
    struct der_value rdn;

    if (!der_enter(name, &rdn_sequence))
        return;

    while (der_next(&rdn_sequence, &rdn)) {
        struct der attributes;
        struct der_value attribute;

        if (!der_enter(&rdn, &attributes))
            continue;

        while (der_next(&attributes, &attribute)) {
            struct der pair;
            struct der_value oid, value;

            if (!der_enter(&attribute, &pair))
                continue;
            if (!der_next(&pair, &oid) || !der_next(&pair, &value))
                continue;

            if (OID_IS(&oid, oid_common_name) && value.length < CERT_NAME_MAX) {
                memcpy(cert->common_name, value.content, value.length);
                cert->common_name[value.length] = '\0';
            }
        }
    }
}

/* Alternative Namen und die Angabe, ob es eine Zertifizierungsstelle ist. */
struct san_entry {
    const uint8_t *name;
    size_t         length;
};

static struct san_entry san_list[SAN_MAX];
static size_t           san_count;
static const struct certificate *san_owner;

static void parse_extensions(struct certificate *cert,
                             const struct der_value *extensions)
{
    struct der outer, list;
    struct der_value wrapper, extension;

    if (!der_enter(extensions, &outer))
        return;
    if (!der_next(&outer, &wrapper) || wrapper.tag != 0x30)
        return;
    if (!der_enter(&wrapper, &list))
        return;

    while (der_next(&list, &extension)) {
        struct der fields;
        struct der_value oid, value;

        if (!der_enter(&extension, &fields))
            continue;
        if (!der_next(&fields, &oid))
            continue;

        /* Das Merkmal "kritisch" kann dazwischenstehen. */
        if (!der_next(&fields, &value))
            continue;
        if (value.tag == 0x01 && !der_next(&fields, &value))
            continue;
        if (value.tag != 0x04)
            continue;

        struct der content;
        struct der_value inner;

        der_start(&content, value.content, value.length);

        if (OID_IS(&oid, oid_basic_constraints)) {
            cert->has_basic_constraints = true;

            if (der_next(&content, &inner) && inner.tag == 0x30) {
                struct der constraints;
                struct der_value ca;

                if (der_enter(&inner, &constraints) &&
                    der_next(&constraints, &ca) && ca.tag == 0x01)
                    cert->is_ca = ca.length > 0 && ca.content[0] != 0;
            }
        } else if (OID_IS(&oid, oid_subject_alt_name)) {
            if (!der_next(&content, &inner) || inner.tag != 0x30)
                continue;

            struct der names;
            struct der_value entry;

            if (!der_enter(&inner, &names))
                continue;

            san_owner = cert;
            san_count = 0;

            while (der_next(&names, &entry) && san_count < SAN_MAX) {
                /* [2] IMPLICIT IA5String ist ein Rechnername. */
                if (entry.tag == 0x82) {
                    san_list[san_count].name = entry.content;
                    san_list[san_count].length = entry.length;
                    san_count++;
                }
            }
        }
    }
}

bool certificate_parse(struct certificate *cert, const uint8_t *data,
                       size_t length)
{
    struct der reader, body, fields;
    struct der_value certificate, tbs, value;

    memset(cert, 0, sizeof(*cert));
    cert->raw = data;
    cert->raw_length = length;

    der_start(&reader, data, length);
    if (!der_next(&reader, &certificate) || certificate.tag != 0x30)
        return false;
    if (!der_enter(&certificate, &body))
        return false;

    /* --- der unterschriebene Teil --- */
    if (!der_next(&body, &tbs) || tbs.tag != 0x30)
        return false;

    cert->tbs = tbs.full;
    cert->tbs_length = tbs.full_length;

    if (!der_enter(&tbs, &fields))
        return false;
    if (!der_next(&fields, &value))
        return false;

    /* Die Versionsangabe ist wahlfrei und mit [0] gekennzeichnet. */
    if (value.tag == 0xA0) {
        if (!der_next(&fields, &value))
            return false;
    }

    /* value ist jetzt die Seriennummer. */
    if (!der_next(&fields, &value))       /* Unterschriftsverfahren */
        return false;

    struct der_value issuer;
    if (!der_next(&fields, &issuer) || issuer.tag != 0x30)
        return false;
    cert->issuer = issuer.full;
    cert->issuer_length = issuer.full_length;

    /* --- Gueltigkeit --- */
    struct der_value validity;
    if (!der_next(&fields, &validity) || validity.tag != 0x30)
        return false;
    {
        struct der times;
        struct der_value from, to;

        if (!der_enter(&validity, &times))
            return false;
        if (!der_next(&times, &from) || !der_next(&times, &to))
            return false;

        cert->not_before = parse_time(&from);
        cert->not_after = parse_time(&to);
    }

    struct der_value subject;
    if (!der_next(&fields, &subject) || subject.tag != 0x30)
        return false;
    cert->subject = subject.full;
    cert->subject_length = subject.full_length;
    parse_common_name(cert, &subject);

    struct der_value spki;
    if (!der_next(&fields, &spki) || spki.tag != 0x30)
        return false;
    if (!parse_public_key(cert, &spki))
        return false;

    /* Danach koennen wahlfreie Felder folgen; uns interessiert [3]. */
    while (der_next(&fields, &value)) {
        if (value.tag == 0xA3)
            parse_extensions(cert, &value);
    }

    /* --- Verfahren und Wert der Unterschrift --- */
    struct der_value algorithm, signature;

    if (!der_next(&body, &algorithm) || algorithm.tag != 0x30)
        return false;
    cert->signature_algorithm = parse_signature_algorithm(&algorithm);

    if (!der_next(&body, &signature) || signature.tag != 0x03)
        return false;
    if (signature.length < 2)
        return false;

    cert->signature = signature.content + 1;      /* das Fuellbyte weg */
    cert->signature_length = signature.length - 1;

    return true;
}

/* --- Namensvergleich -------------------------------------------------- */

static bool name_matches(const char *pattern, size_t pattern_length,
                         const char *host)
{
    size_t host_length = strlen(host);

    /* Platzhalter deckt genau eine Ebene ab: *.beispiel.de */
    if (pattern_length > 2 && pattern[0] == '*' && pattern[1] == '.') {
        const char *dot = strchr(host, '.');

        if (!dot)
            return false;

        size_t rest_length = pattern_length - 1;      /* ohne den Stern */
        size_t host_rest = host_length - (size_t)(dot - host);

        if (rest_length != host_rest)
            return false;
        return strncasecmp(pattern + 1, dot, rest_length) == 0;
    }

    if (pattern_length != host_length)
        return false;
    return strncasecmp(pattern, host, host_length) == 0;
}

bool certificate_matches_host(const struct certificate *cert, const char *host)
{
    /* Wenn alternative Namen vorhanden sind, zaehlen nur diese. */
    if (san_owner == cert && san_count > 0) {
        for (size_t i = 0; i < san_count; i++) {
            if (name_matches((const char *)san_list[i].name,
                             san_list[i].length, host))
                return true;
        }
        return false;
    }

    if (cert->common_name[0])
        return name_matches(cert->common_name, strlen(cert->common_name), host);

    return false;
}

/* --- Unterschrift eines Zertifikats ----------------------------------- */

bool certificate_verify_signature(const struct certificate *cert,
                                  const struct public_key *issuer_key)
{
    uint8_t hash[SHA512_SIZE];
    size_t hash_length;

    switch (cert->signature_algorithm) {
    case SIG_RSA_PKCS1_SHA256:
    case SIG_RSA_PSS_SHA256:
    case SIG_ECDSA_P256_SHA256:
        sha256(cert->tbs, cert->tbs_length, hash);
        hash_length = SHA256_SIZE;
        break;
    case SIG_RSA_PKCS1_SHA384:
    case SIG_RSA_PSS_SHA384:
    case SIG_ECDSA_P384_SHA384:
        sha384(cert->tbs, cert->tbs_length, hash);
        hash_length = SHA384_SIZE;
        break;
    case SIG_RSA_PKCS1_SHA512:
        sha512(cert->tbs, cert->tbs_length, hash);
        hash_length = SHA512_SIZE;
        break;
    default:
        return false;
    }

    switch (cert->signature_algorithm) {
    case SIG_RSA_PKCS1_SHA256:
    case SIG_RSA_PKCS1_SHA384:
    case SIG_RSA_PKCS1_SHA512:
        return issuer_key->type == KEY_RSA &&
               rsa_verify_pkcs1(issuer_key, hash, hash_length,
                                cert->signature, cert->signature_length);

    case SIG_RSA_PSS_SHA256:
    case SIG_RSA_PSS_SHA384:
        return issuer_key->type == KEY_RSA &&
               rsa_verify_pss(issuer_key, hash, hash_length,
                              cert->signature, cert->signature_length);

    case SIG_ECDSA_P256_SHA256:
        return issuer_key->type == KEY_EC_P256 &&
               issuer_key->point_length == 65 &&
               ecdsa_p256_verify(issuer_key->point, hash, hash_length,
                                 cert->signature, cert->signature_length);

    default:
        /* P-384 wird nicht unterstuetzt - lieber ablehnen als vorgeben,
         * geprueft zu haben. */
        return false;
    }
}
