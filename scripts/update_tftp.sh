#!/bin/bash
# Fill the TFTP directory for network boot of the Raspberry Pi 3B+ IOC.
#
# Usage: scripts/update_tftp.sh [TFTP_ROOT]        default /srv/tftp/rpi
#
# Copies the SD card set (SD_card/, symlinks resolved) plus the DHCP
# cmdline.txt into TFTP_ROOT, so the firmware fetches exactly the files it
# would read from the card: bootcode.bin, start.elf, fixup.dat, the dtb,
# config.txt, cmdline.txt, kernel7.img (= bin/.../rpi3_plain.img) and
# epics.tar.gz. A per-board directory named after the board serial (the
# firmware tries <serial>/<file> first) gets symlinks to the shared files,
# so a board-specific override is a matter of replacing one symlink by a
# file. Files are made world-readable: NetworkManager's dnsmasq serves
# them as user "nobody". Run it after every "make" or "make ramfs" whose
# result should reach a network-booted board.
set -eu
TOP=$(cd "$(dirname "$0")/.." && pwd)
ROOT=${1:-/srv/tftp/rpi}
SERIALS=${SERIALS:-9645d7b0}         # space separated list of board serials (last 8 hex digits)

[ -d "$ROOT" ] || { echo "update_tftp: $ROOT does not exist (sudo mkdir -p $ROOT; sudo chown $USER $ROOT)"; exit 1; }
[ -w "$ROOT" ] || { echo "update_tftp: $ROOT is not writable by $USER"; exit 1; }

for f in bootcode.bin start.elf fixup.dat bcm2710-rpi-3-b-plus.dtb config.txt kernel7.img epics.tar.gz; do
    cp -L "$TOP/SD_card/$f" "$ROOT/$f"
done
cp "$TOP/SD_card/cmdline-dhcp.txt" "$ROOT/cmdline.txt"     # network boot implies DHCP for the IOC too
chmod 644 "$ROOT"/*

for s in $SERIALS; do
    mkdir -p "$ROOT/$s"
    chmod 755 "$ROOT/$s"
    for f in start.elf fixup.dat bcm2710-rpi-3-b-plus.dtb config.txt cmdline.txt kernel7.img epics.tar.gz; do
        [ -L "$ROOT/$s/$f" ] || [ ! -e "$ROOT/$s/$f" ] && ln -sfn "../$f" "$ROOT/$s/$f"
    done
done

echo "TFTP root $ROOT:"
ls -lL "$ROOT" | sed 1d
echo "kernel7.img  $(md5sum < "$ROOT/kernel7.img" | cut -c1-8)..  epics.tar.gz $(md5sum < "$ROOT/epics.tar.gz" | cut -c1-8)..  cmdline: $(cat "$ROOT/cmdline.txt")"
for s in $SERIALS; do echo "per-board $s/: $(ls "$ROOT/$s" | tr '\n' ' ')"; done
