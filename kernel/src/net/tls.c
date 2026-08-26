/* tls.c - der Handschlag und die Datenuebertragung nach TLS 1.3.
 *
 * Ablauf einer Verbindung:
 *
 *   Client                                Server
 *     |-- ClientHello (mit X25519-Anteil) --->|
 *     |<-- ServerHello (mit X25519-Anteil) ---|
 *     |        ab hier ist alles verschluesselt
 *     |<-- EncryptedExtensions ---------------|
 *     |<-- Certificate -----------------------|
 *     |<-- CertificateVerify -----------------|
 *     |<-- Finished --------------------------|
 *     |-- Finished ------------------------->|
 *     |        ab hier laufen die Nutzdaten
 *
 * Aus dem gemeinsamen Geheimnis des Schluesseltauschs und dem Verlauf
 * aller bisherigen Nachrichten werden die Schluessel abgeleitet. Weil der
 * Verlauf mit einfliesst, kann niemand unbemerkt eine Nachricht aendern,
 * hinzufuegen oder weglassen.
 *
 * Geprueft wird beides: die Kette der Zertifikate bis zu einer bekannten
 * Wurzel und die Unterschrift, mit der der Server beweist, dass ihm der
 * Schluessel darin auch gehoert.
 */

#include "tls.h"
#include "arch.h"
#include "crypto.h"
#include "kstring.h"
#include "mm.h"
#include "net.h"
#include "pki.h"
#include "rtc.h"
#include "thread.h"

#define TLS_RECORD_MAX     16640          /* 16 KiB Nutzlast plus Zugabe */
#define TLS_TRANSCRIPT_MAX (48 * 1024)
#define TLS_CHAIN_MAX      6
#define TLS_HANDSHAKE_MS   12000

/* Nachrichtenarten */
#define REC_CHANGE_CIPHER  20
#define REC_ALERT          21
#define REC_HANDSHAKE      22
#define REC_APPLICATION    23

#define HS_CLIENT_HELLO        1
#define HS_SERVER_HELLO        2
#define HS_NEW_SESSION_TICKET  4
#define HS_ENCRYPTED_EXTENSIONS 8
#define HS_CERTIFICATE        11
#define HS_CERTIFICATE_VERIFY 15
#define HS_FINISHED           20

/* Verfahren */
#define SUITE_AES_128_GCM     0x1301
#define SUITE_CHACHA20        0x1303

enum cipher_kind { CIPHER_AES_GCM, CIPHER_CHACHA };

struct key_material {
    uint8_t key[32];
    uint8_t iv[12];
    uint64_t sequence;
    struct aes_key aes;
};

struct tls_connection {
    struct tcp_socket *socket;

    enum cipher_kind cipher;
    size_t           key_length;

    struct key_material send_keys;
    struct key_material receive_keys;

    uint8_t client_secret[SHA256_SIZE];
    uint8_t server_secret[SHA256_SIZE];

    /* Rohdaten, die noch nicht zu einem vollstaendigen Datensatz reichen */
    uint8_t  input[TLS_RECORD_MAX + 512];
    uint32_t input_length;

    /* Entschluesselte Nutzdaten, die der Aufrufer noch nicht geholt hat */
    uint8_t  plain[TLS_RECORD_MAX];
    uint32_t plain_length;
    uint32_t plain_read;

    bool established;
    bool closed;

    char description[64];
};

/* Der Verlauf aller Handschlag-Nachrichten, fuer die Ableitung noetig. */
static uint8_t  transcript[TLS_TRANSCRIPT_MAX];
static uint32_t transcript_length;

/* ------------------------------------------------------------------ */
/* Kleinkram                                                           */
/* ------------------------------------------------------------------ */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void transcript_add(const uint8_t *data, uint32_t length)
{
    if (transcript_length + length > TLS_TRANSCRIPT_MAX)
        return;

    memcpy(transcript + transcript_length, data, length);
    transcript_length += length;
}

static void transcript_hash(uint8_t out[SHA256_SIZE])
{
    sha256(transcript, transcript_length, out);
}

