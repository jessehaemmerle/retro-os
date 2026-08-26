/* trust.c - der Vorrat an Wurzelzertifikaten und die Pruefung ganzer Ketten.
 *
 * Die Wurzeln sind beim Uebersetzen fest eingebaut; sie stammen aus der
 * ueblichen Sammlung, die jedes Betriebssystem mitbringt. Ohne sie waere
 * jede Verbindung nur so sicher wie die Behauptung der Gegenseite - erst
 * eine Kette bis zu einer bekannten Wurzel macht daraus einen Nachweis.
 */

#include "pki.h"
#include "kstring.h"

#define TRUST_MAX 256

#ifndef TRUST_STORE_EXTERNAL
/* Im Kernel liegt die Sammlung fest im Abbild. */
extern const uint8_t _binary_wurzelzertifikate_der_start[];
extern const uint8_t _binary_wurzelzertifikate_der_end[];
#endif

static struct certificate roots[TRUST_MAX];
static size_t             root_count;
static bool               loaded;
static const uint8_t     *store_data;
static size_t             store_length;

/* Der Pruefstand auf dem Entwicklungsrechner schiebt die Sammlung zur
 * Laufzeit unter, statt sie einzubetten. */
void trust_store_set(const uint8_t *data, size_t length)
{
    store_data = data;
    store_length = length;
    loaded = false;
    root_count = 0;
}

void trust_store_init(void)
{
    if (loaded)
        return;
    loaded = true;

    const uint8_t *p;
    const uint8_t *end;

    if (store_data) {
        p = store_data;
        end = store_data + store_length;
    } else {
#ifndef TRUST_STORE_EXTERNAL
        p = _binary_wurzelzertifikate_der_start;
        end = _binary_wurzelzertifikate_der_end;
#else
        return;
#endif
    }
    size_t skipped = 0;

    /* Aufbau der Sammlung: vier Byte Laenge, dann das Zertifikat. */
    while (p + 4 <= end && root_count < TRUST_MAX) {
        uint32_t length = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                          ((uint32_t)p[2] << 8) | p[3];

        p += 4;
        if (length == 0 || p + length > end)
            break;

        if (certificate_parse(&roots[root_count], p, length))
            root_count++;
        else
            skipped++;

        p += length;
    }

    kprintf("Zertifikate : %u Wurzeln geladen%s\n", (unsigned)root_count,
            skipped ? " (einige uebersprungen)" : "");
}

size_t trust_store_count(void)
{
    return root_count;
}

const struct certificate *trust_store_find_by_index(size_t index)
{
    trust_store_init();
    return index < root_count ? &roots[index] : NULL;
}

const struct certificate *trust_store_find(const uint8_t *issuer,
                                           size_t issuer_length)
{
    trust_store_init();

    for (size_t i = 0; i < root_count; i++) {
        /* Der Aussteller des einen ist der Inhaber des anderen - die
         * DER-Form beider Namen muss dafuer Byte fuer Byte gleich sein. */
        if (roots[i].subject_length == issuer_length &&
            memcmp(roots[i].subject, issuer, issuer_length) == 0)
            return &roots[i];
    }
    return NULL;
}

bool certificate_verify_chain(struct certificate *chain, size_t count,
                              const char *host, uint64_t now,
                              char *reason, size_t reason_size)
{
    trust_store_init();

    if (count == 0) {
        strlcpy(reason, "Der Server hat kein Zertifikat geschickt.", reason_size);
        return false;
    }

    /* 1. Gehoert das Zertifikat zu diesem Rechner? */
    if (host && host[0] && !certificate_matches_host(&chain[0], host)) {
        ksnprintf(reason, reason_size,
                  "Das Zertifikat gilt nicht fuer %s (ausgestellt auf \"%s\").",
                  host, chain[0].common_name);
        return false;
    }

    /* 2. Ist jedes Glied zeitlich gueltig? */
    if (now != 0) {
        for (size_t i = 0; i < count; i++) {
            if (chain[i].not_before && now < chain[i].not_before) {
                ksnprintf(reason, reason_size,
                          "Das Zertifikat \"%s\" gilt erst spaeter.",
                          chain[i].common_name);
                return false;
            }
            if (chain[i].not_after && now > chain[i].not_after) {
                ksnprintf(reason, reason_size,
                          "Das Zertifikat \"%s\" ist abgelaufen.",
                          chain[i].common_name);
                return false;
            }
        }
    }

    /* 3. Unterschreibt jedes Glied das darunter? */
    for (size_t i = 0; i + 1 < count; i++) {
        if (!chain[i + 1].is_ca) {
            ksnprintf(reason, reason_size,
                      "\"%s\" darf keine Zertifikate ausstellen.",
                      chain[i + 1].common_name);
            return false;
        }
        if (chain[i].issuer_length != chain[i + 1].subject_length ||
            memcmp(chain[i].issuer, chain[i + 1].subject,
                   chain[i].issuer_length) != 0) {
            strlcpy(reason, "Die Zertifikatskette passt nicht zusammen.",
                    reason_size);
            return false;
        }
        if (!certificate_verify_signature(&chain[i], &chain[i + 1].key)) {
            ksnprintf(reason, reason_size,
                      "Die Unterschrift unter \"%s\" stimmt nicht.",
                      chain[i].common_name);
            return false;
        }
    }

    /* 4. Endet die Kette bei einer bekannten Wurzel? */
    const struct certificate *last = &chain[count - 1];
    const struct certificate *root = trust_store_find(last->issuer,
                                                      last->issuer_length);

    if (!root) {
        /* Manche Server schicken die Wurzel selbst mit - dann muss sie in
         * unserem Vorrat stehen. */
        root = trust_store_find(last->subject, last->subject_length);

        if (root && root->raw_length == last->raw_length &&
            memcmp(root->raw, last->raw, last->raw_length) == 0) {
            return true;
        }

        ksnprintf(reason, reason_size,
                  "Der Aussteller \"%s\" ist nicht vertrauenswuerdig.",
                  last->common_name);
        return false;
    }

    if (!certificate_verify_signature(last, &root->key)) {
        strlcpy(reason, "Die Unterschrift der Wurzel stimmt nicht.",
                reason_size);
        return false;
    }

    return true;
}
