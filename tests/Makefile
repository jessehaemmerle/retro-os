# Prueft Kryptografie und Bildleser gegen Vergleichsdaten.
CC      ?= gcc
CFLAGS  := -std=gnu11 -O2 -g -Wall -Wextra -Wno-unused-parameter \
           -I../kernel/include -I. -DTRUST_STORE_EXTERNAL -DRETRO_HOSTED

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

TABELLE := tabellen_test.c shim.c \
           ../kernel/src/apps/sheet.c

KONSOLE := konsole_test.c shim.c \
           ../kernel/src/apps/shellutil.c

KAEFIG := kaefig_test.c shim.c \
          ../kernel/src/proc/sandbox.c

NETZFILTER := netzfilter_test.c shim.c \
              ../kernel/src/net/firewall.c

PROTOKOLL := protokoll_test.c shim.c \
             ../kernel/src/lib/log.c

AUFGABEN := aufgaben_test.c shim.c \
            ../kernel/src/apps/tasks.c

RECHTE := rechte_test.c shim.c \
          ../kernel/src/fs/perm.c \
          ../kernel/src/fs/user.c \
          ../kernel/src/crypto/sha2.c \
          ../kernel/src/crypto/hkdf.c

SCHREIBEN := schreiben_test.c shim.c \
             ../kernel/src/apps/writedoc.c \
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

.PHONY: all krypto bilder skript baum tabelle schreiben rechte aufgaben \
        protokoll netzfilter kaefig konsole testbilder clean
all: krypto bilder skript baum tabelle schreiben rechte aufgaben protokoll \
     netzfilter kaefig konsole

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

konsole: konsole-test
	@./konsole-test

konsole-test: $(KONSOLE)
	@$(CC) $(CFLAGS) $(KONSOLE) -o $@

kaefig: kaefig-test
	@./kaefig-test

kaefig-test: $(KAEFIG)
	@$(CC) $(CFLAGS) $(KAEFIG) -o $@

netzfilter: netzfilter-test
	@./netzfilter-test

netzfilter-test: $(NETZFILTER)
	@$(CC) $(CFLAGS) $(NETZFILTER) -o $@

protokoll: protokoll-test
	@./protokoll-test

protokoll-test: $(PROTOKOLL)
	@$(CC) $(CFLAGS) $(PROTOKOLL) -o $@

aufgaben: aufgaben-test
	@./aufgaben-test

aufgaben-test: $(AUFGABEN)
	@$(CC) $(CFLAGS) $(AUFGABEN) -o $@

rechte: rechte-test
	@./rechte-test

rechte-test: $(RECHTE)
	@$(CC) $(CFLAGS) $(RECHTE) -o $@

tabelle: tabellen-test
	@./tabellen-test

tabellen-test: $(TABELLE)
	@$(CC) $(CFLAGS) $(TABELLE) -o $@

schreiben: schreiben-test
	@./schreiben-test

schreiben-test: $(SCHREIBEN)
	@$(CC) $(CFLAGS) $(SCHREIBEN) -o $@

bild-test: $(BILDER)
	@$(CC) $(CFLAGS) $(BILDER) -lm -o $@

# Erzeugt die Testbilder neu (braucht Pillow).
testbilder:
	@python3 gen_images.py

clean:
	@rm -f krypto-test bild-test js-test baum-test tabellen-test \
	       schreiben-test