/* Derive-Secret aus RFC 8446: Ableitung mit dem Verlauf als Zusatz. */
static void derive_secret(const uint8_t secret[SHA256_SIZE], const char *label,
                          const uint8_t hash[SHA256_SIZE],
                          uint8_t out[SHA256_SIZE])
{
    hkdf_expand_label(secret, label, hash, SHA256_SIZE, out, SHA256_SIZE);
}

static void derive_keys(struct tls_connection *tls,
                        const uint8_t secret[SHA256_SIZE],
                        struct key_material *material)
{
    hkdf_expand_label(secret, "key", NULL, 0, material->key, tls->key_length);
    hkdf_expand_label(secret, "iv", NULL, 0, material->iv, 12);
    material->sequence = 0;

    if (tls->cipher == CIPHER_AES_GCM)
        aes_set_key(&material->aes, material->key, tls->key_length * 8);
}

/* Die Nonce entsteht aus dem festen Anteil und der laufenden Nummer. */
static void build_nonce(const struct key_material *material, uint8_t out[12])
{
    memcpy(out, material->iv, 12);

    for (int i = 0; i < 8; i++)
        out[11 - i] ^= (uint8_t)(material->sequence >> (i * 8));
}

/* ------------------------------------------------------------------ */
/* Datensaetze lesen und schreiben                                     */
/* ------------------------------------------------------------------ */

static bool send_raw(struct tls_connection *tls, uint8_t type,
                     const uint8_t *data, uint32_t length)
{
    uint8_t header[5];

    header[0] = type;
    put16(header + 1, 0x0303);
    put16(header + 3, (uint16_t)length);

    if (tcp_send(tls->socket, header, sizeof(header)) < 0)
        return false;
    return tcp_send(tls->socket, data, length) >= 0;
}

/* Verschluesselt und schickt; die wahre Art wird angehaengt. */
static bool send_encrypted(struct tls_connection *tls, uint8_t inner_type,
                           const uint8_t *data, uint32_t length)
{
    static uint8_t buffer[TLS_RECORD_MAX];
    uint8_t nonce[12];
    uint8_t aad[5];

    if (length + 1 + 16 > sizeof(buffer))
        return false;

    memcpy(buffer, data, length);
    buffer[length] = inner_type;

    uint32_t plain_length = length + 1;
    uint32_t total = plain_length + 16;

    aad[0] = REC_APPLICATION;
    put16(aad + 1, 0x0303);
    put16(aad + 3, (uint16_t)total);

    build_nonce(&tls->send_keys, nonce);

    uint8_t tag[16];

    if (tls->cipher == CIPHER_AES_GCM)
        aes_gcm_seal(&tls->send_keys.aes, nonce, aad, sizeof(aad),
                     buffer, plain_length, buffer, tag);
    else
        chacha20poly1305_seal(tls->send_keys.key, nonce, aad, sizeof(aad),
                              buffer, plain_length, buffer, tag);

    memcpy(buffer + plain_length, tag, 16);
    tls->send_keys.sequence++;

    if (tcp_send(tls->socket, aad, sizeof(aad)) < 0)
        return false;
    return tcp_send(tls->socket, buffer, total) >= 0;
}

/* Holt so lange Daten, bis ein vollstaendiger Datensatz vorliegt. */
static bool read_record(struct tls_connection *tls, uint8_t *type,
                        uint8_t *out, uint32_t *out_length, uint32_t timeout_ms)
{
    uint64_t deadline = timer_ms() + timeout_ms;

    for (;;) {
        if (tls->input_length >= 5) {
            uint32_t length = get16(tls->input + 3);

            if (length > TLS_RECORD_MAX)
                return false;

            if (tls->input_length >= 5 + length) {
                *type = tls->input[0];
                memcpy(out, tls->input + 5, length);
                *out_length = length;

                uint32_t consumed = 5 + length;
                memmove(tls->input, tls->input + consumed,
                        tls->input_length - consumed);
                tls->input_length -= consumed;
                return true;
            }
        }

        if (timer_ms() > deadline)
            return false;

        uint32_t room = (uint32_t)sizeof(tls->input) - tls->input_length;
        if (room == 0)
            return false;

        int n = tcp_receive(tls->socket, tls->input + tls->input_length,
                            room, 300);

        if (n > 0)
            tls->input_length += (uint32_t)n;
        else if (tcp_finished(tls->socket) && tls->input_length < 5)
            return false;
    }
}

