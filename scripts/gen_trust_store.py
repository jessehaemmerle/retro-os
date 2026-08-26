#!/usr/bin/env python3
"""Erzeugt den Wurzelspeicher von RetroOS aus einer PEM-Sammlung.

Aus einer Datei mit vielen "BEGIN CERTIFICATE"-Bloecken (wie sie jedes
Linux-System unter /etc/ssl/certs mitbringt) wird eine schlichte
Binärdatei: für jedes Zertifikat vier Byte Länge, dann die DER-Form.

    python3 scripts/gen_trust_store.py [quelle.pem]

Das Ergebnis liegt in data/wurzelzertifikate.der und wird beim Bauen in
den Kernel eingebettet.
"""

import base64
import os
import re
import sys

DEFAULT_SOURCES = [
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/usr/share/ssl/certs/ca-bundle.crt",
]


def find_source(argv):
    if len(argv) > 1:
        return argv[1]
    for path in DEFAULT_SOURCES:
        if os.path.exists(path):
            return path
    sys.exit("Keine PEM-Sammlung gefunden - Pfad bitte angeben.")


def main():
    source = find_source(sys.argv)
    text = open(source).read()
    blocks = re.findall(
        r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", text, re.S)

    if not blocks:
        sys.exit("In %s steht kein Zertifikat." % source)

    out = bytearray()
    for block in blocks:
        der = base64.b64decode(re.sub(r"\s", "", block))
        out += len(der).to_bytes(4, "big") + der

    destination = os.path.join(os.path.dirname(__file__), "..", "data",
                               "wurzelzertifikate.der")
    destination = os.path.normpath(destination)
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    open(destination, "wb").write(out)

    print("%d Zertifikate, %d Byte -> %s" % (len(blocks), len(out), destination))


if __name__ == "__main__":
    main()
