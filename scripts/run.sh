#!/bin/sh
# Startet RetroOS in QEMU - mit Festplatte und Netzwerk.
#
#   scripts/run.sh            BIOS-Boot, Grafikfenster
#   scripts/run.sh --uefi     UEFI-Boot (benoetigt OVMF)
#   scripts/run.sh --vnc      ohne lokales Fenster, VNC auf :0
#   scripts/run.sh --no-disk  ohne Festplatte starten
#   scripts/run.sh --no-net   ohne Netzwerkkarte starten
set -e
cd "$(dirname "$0")/.."

ISO="retroos.iso"
DISK="build/festplatte.img"
DISK_SIZE_MB=256

[ -f "$ISO" ] || { echo "Kein $ISO - erst 'make' ausfuehren."; exit 1; }

QEMU="qemu-system-x86_64"
ARGS="-M q35 -m 512M -cdrom $ISO -boot d -serial stdio -rtc base=localtime"

want_disk=1
want_net=1

for arg in "$@"; do
    case "$arg" in
        --no-disk) want_disk=0 ;;
        --no-net)  want_net=0 ;;
    esac
done

if [ "$want_disk" = "1" ]; then
    if [ ! -f "$DISK" ]; then
        mkdir -p build
        echo "Lege $DISK an (${DISK_SIZE_MB} MiB) ..."
        if command -v qemu-img >/dev/null 2>&1; then
            qemu-img create -f raw "$DISK" "${DISK_SIZE_MB}M" >/dev/null
        else
            dd if=/dev/zero of="$DISK" bs=1M count="$DISK_SIZE_MB" \
               status=none
        fi
        # Wenn moeglich gleich mit FAT32 vorbereiten; sonst formatiert
        # RetroOS die Platte selbst ueber den Konsolenbefehl "formatieren".
        if command -v mkfs.vfat >/dev/null 2>&1; then
            mkfs.vfat -F 32 -n RETROOS "$DISK" >/dev/null 2>&1 || true
        else
            echo "Hinweis: In RetroOS 'formatieren wirklich' ausfuehren,"
            echo "         damit die Platte nutzbar wird."
        fi
    fi
    ARGS="$ARGS -drive file=$DISK,if=none,id=hd0,format=raw"
    ARGS="$ARGS -device ide-hd,drive=hd0,bus=ide.0"
fi

if [ "$want_net" = "1" ]; then
    ARGS="$ARGS -netdev user,id=n0 -device e1000,netdev=n0"
fi

for arg in "$@"; do
    case "$arg" in
        --no-disk|--no-net) ;;
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