/* Liest einen Datensatz und entschluesselt ihn, wenn noetig. */
static bool read_plain_record(struct tls_connection *tls, uint8_t *inner_type,
                              uint8_t *out, uint32_t *out_length,
                              uint32_t timeout_ms)
{
    static uint8_t record[TLS_RECORD_MAX];
    uint8_t type;
    uint32_t length;

    for (;;) {
        if (!read_record(tls, &type, record, &length, timeout_ms))
            return false;

        /* Zur Vertraeglichkeit mit alter Technik geschickt - ignorieren. */
        if (type == REC_CHANGE_CIPHER)
            continue;

        if (type != REC_APPLICATION) {
            /* Vor dem Schluesselwechsel kommt der Handschlag im Klartext. */
            *inner_type = type;
            memcpy(out, record, length);
            *out_length = length;
            return true;
        }

        if (length < 17)
            return false;

        uint8_t aad[5];
        uint8_t nonce[12];
        uint32_t payload = length - 16;

        aad[0] = REC_APPLICATION;
        put16(aad + 1, 0x0303);
        put16(aad + 3, (uint16_t)length);

        build_nonce(&tls->receive_keys, nonce);

        bool ok;
        if (tls->cipher == CIPHER_AES_GCM)
            ok = aes_gcm_open(&tls->receive_keys.aes, nonce, aad, sizeof(aad),
                              record, payload, record + payload, record);
        else
            ok = chacha20poly1305_open(tls->receive_keys.key, nonce, aad,
                                       sizeof(aad), record, payload,
                                       record + payload, record);

        if (!ok)
            return false;

        tls->receive_keys.sequence++;

        /* Hinten steht die wahre Art, davor koennen Nullen als Fuellung
         * stehen. */
        while (payload > 0 && record[payload - 1] == 0)
            payload--;
        if (payload == 0)
            continue;

        *inner_type = record[payload - 1];
        *out_length = payload - 1;
        memcpy(out, record, *out_length);

        if (*inner_type == REC_ALERT)
            return false;

        return true;
    }
}

/* ------------------------------------------------------------------ */
/* Der Handschlag                                                      */
/* ------------------------------------------------------------------ */

static uint32_t build_client_hello(uint8_t *out, const char *host,
                                   const uint8_t public_key[32],
                                   const uint8_t random[32],
                                   const uint8_t session_id[32])
{
    uint32_t pos = 0;

    out[pos++] = HS_CLIENT_HELLO;
    pos += 3;                                   /* Laenge, spaeter */

    uint32_t body = pos;

    put16(out + pos, 0x0303); pos += 2;         /* alte Versionsangabe */
    memcpy(out + pos, random, 32); pos += 32;

    out[pos++] = 32;                            /* Sitzungskennung */
    memcpy(out + pos, session_id, 32); pos += 32;

    put16(out + pos, 4); pos += 2;              /* Verfahren */
    put16(out + pos, SUITE_AES_128_GCM); pos += 2;
    put16(out + pos, SUITE_CHACHA20); pos += 2;

    out[pos++] = 1;                             /* keine Kompression */
    out[pos++] = 0;

    uint32_t extensions_length_at = pos;
    pos += 2;
    uint32_t extensions_start = pos;

    /* Rechnername */
    size_t host_length = strlen(host);
    put16(out + pos, 0); pos += 2;
    put16(out + pos, (uint16_t)(host_length + 5)); pos += 2;
    put16(out + pos, (uint16_t)(host_length + 3)); pos += 2;
    out[pos++] = 0;                             /* Art: Rechnername */
    put16(out + pos, (uint16_t)host_length); pos += 2;
    memcpy(out + pos, host, host_length); pos += host_length;

    /* Unterstuetzte Fassungen: nur TLS 1.3 */
    put16(out + pos, 43); pos += 2;
    put16(out + pos, 3); pos += 2;
    out[pos++] = 2;
    put16(out + pos, 0x0304); pos += 2;

    /* Gruppen: nur X25519 */
    put16(out + pos, 10); pos += 2;
    put16(out + pos, 4); pos += 2;
    put16(out + pos, 2); pos += 2;
    put16(out + pos, 0x001d); pos += 2;

    /* Unterschriftsverfahren, die wir pruefen koennen */
    static const uint16_t algorithms[] = {
        0x0804,   /* rsa_pss_rsae_sha256   */
        0x0805,   /* rsa_pss_rsae_sha384   */
        0x0403,   /* ecdsa_secp256r1_sha256 */
        0x0401,   /* rsa_pkcs1_sha256      */
        0x0501,   /* rsa_pkcs1_sha384      */
    };

    put16(out + pos, 13); pos += 2;
    put16(out + pos, (uint16_t)(sizeof(algorithms) + 2)); pos += 2;
    put16(out + pos, (uint16_t)sizeof(algorithms)); pos += 2;
    for (size_t i = 0; i < ARRAY_LEN(algorithms); i++) {
        put16(out + pos, algorithms[i]);
        pos += 2;
    }

    /* Unser Anteil am Schluesseltausch */
    put16(out + pos, 51); pos += 2;
    put16(out + pos, 38); pos += 2;
    put16(out + pos, 36); pos += 2;
    put16(out + pos, 0x001d); pos += 2;
    put16(out + pos, 32); pos += 2;
    memcpy(out + pos, public_key, 32); pos += 32;

    put16(out + extensions_length_at, (uint16_t)(pos - extensions_start));

    uint32_t length = pos - body;
    out[body - 3] = (uint8_t)(length >> 16);
    out[body - 2] = (uint8_t)(length >> 8);
    out[body - 1] = (uint8_t)length;

    return pos;
}

