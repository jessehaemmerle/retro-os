/* crypto_test.c - Testvektoren fuer die selbst geschriebene Kryptografie.
 *
 * Jeder Fall stammt aus der Spezifikation des jeweiligen Verfahrens. Faellt
 * einer davon durch, stimmt die Rechnung nicht - und dann hilft es nichts,
 * wenn eine Verbindung "irgendwie" zustande kommt.
 */

#include "crypto.h"
#include "bignum.h"
#include "pki.h"

#include "signature_vectors.h"
#include "chain_vectors.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

static void hex_to_bytes(const char *hex, uint8_t *out, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        unsigned value;

        sscanf(hex + i * 2, "%2x", &value);
        out[i] = (uint8_t)value;
    }
}

static void show(const char *label, const uint8_t *data, size_t length)
{
    printf("    %s: ", label);
    for (size_t i = 0; i < length; i++)
        printf("%02x", data[i]);
    printf("\n");
}

static void check(const char *name, const uint8_t *got, const char *expected_hex,
                  size_t length)
{
    uint8_t expected[512];

    checks++;
    hex_to_bytes(expected_hex, expected, length);

    if (memcmp(got, expected, length) == 0) {
        printf("  ok    %s\n", name);
        return;
    }

    printf("  FEHLER %s\n", name);
    show("erwartet", expected, length);
    show("erhalten", got, length);
    failures++;
}

static void check_true(const char *name, bool condition)
{
    checks++;
    if (condition) {
        printf("  ok    %s\n", name);
    } else {
        printf("  FEHLER %s\n", name);
        failures++;
    }
}

/* --- SHA-2 (FIPS 180-4) ---------------------------------------------- */

