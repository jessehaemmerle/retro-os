#!/bin/sh
# Startet RetroOS in QEMU.
#   scripts/run.sh            BIOS-Boot, Grafikfenster
#   scripts/run.sh --uefi     UEFI-Boot (benoetigt OVMF)
#   scripts/run.sh --vnc      ohne lokales Fenster, VNC auf :0
set -e
cd "$(dirname "$0")/.."

ISO="retroos.iso"
[ -f "$ISO" ] || { echo "Kein $ISO - erst 'make' ausfuehren."; exit 1; }

QEMU="qemu-system-x86_64"
ARGS="-M q35 -m 512M -cdrom $ISO -boot d -serial stdio -rtc base=localtime"

for arg in "$@"; do
    case "$arg" in
        --uefi)
            CODE=$(ls /usr/share/OVMF/OVMF_CODE*.fd 2>/dev/null | head -1)
            VARS=$(ls /usr/share/OVMF/OVMF_VARS*.fd 2>/dev/null | head -1)
            [ -n "$CODE" ] || { echo "OVMF nicht gefunden (apt install ovmf)"; exit 1; }

            # Der Variablenspeicher muss beschreibbar sein - Kopie anlegen.
            mkdir -p build
            [ -f build/ovmf_vars.fd ] || cp "$VARS" build/ovmf_vars.fd

            ARGS="$ARGS -drive if=pflash,format=raw,unit=0,readonly=on,file=$CODE"
            ARGS="$ARGS -drive if=pflash,format=raw,unit=1,file=build/ovmf_vars.fd"
            ;;
        --vnc) ARGS="$ARGS -vnc :0" ;;
        *) ARGS="$ARGS $arg" ;;
    esac
done

exec $QEMU $ARGS