/* Liest die Antwort des Servers und holt seinen Schluesselanteil heraus. */
static bool parse_server_hello(struct tls_connection *tls, const uint8_t *data,
                               uint32_t length, uint8_t peer_key[32],
                               char *error, size_t error_size)
{
    if (length < 44) {
        strlcpy(error, "Die Antwort des Servers ist zu kurz.", error_size);
        return false;
    }

    uint32_t pos = 4;                           /* Art und Laenge ueberspringen */

    pos += 2;                                   /* alte Versionsangabe */
    pos += 32;                                  /* Zufall des Servers  */

    uint8_t session_length = data[pos++];
    pos += session_length;

    if (pos + 3 > length) {
        strlcpy(error, "Die Antwort des Servers ist unvollstaendig.", error_size);
        return false;
    }

    uint16_t suite = get16(data + pos);
    pos += 2;
    pos += 1;                                   /* Kompression */

    if (suite == SUITE_AES_128_GCM) {
        tls->cipher = CIPHER_AES_GCM;
        tls->key_length = 16;
        strlcpy(tls->description, "TLS 1.3, X25519, AES-128-GCM",
                sizeof(tls->description));
    } else if (suite == SUITE_CHACHA20) {
        tls->cipher = CIPHER_CHACHA;
        tls->key_length = 32;
        strlcpy(tls->description, "TLS 1.3, X25519, ChaCha20-Poly1305",
                sizeof(tls->description));
    } else {
        ksnprintf(error, error_size,
                  "Der Server bietet nur das Verfahren 0x%x an.", suite);
        return false;
    }

    if (pos + 2 > length)
        return false;

    uint16_t extensions_length = get16(data + pos);
    pos += 2;

    uint32_t end = pos + extensions_length;
    bool found_key = false;
    bool version_ok = false;

    while (pos + 4 <= end && pos + 4 <= length) {
        uint16_t type = get16(data + pos);
        uint16_t size = get16(data + pos + 2);

        pos += 4;
        if (pos + size > length)
            break;

        if (type == 51 && size >= 36) {          /* key_share */
            if (get16(data + pos) == 0x001d && get16(data + pos + 2) == 32) {
                memcpy(peer_key, data + pos + 4, 32);
                found_key = true;
            }
        } else if (type == 43 && size == 2) {    /* supported_versions */
            version_ok = get16(data + pos) == 0x0304;
        }

        pos += size;
    }

    if (!version_ok) {
        strlcpy(error, "Der Server spricht kein TLS 1.3.", error_size);
        return false;
    }
    if (!found_key) {
        strlcpy(error, "Der Server hat keinen X25519-Anteil geschickt.",
                error_size);
        return false;
    }
    return true;
}

