# Prueft die Kryptografie gegen die offiziellen Testvektoren.
CC      ?= gcc
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
           -I../kernel/include -I. -DTRUST_STORE_EXTERNAL
SOURCES := crypto_test.c shim.c \
           ../kernel/src/crypto/sha2.c \
           ../kernel/src/crypto/hkdf.c \
           ../kernel/src/crypto/chacha.c \
           ../kernel/src/crypto/aes.c \
           ../kernel/src/crypto/x25519.c \
           ../kernel/src/crypto/bignum.c \
           ../kernel/src/crypto/asn1.c \
           ../kernel/src/crypto/rsa.c \
           ../kernel/src/crypto/p256.c \
           ../kernel/src/crypto/x509.c \
           ../kernel/src/crypto/trust.c

.PHONY: all clean
all: krypto-test
	@./krypto-test

krypto-test: $(SOURCES)
	@$(CC) $(CFLAGS) $(SOURCES) -o $@

clean:
	@rm -f krypto-test
