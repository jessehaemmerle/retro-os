# Prueft Kryptografie und Bildleser gegen Vergleichsdaten.
CC      ?= gcc
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
           -I../kernel/include -I. -DTRUST_STORE_EXTERNAL

KRYPTO := crypto_test.c shim.c \
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

BILDER := image_test.c shim.c \
          ../kernel/src/gfx/inflate.c \
          ../kernel/src/gfx/png.c \
          ../kernel/src/gfx/jpeg.c \
          ../kernel/src/gfx/gif.c \
          ../kernel/src/gfx/image.c

SKRIPT := js_test.c shim.c \
          ../kernel/src/js/jsvalue.c \
          ../kernel/src/js/jsparse.c \
          ../kernel/src/js/jsinterp.c \
          ../kernel/src/js/jsbuiltin.c \
          ../kernel/src/js/jsdom.c \
          ../kernel/src/js/jsmain.c \
          ../kernel/src/apps/dom.c \
          ../kernel/src/apps/htmlparse.c

BAUM := dom_test.c shim.c \
        ../kernel/src/apps/dom.c \
        ../kernel/src/apps/htmlparse.c \
        ../kernel/src/apps/css.c \
        ../kernel/src/apps/layout.c \
        ../kernel/src/js/jsvalue.c \
        ../kernel/src/js/jsparse.c \
        ../kernel/src/js/jsinterp.c \
        ../kernel/src/js/jsbuiltin.c \
        ../kernel/src/js/jsdom.c \
        ../kernel/src/js/jsmain.c

.PHONY: all krypto bilder skript baum testbilder clean
all: krypto bilder skript baum

krypto: krypto-test
	@./krypto-test

bilder: bild-test
	@./bild-test

krypto-test: $(KRYPTO)
	@$(CC) $(CFLAGS) $(KRYPTO) -o $@

skript: js-test
	@./js-test

js-test: $(SKRIPT)
	@$(CC) $(CFLAGS) -I../kernel/src/js $(SKRIPT) -o $@

baum: baum-test
	@./baum-test

baum-test: $(BAUM)
	@$(CC) $(CFLAGS) -I../kernel/src/js $(BAUM) -o $@

bild-test: $(BILDER)
	@$(CC) $(CFLAGS) $(BILDER) -lm -o $@

# Erzeugt die Testbilder neu (braucht Pillow).
testbilder:
	@python3 gen_images.py

clean:
	@rm -f krypto-test bild-test js-test baum-test