/* Aktueller Zeitpunkt als JJJJMMTTHHMMSS - fuer die Gueltigkeitspruefung. */
static uint64_t current_time_number(void)
{
    struct datetime now;

    rtc_read(&now);

    if (now.year < 2000 || now.year > 2200)
        return 0;                                /* Uhr unglaubwuerdig */

    return (uint64_t)now.year * 10000000000ULL +
           (uint64_t)now.month * 100000000ULL +
           (uint64_t)now.day * 1000000ULL +
           (uint64_t)now.hour * 10000ULL +
           (uint64_t)now.minute * 100ULL + now.second;
}

/* Prueft die Unterschrift, mit der der Server seinen Schluessel beglaubigt. */
static bool check_certificate_verify(const struct certificate *server,
                                     const uint8_t *message, uint32_t length,
                                     const uint8_t transcript_before[SHA256_SIZE],
                                     char *error, size_t error_size)
{
    if (length < 8) {
        strlcpy(error, "Die Unterschrift des Servers fehlt.", error_size);
        return false;
    }

    uint16_t algorithm = get16(message + 4);
    uint16_t signature_length = get16(message + 6);
    const uint8_t *signature = message + 8;

    if ((uint32_t)signature_length + 8u > length) {
        strlcpy(error, "Die Unterschrift des Servers ist unvollstaendig.",
                error_size);
        return false;
    }

    /* Unterschrieben wird ein festgelegter Text plus der Verlauf. */
    uint8_t content[130 + SHA256_SIZE];
    uint32_t pos = 0;

    memset(content, 0x20, 64);
    pos = 64;

    static const char context[] = "TLS 1.3, server CertificateVerify";
    memcpy(content + pos, context, sizeof(context));    /* mit Null am Ende */
    pos += sizeof(context);

    memcpy(content + pos, transcript_before, SHA256_SIZE);
    pos += SHA256_SIZE;

    uint8_t hash[SHA512_SIZE];
    size_t hash_length;

    switch (algorithm) {
    case 0x0804:                                  /* RSA-PSS mit SHA-256 */
    case 0x0403:                                  /* ECDSA P-256         */
    case 0x0401:                                  /* RSA PKCS#1 SHA-256  */
        sha256(content, pos, hash);
        hash_length = SHA256_SIZE;
        break;
    case 0x0805:                                  /* RSA-PSS mit SHA-384 */
    case 0x0501:
        sha384(content, pos, hash);
        hash_length = SHA384_SIZE;
        break;
    default:
        ksnprintf(error, error_size,
                  "Unbekanntes Unterschriftsverfahren 0x%x.", algorithm);
        return false;
    }

    bool ok = false;

    switch (algorithm) {
    case 0x0804:
    case 0x0805:
        ok = server->key.type == KEY_RSA &&
             rsa_verify_pss(&server->key, hash, hash_length, signature,
                            signature_length);
        break;
    case 0x0401:
    case 0x0501:
        ok = server->key.type == KEY_RSA &&
             rsa_verify_pkcs1(&server->key, hash, hash_length, signature,
                              signature_length);
        break;
    case 0x0403:
        ok = server->key.type == KEY_EC_P256 && server->key.point_length == 65 &&
             ecdsa_p256_verify(server->key.point, hash, hash_length,
                               signature, signature_length);
        break;
    }

    if (!ok)
        strlcpy(error, "Der Server konnte seinen Schluessel nicht nachweisen.",
                error_size);
    return ok;
}

