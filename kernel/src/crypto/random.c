/* random.c - Zufall fuer Schluessel und Nonces.
 *
 * Bevorzugt wird der Zufallsgenerator der CPU (RDRAND). Fehlt er, wird
 * aus allem, was sich im Rechner unterscheidet - Zeitstempelzaehler,
 * Systemuhr, Laufzeit, Adressen frisch geholter Seiten - ein Startwert
 * gebildet und daraus mit SHA-256 ein Strom erzeugt.
 *
 * Der zweite Weg ist deutlich schwaecher als ein richtiger Zufalls-
 * generator. Er reicht, damit zwei Verbindungen nicht dieselben
 * Schluessel benutzen; fuer Geheimnisse, an denen etwas haengt, sollte
 * man sich darauf nicht verlassen.
 */

#include "crypto.h"
#include "arch.h"
#include "kstring.h"
#include "rtc.h"

static uint8_t  pool[SHA256_SIZE];
static uint64_t counter;
static bool     have_rdrand;
static bool     checked;

static uint64_t timestamp(void)
{
    uint32_t low, high;

    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static bool cpu_has_rdrand(void)
{
    uint32_t a, b, c, d;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(1), "c"(0));
    return (c & (1u << 30)) != 0;
}

static bool rdrand64(uint64_t *out)
{
    unsigned char ok = 0;
    uint64_t value = 0;

    for (int attempt = 0; attempt < 10; attempt++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(value), "=qm"(ok));
        if (ok) {
            *out = value;
            return true;
        }
    }
    return false;
}

void crypto_seed(const void *data, size_t length)
{
    struct sha256 ctx;

    sha256_init(&ctx);
    sha256_update(&ctx, pool, sizeof(pool));
    sha256_update(&ctx, data, length);
    sha256_final(&ctx, pool);
}

static void stir(void)
{
    struct datetime now;
    uint64_t values[4];

    rtc_read(&now);

    values[0] = timestamp();
    values[1] = timer_ms();
    values[2] = ((uint64_t)now.year << 40) | ((uint64_t)now.month << 32) |
                ((uint64_t)now.day << 24) | ((uint64_t)now.hour << 16) |
                ((uint64_t)now.minute << 8) | now.second;
    values[3] = (uint64_t)(uintptr_t)&values;

    crypto_seed(values, sizeof(values));
}

void crypto_random(void *out, size_t length)
{
    uint8_t *bytes = out;

    if (!checked) {
        checked = true;
        have_rdrand = cpu_has_rdrand();
        stir();
    }

    while (length > 0) {
        uint8_t block[SHA256_SIZE];

        if (have_rdrand) {
            uint64_t value;
            size_t produced = 0;

            while (produced < SHA256_SIZE && rdrand64(&value)) {
                memcpy(block + produced, &value, 8);
                produced += 8;
            }

            if (produced == SHA256_SIZE) {
                /* Auch den Zufall der CPU noch durch die Streuwert-
                 * funktion schicken - dann faellt ein schwacher
                 * Generator nicht unmittelbar durch. */
                crypto_seed(block, sizeof(block));
            } else {
                have_rdrand = false;
                continue;
            }
        }

        struct sha256 ctx;

        counter++;
        sha256_init(&ctx);
        sha256_update(&ctx, pool, sizeof(pool));
        sha256_update(&ctx, &counter, sizeof(counter));
        uint64_t stamp = timestamp();
        sha256_update(&ctx, &stamp, sizeof(stamp));
        sha256_final(&ctx, block);

        size_t take = MIN(length, sizeof(block));
        memcpy(bytes, block, take);
        bytes += take;
        length -= take;
    }
}
