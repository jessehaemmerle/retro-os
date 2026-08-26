#!/bin/sh
# Holt den Limine-Bootloader (vorkompilierte Binaries) nach third_party/limine.
# RetroOS bringt seinen eigenen Kernel mit; Limine uebernimmt nur das Laden
# des Kernels im Long Mode inklusive Framebuffer (BIOS und UEFI).
set -e

VERSION="v8.7.0-binary"
DEST="$(dirname "$0")/../third_party/limine"

if [ -f "$DEST/limine.h" ] && [ -x "$DEST/limine" ]; then
    echo "limine: bereits vorhanden ($DEST)"
    exit 0
fi

echo "limine: lade $VERSION ..."
mkdir -p "$(dirname "$DEST")"
rm -rf "$DEST"
git clone --depth=1 --branch="$VERSION" \
    https://github.com/limine-bootloader/limine.git "$DEST" 2>/dev/null \
  || git clone --depth=1 --branch="v8.x-binary" \
    https://github.com/limine-bootloader/limine.git "$DEST"

make -C "$DEST" >/dev/null
echo "limine: bereit"