struct tls_connection *tls_connect(struct tcp_socket *socket, const char *host,
                                   char *error, size_t error_size)
{
    static uint8_t message[TLS_RECORD_MAX];
    static uint8_t certificates[TLS_RECORD_MAX];

    struct tls_connection *tls = kzalloc(sizeof(*tls));

    if (!tls) {
        strlcpy(error, "Zu wenig Speicher fuer die Verbindung.", error_size);
        return NULL;
    }

    tls->socket = socket;
    transcript_length = 0;

    /* --- eigenen Schluesselanteil erzeugen --- */
    uint8_t secret[32], public_key[32], random[32], session_id[32];

    crypto_random(secret, sizeof(secret));
    crypto_random(random, sizeof(random));
    crypto_random(session_id, sizeof(session_id));
    x25519_base(public_key, secret);

    uint32_t hello_length = build_client_hello(message, host, public_key,
                                               random, session_id);

    transcript_add(message, hello_length);

    if (!send_raw(tls, REC_HANDSHAKE, message, hello_length)) {
        strlcpy(error, "Der Verbindungswunsch liess sich nicht senden.",
                error_size);
        kfree(tls);
        return NULL;
    }

    /* --- Antwort des Servers --- */
    uint8_t type;
    uint32_t length;

    if (!read_plain_record(tls, &type, message, &length, TLS_HANDSHAKE_MS) ||
        type != REC_HANDSHAKE || length < 4 || message[0] != HS_SERVER_HELLO) {
        strlcpy(error, "Der Server hat nicht wie erwartet geantwortet.",
                error_size);
        kfree(tls);
        return NULL;
    }

    uint8_t peer_key[32];

    if (!parse_server_hello(tls, message, length, peer_key, error, error_size)) {
        kfree(tls);
        return NULL;
    }

    transcript_add(message, length);

    /* --- Schluessel ableiten --- */
    uint8_t shared[32];
    uint8_t early[SHA256_SIZE], derived[SHA256_SIZE], handshake[SHA256_SIZE];
    uint8_t empty_hash[SHA256_SIZE];
    uint8_t hello_hash[SHA256_SIZE];

    x25519(shared, secret, peer_key);

    /* Ein Schluessel aus lauter Nullen waere ein Angriff. */
    bool all_zero = true;
    for (int i = 0; i < 32; i++) {
        if (shared[i])
            all_zero = false;
    }
    if (all_zero) {
        strlcpy(error, "Der Schluesseltausch ist fehlgeschlagen.", error_size);
        kfree(tls);
        return NULL;
    }

    /* Ohne vorab geteilten Schluessel besteht der Ausgangswert aus so
     * vielen Nullbytes, wie die Streuwertfunktion lang ist. */
    static const uint8_t zero_key[SHA256_SIZE] = { 0 };

    sha256("", 0, empty_hash);
    hkdf_extract(NULL, 0, zero_key, sizeof(zero_key), early);
    derive_secret(early, "derived", empty_hash, derived);
    hkdf_extract(derived, SHA256_SIZE, shared, sizeof(shared), handshake);

    transcript_hash(hello_hash);
    derive_secret(handshake, "c hs traffic", hello_hash, tls->client_secret);
    derive_secret(handshake, "s hs traffic", hello_hash, tls->server_secret);

    derive_keys(tls, tls->client_secret, &tls->send_keys);
    derive_keys(tls, tls->server_secret, &tls->receive_keys);

    /* --- verschluesselte Nachrichten des Servers --- */
    struct certificate chain[TLS_CHAIN_MAX];
    size_t chain_count = 0;
    bool have_certificate = false;
    bool verified = false;
    uint8_t transcript_before_verify[SHA256_SIZE];
    uint8_t server_finished_hash[SHA256_SIZE];
    uint64_t deadline = timer_ms() + TLS_HANDSHAKE_MS;

    while (timer_ms() < deadline) {
        if (!read_plain_record(tls, &type, message, &length, 4000)) {
            strlcpy(error, "Die Antwort des Servers brach ab.", error_size);
            kfree(tls);
            return NULL;
        }
        if (type != REC_HANDSHAKE)
            continue;

        uint32_t offset = 0;

        while (offset + 4 <= length) {
            uint8_t kind = message[offset];
            uint32_t size = get24(message + offset + 1);

            if (offset + 4 + size > length)
                break;

            const uint8_t *body = message + offset + 4;

            if (kind == HS_CERTIFICATE) {
                /* Aufbau: Kontext, dann eine Liste von Zertifikaten. */
                uint32_t pos = 0;
                uint8_t context_length = body[pos++];

                pos += context_length;
                if (pos + 3 > size)
                    break;

                uint32_t list_length = get24(body + pos);
                pos += 3;

                uint32_t list_end = MIN(pos + list_length, size);
                uint32_t stored = 0;

                while (pos + 3 <= list_end && chain_count < TLS_CHAIN_MAX) {
                    uint32_t cert_length = get24(body + pos);

                    pos += 3;
                    if (pos + cert_length > list_end)
                        break;

                    if (stored + cert_length <= sizeof(certificates)) {
                        memcpy(certificates + stored, body + pos, cert_length);

                        if (certificate_parse(&chain[chain_count],
                                              certificates + stored,
                                              cert_length))
                            chain_count++;
                        stored += cert_length;
                    }

                    pos += cert_length;
                    if (pos + 2 > list_end)
                        break;
                    pos += 2 + get16(body + pos);   /* Zusaetze je Zertifikat */
                }

                have_certificate = chain_count > 0;
                transcript_add(message + offset, 4 + size);

                if (!have_certificate) {
                    strlcpy(error, "Das Zertifikat war nicht lesbar.",
                            error_size);
                    kfree(tls);
                    return NULL;
                }

                /* Der Verlauf bis hierher geht in die naechste Pruefung ein. */
                transcript_hash(transcript_before_verify);
                offset += 4 + size;
                continue;

            } else if (kind == HS_CERTIFICATE_VERIFY) {
                if (!have_certificate) {
                    strlcpy(error, "Der Server hat kein Zertifikat geschickt.",
                            error_size);
                    kfree(tls);
                    return NULL;
                }
                if (!check_certificate_verify(&chain[0], message + offset,
                                              4 + size,
                                              transcript_before_verify,
                                              error, error_size)) {
                    kfree(tls);
                    return NULL;
                }

                /* Erst jetzt die Kette pruefen - sie ist der eigentliche
                 * Nachweis, dass wir mit dem richtigen Gegenueber reden. */
                if (!certificate_verify_chain(chain, chain_count, host,
                                              current_time_number(),
                                              error, error_size)) {
                    kfree(tls);
                    return NULL;
                }
                verified = true;

            } else if (kind == HS_FINISHED) {
                if (!verified) {
                    strlcpy(error, "Der Server hat sich nicht ausgewiesen.",
                            error_size);
                    kfree(tls);
                    return NULL;
                }

                /* Der Abschluss ist ein HMAC ueber den bisherigen Verlauf. */
                uint8_t finished_key[SHA256_SIZE];
                uint8_t expected[SHA256_SIZE];
                uint8_t hash[SHA256_SIZE];

                transcript_hash(hash);
                hkdf_expand_label(tls->server_secret, "finished", NULL, 0,
                                  finished_key, SHA256_SIZE);
                hmac_sha256(finished_key, SHA256_SIZE, hash, SHA256_SIZE,
                            expected);

                if (size != SHA256_SIZE ||
                    !crypto_equal(expected, body, SHA256_SIZE)) {
                    strlcpy(error, "Der Abschluss des Servers stimmt nicht.",
                            error_size);
                    kfree(tls);
                    return NULL;
                }

                transcript_add(message + offset, 4 + size);
                transcript_hash(server_finished_hash);
                goto handshake_done;
            }

            transcript_add(message + offset, 4 + size);
            offset += 4 + size;
        }
    }

    strlcpy(error, "Der Handschlag kam nicht zum Ende.", error_size);
    kfree(tls);
    return NULL;

handshake_done:
    {
        /* --- eigener Abschluss --- */
        uint8_t finished_key[SHA256_SIZE];
        uint8_t verify_data[SHA256_SIZE];
        uint8_t finished[4 + SHA256_SIZE];

        hkdf_expand_label(tls->client_secret, "finished", NULL, 0,
                          finished_key, SHA256_SIZE);
        hmac_sha256(finished_key, SHA256_SIZE, server_finished_hash,
                    SHA256_SIZE, verify_data);

        finished[0] = HS_FINISHED;
        finished[1] = 0;
        finished[2] = 0;
        finished[3] = SHA256_SIZE;
        memcpy(finished + 4, verify_data, SHA256_SIZE);

        /* Ein leerer Datensatz zur Vertraeglichkeit mit alter Technik. */
        uint8_t dummy = 1;
        send_raw(tls, REC_CHANGE_CIPHER, &dummy, 1);

        if (!send_encrypted(tls, REC_HANDSHAKE, finished, sizeof(finished))) {
            strlcpy(error, "Der Abschluss liess sich nicht senden.",
                    error_size);
            kfree(tls);
            return NULL;
        }

        /* --- Schluessel fuer die Nutzdaten --- */
        uint8_t derived2[SHA256_SIZE], master[SHA256_SIZE];
        uint8_t empty_hash[SHA256_SIZE];
        uint8_t handshake_secret[SHA256_SIZE];

        /* handshake wurde oben berechnet; hier noch einmal, weil der
         * Bereich verlassen wurde. */
        static const uint8_t zero_key[SHA256_SIZE] = { 0 };

        sha256("", 0, empty_hash);
        hkdf_extract(NULL, 0, zero_key, sizeof(zero_key), early);
        derive_secret(early, "derived", empty_hash, derived);
        hkdf_extract(derived, SHA256_SIZE, shared, sizeof(shared),
                     handshake_secret);

        derive_secret(handshake_secret, "derived", empty_hash, derived2);
        hkdf_extract(derived2, SHA256_SIZE, zero_key, sizeof(zero_key), master);

        derive_secret(master, "c ap traffic", server_finished_hash,
                      tls->client_secret);
        derive_secret(master, "s ap traffic", server_finished_hash,
                      tls->server_secret);

        derive_keys(tls, tls->client_secret, &tls->send_keys);
        derive_keys(tls, tls->server_secret, &tls->receive_keys);

        tls->established = true;
    }

    return tls;
}