static void test_sha(void)
{
    uint8_t out[64];

    printf("SHA-2\n");

    sha256("abc", 3, out);
    check("SHA-256(\"abc\")", out,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32);

    sha256("", 0, out);
    check("SHA-256(\"\")", out,
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", 32);

    const char *long_input =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(long_input, strlen(long_input), out);
    check("SHA-256(56 Zeichen)", out,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 32);

    sha384("abc", 3, out);
    check("SHA-384(\"abc\")", out,
          "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed"
          "8086072ba1e7cc2358baeca134c825a7", 48);

    sha512("abc", 3, out);
    check("SHA-512(\"abc\")", out,
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", 64);
}

/* --- HMAC (RFC 4231) und HKDF (RFC 5869) ----------------------------- */

static void test_hmac_hkdf(void)
{
    uint8_t key[64], out[64], prk[32];

    printf("HMAC und HKDF\n");

    memset(key, 0x0b, 20);
    hmac_sha256(key, 20, "Hi There", 8, out);
    check("HMAC-SHA256 Fall 1", out,
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32);

    hmac_sha256((const uint8_t *)"Jefe", 4,
                "what do ya want for nothing?", 28, out);
    check("HMAC-SHA256 Fall 2", out,
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);

    uint8_t ikm[22], salt[13], info[10];

    memset(ikm, 0x0b, sizeof(ikm));
    hex_to_bytes("000102030405060708090a0b0c", salt, sizeof(salt));
    hex_to_bytes("f0f1f2f3f4f5f6f7f8f9", info, sizeof(info));

    hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
    check("HKDF-Extract Fall 1", prk,
          "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5", 32);

    hkdf_expand(prk, info, sizeof(info), out, 42);
    check("HKDF-Expand Fall 1", out,
          "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
          "34007208d5b887185865", 42);
}

/* --- ChaCha20 und Poly1305 (RFC 8439) -------------------------------- */

static void test_chacha(void)
{
    uint8_t key[32], nonce[12], out[256];

    printf("ChaCha20 und Poly1305\n");

    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)i;
    hex_to_bytes("000000090000004a00000000", nonce, 12);

    chacha20_block(key, nonce, 1, out);
    check("ChaCha20 Block (2.3.2)", out,
          "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
          "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", 64);

    const char *plain =
        "Ladies and Gentlemen of the class of '99: If I could offer you "
        "only one tip for the future, sunscreen would be it.";

    hex_to_bytes("000000000000004a00000000", nonce, 12);
    chacha20_xor(key, nonce, 1, (const uint8_t *)plain, out, strlen(plain));
    check("ChaCha20 Verschluesselung (2.4.2)", out,
          "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0b"
          "f91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d8"
          "07ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab7793736"
          "5af90bbf74a35be6b40b8eedf2785e42874d", 114);

    uint8_t poly_key[32];
    hex_to_bytes("85d6be7857556d337f4452fe42d506a8"
                 "0103808afb0db2fd4abff6af4149f51b", poly_key, 32);
    const char *message = "Cryptographic Forum Research Group";

    poly1305(poly_key, message, strlen(message), out);
    check("Poly1305 (2.5.2)", out, "a8061dc1305136c6c22b8baf0c0127a9", 16);
}

static void test_aead(void)
{
    uint8_t key[32], nonce[12], aad[12], cipher[256], tag[16], plain[256];

    printf("ChaCha20-Poly1305\n");

    const char *text =
        "Ladies and Gentlemen of the class of '99: If I could offer you "
        "only one tip for the future, sunscreen would be it.";

    for (int i = 0; i < 32; i++)
        key[i] = (uint8_t)(0x80 + i);
    hex_to_bytes("070000004041424344454647", nonce, 12);
    hex_to_bytes("50515253c0c1c2c3c4c5c6c7", aad, 12);

    chacha20poly1305_seal(key, nonce, aad, 12, (const uint8_t *)text,
                          strlen(text), cipher, tag);

    check("AEAD Geheimtext (2.8.2)", cipher,
          "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
          "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
          "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
          "3ff4def08e4b7a9de576d26586cec64b6116", 114);
    check("AEAD Nachweis (2.8.2)", tag, "1ae10b594f09e26a7e902ecbd0600691", 16);

    check_true("AEAD Entschluesselung",
               chacha20poly1305_open(key, nonce, aad, 12, cipher, strlen(text),
                                     tag, plain) &&
               memcmp(plain, text, strlen(text)) == 0);

    tag[0] ^= 1;
    check_true("AEAD erkennt Veraenderung",
               !chacha20poly1305_open(key, nonce, aad, 12, cipher, strlen(text),
                                      tag, plain));
}

/* --- AES (FIPS 197) und GCM (NIST) ----------------------------------- */

static void test_aes(void)
{
    struct aes_key key;
    uint8_t material[32], block[16], out[64], tag[16], nonce[12];

    printf("AES und GCM\n");

    hex_to_bytes("000102030405060708090a0b0c0d0e0f", material, 16);
    hex_to_bytes("00112233445566778899aabbccddeeff", block, 16);
    aes_set_key(&key, material, 128);
    aes_encrypt_block(&key, block, out);
    check("AES-128 (FIPS 197)", out, "69c4e0d86a7b0430d8cdb78070b4c55a", 16);

    hex_to_bytes("000102030405060708090a0b0c0d0e0f"
                 "101112131415161718191a1b1c1d1e1f", material, 32);
    aes_set_key(&key, material, 256);
    aes_encrypt_block(&key, block, out);
    check("AES-256 (FIPS 197)", out, "8ea2b7ca516745bfeafc49904b496089", 16);

    /* NIST GCM, Fall 1: alles Null, kein Klartext. */
    memset(material, 0, 16);
    memset(nonce, 0, 12);
    aes_set_key(&key, material, 128);
    aes_gcm_seal(&key, nonce, NULL, 0, NULL, 0, out, tag);
    check("AES-128-GCM Fall 1 (Nachweis)", tag,
          "58e2fccefa7e3061367f1d57a4e7455a", 16);

    /* Fall 2: 16 Null-Bytes Klartext. */
    uint8_t zeros[16];
    memset(zeros, 0, 16);
    aes_gcm_seal(&key, nonce, NULL, 0, zeros, 16, out, tag);
    check("AES-128-GCM Fall 2 (Geheimtext)", out,
          "0388dace60b6a392f328c2b971b2fe78", 16);
    check("AES-128-GCM Fall 2 (Nachweis)", tag,
          "ab6e47d42cec13bdf53a67b21257bddf", 16);

    uint8_t plain[16];
    check_true("AES-GCM Entschluesselung",
               aes_gcm_open(&key, nonce, NULL, 0, out, 16, tag, plain) &&
               memcmp(plain, zeros, 16) == 0);

    tag[3] ^= 0x10;
    check_true("AES-GCM erkennt Veraenderung",
               !aes_gcm_open(&key, nonce, NULL, 0, out, 16, tag, plain));
}

/* --- X25519 (RFC 7748) ------------------------------------------------ */

static void test_x25519(void)
{
    uint8_t scalar[32], point[32], out[32];

    printf("X25519\n");

    hex_to_bytes("a546e36bf0527c9d3b16154b82465edd"
                 "62144c0ac1fc5a18506a2244ba449ac4", scalar, 32);
    hex_to_bytes("e6db6867583030db3594c1a424b15f7c"
                 "726624ec26b3353b10a903a6d0ab1c4c", point, 32);
    x25519(out, scalar, point);
    check("X25519 Einzelschritt (5.2)", out,
          "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);

    uint8_t alice_secret[32], bob_secret[32];
    uint8_t alice_public[32], bob_public[32], shared_a[32], shared_b[32];

    hex_to_bytes("77076d0a7318a57d3c16c17251b26645"
                 "df4c2f87ebc0992ab177fba51db92c2a", alice_secret, 32);
    hex_to_bytes("5dab087e624a8a4b79e17f8b83800ee6"
                 "6f3bb1292618b6fd1c2f8b27ff88e0eb", bob_secret, 32);

    x25519_base(alice_public, alice_secret);
    check("X25519 oeffentlicher Teil A (6.1)", alice_public,
          "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32);

    x25519_base(bob_public, bob_secret);
    check("X25519 oeffentlicher Teil B (6.1)", bob_public,
          "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);

    x25519(shared_a, alice_secret, bob_public);
    x25519(shared_b, bob_secret, alice_public);
    check("X25519 gemeinsames Geheimnis (6.1)", shared_a,
          "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", 32);
    check_true("X25519 beide Seiten gleich",
               memcmp(shared_a, shared_b, 32) == 0);
}

/* --- grosse Zahlen ---------------------------------------------------- */

static void test_bignum(void)
{
    struct bignum base, modulus, result;
    uint8_t out[32];

    printf("Grosse Zahlen\n");

    /* 5^65537 mod 2^127-1, gegengerechnet mit Python. */
    uint8_t base_bytes[1] = { 5 };
    uint8_t modulus_bytes[16] = {
        0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };

    bn_from_bytes(&base, base_bytes, 1);
    bn_from_bytes(&modulus, modulus_bytes, 16);
    check_true("modexp laeuft", bn_modexp(&result, &base, 65537, &modulus));
    bn_to_bytes(&result, out, 16);
    check("5^65537 mod (2^127-1)", out, "5c41ceba82c7815c580db9e3e4d0d476", 16);

    /* Hin und zurueck durch die Bytedarstellung. */
    uint8_t sample[32];
    for (int i = 0; i < 32; i++)
        sample[i] = (uint8_t)(i * 7 + 1);

    struct bignum roundtrip;
    bn_from_bytes(&roundtrip, sample, 32);
    bn_to_bytes(&roundtrip, out, 32);
    check_true("Bytes hin und zurueck", memcmp(out, sample, 32) == 0);
}

/* --- Unterschriften (mit OpenSSL erzeugt) ----------------------------- */

static void test_signatures(void)
{
    uint8_t hash[32];
    struct public_key key;

    printf("Unterschriften\n");

    sha256(signed_message, strlen(signed_message), hash);

    memset(&key, 0, sizeof(key));
    key.type = KEY_RSA;
    key.modulus = rsa_modulus;
    key.modulus_length = sizeof(rsa_modulus);
    key.exponent = 65537;

    check_true("RSA PKCS#1 v1.5 wird angenommen",
               rsa_verify_pkcs1(&key, hash, 32, rsa_sig_pkcs1,
                                sizeof(rsa_sig_pkcs1)));
    check_true("RSA PSS wird angenommen",
               rsa_verify_pss(&key, hash, 32, rsa_sig_pss,
                              sizeof(rsa_sig_pss)));

    /* Ein veraenderter Streuwert darf nicht durchgehen. */
    uint8_t wrong[32];
    memcpy(wrong, hash, 32);
    wrong[0] ^= 1;

    check_true("RSA PKCS#1 erkennt falschen Streuwert",
               !rsa_verify_pkcs1(&key, wrong, 32, rsa_sig_pkcs1,
                                 sizeof(rsa_sig_pkcs1)));
    check_true("RSA PSS erkennt falschen Streuwert",
               !rsa_verify_pss(&key, wrong, 32, rsa_sig_pss,
                               sizeof(rsa_sig_pss)));

    /* Eine veraenderte Unterschrift ebenfalls nicht. */
    uint8_t broken[256];
    memcpy(broken, rsa_sig_pkcs1, sizeof(rsa_sig_pkcs1));
    broken[100] ^= 0x40;
    check_true("RSA erkennt veraenderte Unterschrift",
               !rsa_verify_pkcs1(&key, hash, 32, broken,
                                 sizeof(rsa_sig_pkcs1)));

    check_true("ECDSA P-256 wird angenommen",
               ecdsa_p256_verify(ec_point, hash, 32, ec_signature,
                                 sizeof(ec_signature)));
    check_true("ECDSA erkennt falschen Streuwert",
               !ecdsa_p256_verify(ec_point, wrong, 32, ec_signature,
                                  sizeof(ec_signature)));

    uint8_t bad_point[65];
    memcpy(bad_point, ec_point, 65);
    bad_point[10] ^= 1;
    check_true("ECDSA erkennt Punkt ausserhalb der Kurve",
               !ecdsa_p256_verify(bad_point, hash, 32, ec_signature,
                                  sizeof(ec_signature)));
}

/* --- Zertifikate und Ketten ------------------------------------------- */

/* Ein Vorrat, der genau unsere Testwurzel enthaelt. */
static uint8_t trust_blob[4096];

static void build_trust_store(void)
{
    size_t length = sizeof(chain_root);

    trust_blob[0] = (uint8_t)(length >> 24);
    trust_blob[1] = (uint8_t)(length >> 16);
    trust_blob[2] = (uint8_t)(length >> 8);
    trust_blob[3] = (uint8_t)length;
    memcpy(trust_blob + 4, chain_root, length);

    trust_store_set(trust_blob, length + 4);
    trust_store_init();
}

static void test_certificates(void)
{
    struct certificate chain[3];
    char reason[160];

    printf("Zertifikate\n");

    check_true("Serverzertifikat wird gelesen",
               certificate_parse(&chain[0], chain_server, sizeof(chain_server)));
    check_true("Zwischenstelle wird gelesen",
               certificate_parse(&chain[1], chain_intermediate,
                                 sizeof(chain_intermediate)));
    check_true("Wurzel wird gelesen",
               certificate_parse(&chain[2], chain_root, sizeof(chain_root)));

    check_true("Name des Servers erkannt",
               strcmp(chain[0].common_name, "beispiel.retroos") == 0);
    check_true("Zwischenstelle ist als solche gekennzeichnet", chain[1].is_ca);
    check_true("Server ist keine Zertifizierungsstelle", !chain[0].is_ca);
    check_true("Schluesselart des Servers ist P-256",
               chain[0].key.type == KEY_EC_P256);
    check_true("Schluesselart der Wurzel ist RSA",
               chain[2].key.type == KEY_RSA);

    check_true("Unterschrift des Servers stimmt",
               certificate_verify_signature(&chain[0], &chain[1].key));
    check_true("Unterschrift der Zwischenstelle stimmt",
               certificate_verify_signature(&chain[1], &chain[2].key));

    build_trust_store();
    check_true("Wurzel liegt im Vorrat", trust_store_count() == 1);

    /* Nur Server und Zwischenstelle schicken - die Wurzel kennen wir. */
    struct certificate two[2];
    certificate_parse(&two[0], chain_server, sizeof(chain_server));
    certificate_parse(&two[1], chain_intermediate, sizeof(chain_intermediate));

    reason[0] = '\0';
    bool accepted = certificate_verify_chain(two, 2, "beispiel.retroos",
                                             20261001000000ULL, reason,
                                             sizeof(reason));
    if (!accepted)
        printf("    Grund: %s\n", reason);
    check_true("vollstaendige Kette wird angenommen", accepted);

    certificate_parse(&two[0], chain_server, sizeof(chain_server));
    certificate_parse(&two[1], chain_intermediate, sizeof(chain_intermediate));
    check_true("Platzhalter im Namen wird beachtet",
               certificate_verify_chain(two, 2, "www.beispiel.retroos",
                                        20261001000000ULL, reason,
                                        sizeof(reason)));

    certificate_parse(&two[0], chain_server, sizeof(chain_server));
    certificate_parse(&two[1], chain_intermediate, sizeof(chain_intermediate));
    check_true("falscher Rechnername wird abgelehnt",
               !certificate_verify_chain(two, 2, "boese.example",
                                         20261001000000ULL, reason,
                                         sizeof(reason)));

    certificate_parse(&two[0], chain_server, sizeof(chain_server));
    certificate_parse(&two[1], chain_intermediate, sizeof(chain_intermediate));
    check_true("abgelaufenes Zertifikat wird abgelehnt",
               !certificate_verify_chain(two, 2, "beispiel.retroos",
                                         20400101000000ULL, reason,
                                         sizeof(reason)));

    /* Ohne die Zwischenstelle fehlt das Bindeglied zur Wurzel. */
    struct certificate alone[1];
    certificate_parse(&alone[0], chain_server, sizeof(chain_server));
    check_true("unvollstaendige Kette wird abgelehnt",
               !certificate_verify_chain(alone, 1, "beispiel.retroos",
                                         20261001000000ULL, reason,
                                         sizeof(reason)));

    /* Ein veraendertes Zertifikat darf nicht durchgehen. */
    static uint8_t tampered[4096];
    memcpy(tampered, chain_server, sizeof(chain_server));
    tampered[sizeof(chain_server) / 2] ^= 0x20;

    struct certificate bad[2];
    if (certificate_parse(&bad[0], tampered, sizeof(chain_server))) {
        certificate_parse(&bad[1], chain_intermediate,
                          sizeof(chain_intermediate));
        check_true("veraendertes Zertifikat wird abgelehnt",
                   !certificate_verify_chain(bad, 2, "beispiel.retroos",
                                             20261001000000ULL, reason,
                                             sizeof(reason)));
    } else {
        check_true("veraendertes Zertifikat wird abgelehnt", true);
    }
}

/* Prueft, dass die echte Sammlung gelesen werden kann und die Wurzeln
 * ihre eigene Unterschrift tragen. */
static void test_real_trust_store(void)
{
    printf("Echter Wurzelspeicher\n");

    FILE *file = fopen("../data/wurzelzertifikate.der", "rb");

    if (!file) {
        printf("  (uebersprungen - data/wurzelzertifikate.der fehlt)\n");
        return;
    }

    static uint8_t blob[1 << 20];
    size_t length = fread(blob, 1, sizeof(blob), file);
    fclose(file);

    trust_store_set(blob, length);
    trust_store_init();

    check_true("mehr als hundert Wurzeln gelesen", trust_store_count() > 100);

    /* Wurzeln sind selbst unterschrieben. Geprueft werden die, deren
     * Verfahren RetroOS ueberhaupt unterstuetzt - aeltere Wurzeln mit
     * SHA-1 oder P-384 werden bewusst abgelehnt. */
    size_t supported = 0, good = 0, unsupported = 0;

    for (size_t i = 0; i < trust_store_count(); i++) {
        const struct certificate *root = trust_store_find_by_index(i);

        if (!root)
            continue;
        if (root->issuer_length != root->subject_length ||
            memcmp(root->issuer, root->subject, root->issuer_length) != 0)
            continue;

        bool can_check =
            (root->signature_algorithm == SIG_RSA_PKCS1_SHA256 ||
             root->signature_algorithm == SIG_RSA_PKCS1_SHA384 ||
             root->signature_algorithm == SIG_RSA_PKCS1_SHA512 ||
             root->signature_algorithm == SIG_RSA_PSS_SHA256 ||
             root->signature_algorithm == SIG_RSA_PSS_SHA384 ||
             root->signature_algorithm == SIG_ECDSA_P256_SHA256) &&
            (root->key.type == KEY_RSA || root->key.type == KEY_EC_P256);

        if (!can_check) {
            unsupported++;
            continue;
        }

        supported++;
        if (certificate_verify_signature(root, &root->key))
            good++;
        else
            printf("    nicht bestaetigt: %s\n", root->common_name);
    }

    printf("    %zu von %zu unterstuetzten Wurzeln bestaetigen sich selbst"
           " (%zu mit anderen Verfahren)\n", good, supported, unsupported);
    check_true("alle unterstuetzten Wurzeln sind in sich stimmig",
               supported > 20 && good == supported);
}

/* --- Der Schluesselfahrplan von TLS 1.3 (RFC 8448) --------------------
 *
 * Der offizielle Mitschnitt eines vollstaendigen Handschlags. Stimmen die
 * abgeleiteten Werte damit ueberein, rechnet RetroOS genau so, wie es die
 * Spezifikation vorsieht - und nur dann kann eine echte Verbindung
 * zustande kommen.
 */

static void derive_secret_test(const uint8_t secret[32], const char *label,
                               const uint8_t hash[32], uint8_t out[32])
{
    hkdf_expand_label(secret, label, hash, 32, out, 32);
}

static void test_tls_schedule(void)
{
    uint8_t shared[32], empty_hash[32], hello_hash[32];
    uint8_t early[32], derived[32], handshake[32], secret[32];
    uint8_t key[16], iv[12];

    printf("Schluesselfahrplan von TLS 1.3\n");

    hex_to_bytes("8bd4054fb55b9d63fdfbacf9f04b9f0d"
                 "35e6d63f537563efd46272900f89492d", shared, 32);
    hex_to_bytes("e3b0c44298fc1c149afbf4c8996fb924"
                 "27ae41e4649b934ca495991b7852b855", empty_hash, 32);
    hex_to_bytes("860c06edc07858ee8e78f0e7428c58ed"
                 "d6b43f2ca3e6e95f02ed063cf0e1cad8", hello_hash, 32);

    static const uint8_t zero_key[32] = { 0 };

    hkdf_extract(NULL, 0, zero_key, sizeof(zero_key), early);
    check("Early Secret", early,
          "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a", 32);

    derive_secret_test(early, "derived", empty_hash, derived);
    check("Derived Secret", derived,
          "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba", 32);

    hkdf_extract(derived, 32, shared, 32, handshake);
    check("Handshake Secret", handshake,
          "1dc826e93606aa6fdc0aadc12f741b01046aa6b99f691ed221a9f0ca043fbeac", 32);

    derive_secret_test(handshake, "c hs traffic", hello_hash, secret);
    check("Schluessel des Clients", secret,
          "b3eddb126e067f35a780b3abf45e2d8f3b1a950738f52e9600746a0e27a55a21", 32);

    derive_secret_test(handshake, "s hs traffic", hello_hash, secret);
    check("Schluessel des Servers", secret,
          "b67b7d690cc16c4e75e54213cb2d37b4e9c912bcded9105d42befd59d391ad38", 32);

    /* Aus dem Geheimnis des Servers werden Schluessel und Nonce gebildet. */
    hkdf_expand_label(secret, "key", NULL, 0, key, sizeof(key));
    check("Schluessel fuer AES-128-GCM", key,
          "3fce516009c21727d0f2e4e86ee403bc", 16);

    hkdf_expand_label(secret, "iv", NULL, 0, iv, sizeof(iv));
    check("Nonce-Anteil", iv, "5d313eb2671276ee13000b30", 12);

    /* Und weiter zum Geheimnis fuer die Nutzdaten. */
    uint8_t derived2[32], master[32];
    uint8_t finished_hash[32];

    derive_secret_test(handshake, "derived", empty_hash, derived2);
    hkdf_extract(derived2, 32, zero_key, sizeof(zero_key), master);
    check("Master Secret", master,
          "18df06843d13a08bf2a449844c5f8a478001bc4d4c627984d5a41da8d0402919", 32);

    hex_to_bytes("9608102a0f1ccc6db6250b7b7e417b1a"
                 "000eaada3daae4777a7686c9ff83df13", finished_hash, 32);
    derive_secret_test(master, "c ap traffic", finished_hash, secret);
    check("Nutzdaten-Schluessel des Clients", secret,
          "9e40646ce79a7f9dc05af8889bce6552875afa0b06df0087f792ebb7c17504a5", 32);

    derive_secret_test(master, "s ap traffic", finished_hash, secret);
    check("Nutzdaten-Schluessel des Servers", secret,
          "a11af9f05531f856ad47116b45a950328204b4f44bfb6b3a4b4f1f3fcb631643", 32);
}

int main(void)
{
    printf("Pruefstand fuer die Kryptografie von RetroOS\n");
    printf("===========================================\n\n");

    test_sha();
    test_hmac_hkdf();
    test_chacha();
    test_aead();
    test_aes();
    test_x25519();
    test_bignum();
    test_signatures();
    test_certificates();
    test_real_trust_store();
    test_tls_schedule();

    printf("\n%d Pruefungen, %d Fehler\n", checks, failures);
    return failures ? 1 : 0;
}
