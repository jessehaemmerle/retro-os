# Prüfstand für die Kryptografie

Die Verfahren in `kernel/src/crypto` sind von Hand geschrieben. Damit sie
nachweislich das Richtige tun, werden dieselben Quelldateien hier gegen die
offiziellen Testvektoren der jeweiligen Spezifikation übersetzt und geprüft:

| Verfahren | Vektoren aus |
| --- | --- |
| SHA-256/384/512 | FIPS 180-4 |
| HMAC-SHA256 | RFC 4231 |
| HKDF | RFC 5869 |
| ChaCha20 / Poly1305 / AEAD | RFC 8439 |
| AES-128/256 | FIPS 197 |
| AES-GCM | NIST GCM Test Vectors |
| X25519 | RFC 7748 |
| RSA / ECDSA / X.509 | eigene, aus echten Zertifikaten erzeugte Fälle |

```sh
make -C tests        # übersetzt und führt alles aus
```

Der Prüfstand läuft auf dem Entwicklungsrechner, nicht in RetroOS – er
braucht keine besondere Umgebung und ist deshalb der schnellste Weg, einen
Fehler in der Rechnung zu finden.