/* ------------------------------------------------------------------ */
/* Nutzdaten                                                           */
/* ------------------------------------------------------------------ */

int tls_send(struct tls_connection *tls, const void *data, uint32_t length)
{
    const uint8_t *bytes = data;
    uint32_t done = 0;

    if (!tls || !tls->established)
        return -1;

    while (done < length) {
        uint32_t chunk = MIN(length - done, (uint32_t)(TLS_RECORD_MAX - 64));

        if (!send_encrypted(tls, REC_APPLICATION, bytes + done, chunk))
            return -1;
        done += chunk;
    }
    return (int)done;
}

int tls_receive(struct tls_connection *tls, void *buffer, uint32_t capacity,
                uint32_t timeout_ms)
{
    if (!tls || !tls->established)
        return -1;

    /* Erst ausliefern, was noch daliegt. */
    if (tls->plain_read < tls->plain_length) {
        uint32_t available = tls->plain_length - tls->plain_read;
        uint32_t take = MIN(available, capacity);

        memcpy(buffer, tls->plain + tls->plain_read, take);
        tls->plain_read += take;
        return (int)take;
    }

    uint8_t type;
    uint32_t length;

    for (;;) {
        if (!read_plain_record(tls, &type, tls->plain, &length, timeout_ms)) {
            tls->closed = true;
            return 0;
        }

        /* Sitzungsscheine und dergleichen kommen nach dem Handschlag und
         * werden hier nicht gebraucht. */
        if (type == REC_HANDSHAKE)
            continue;
        if (type != REC_APPLICATION) {
            tls->closed = true;
            return 0;
        }

        tls->plain_length = length;
        tls->plain_read = 0;

        uint32_t take = MIN(length, capacity);
        memcpy(buffer, tls->plain, take);
        tls->plain_read = take;
        return (int)take;
    }
}

bool tls_finished(const struct tls_connection *tls)
{
    if (!tls)
        return true;
    if (tls->plain_read < tls->plain_length)
        return false;
    return tls->closed || tcp_finished(tls->socket);
}

void tls_close(struct tls_connection *tls)
{
    if (!tls)
        return;

    if (tls->established && !tls->closed) {
        /* Hoeflich Bescheid geben: close_notify. */
        uint8_t alert[2] = { 1, 0 };

        send_encrypted(tls, REC_ALERT, alert, sizeof(alert));
    }
    kfree(tls);
}

const char *tls_description(const struct tls_connection *tls)
{
    return tls ? tls->description : "";
}
