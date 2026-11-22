#!/usr/bin/env bash
#
# install_windows.sh - install Windows onto a disk image so VMBR can boot it.
#
# Two-phase plan (see docs/DISK_DRIVER.md):
#   1. This script boots PLAIN QEMU (no VMBR) from the install ISO and lets
#      Windows install itself onto a fresh disk image, in LEGACY BIOS / MBR
#      mode - which is exactly what VMBR boots (an MBR at 0x7C00). Do NOT add
#      UEFI firmware here, or Windows installs in GPT/UEFI mode and VMBR's
#      int 0x13 MBR boot won't find it.
#   2. Afterwards, boot the installed disk UNDER VMBR:
#         make run DISK_IMG=<disk> DISK_FMT=<fmt> MEM=4G
#
# The QEMU binary is invoked exactly as the Makefile does (bare name), so it
# resolves in the same environment where `make run` already works. Override
# with QEMU=/path and QEMU_IMG=/path if needed.
#
# Usage:
#   tools/install_windows.sh                 # uses the defaults below
#   tools/install_windows.sh <iso> <disk> <size>
#   ISO=... DISK=... SIZE=40G MEM=4G tools/install_windows.sh
#
set -euo pipefail

# --- configuration (env or positional args) ---
ISO="${1:-${ISO:-/mnt/d/CyberProject12/VMBR/Win10/Windows.iso}}"
DISK="${2:-${DISK:-/mnt/d/CyberProject12/VMBR/Win10/win10.qcow2}}"
SIZE="${3:-${SIZE:-40G}}"
MEM="${MEM:-4G}"
SMP="${SMP:-2}"
QEMU="${QEMU:-qemu-system-x86_64}"
QEMU_IMG="${QEMU_IMG:-qemu-img}"

echo "== VMBR Windows install helper =="
echo "  ISO:   $ISO"
echo "  DISK:  $DISK"
echo "  SIZE:  $SIZE   MEM: $MEM   SMP: $SMP"
echo

# --- sanity checks ---
if [ ! -f "$ISO" ]; then
    echo "ERROR: install ISO not found: $ISO" >&2
    exit 1
fi

# --- create the disk image if it doesn't exist ---
# Prefer qcow2 (grows on demand - friendly on an NTFS/drvfs mount). If
# qemu-img isn't available, fall back to a raw sparse file via truncate.
DISK_FMT="qcow2"
if [ -e "$DISK" ]; then
    echo "[*] Disk already exists, reusing it: $DISK"
    case "$DISK" in
        *.raw|*.img) DISK_FMT="raw" ;;
        *)           DISK_FMT="qcow2" ;;
    esac
else
    if command -v "$QEMU_IMG" >/dev/null 2>&1; then
        echo "[*] Creating qcow2 disk ($SIZE)..."
        "$QEMU_IMG" create -f qcow2 "$DISK" "$SIZE"
        DISK_FMT="qcow2"
    else
        echo "[!] $QEMU_IMG not found - creating a raw sparse disk instead."
        DISK="${DISK%.qcow2}.img"
        truncate -s "$SIZE" "$DISK"
        DISK_FMT="raw"
        echo "[*] Created raw disk: $DISK"
    fi
fi

echo
echo "[*] Launching the Windows installer in plain QEMU (legacy BIOS, no VMBR)."
echo "    A graphical QEMU window should open - click through the install."
echo "    When Windows finishes installing and reboots to the desktop, you"
echo "    can close QEMU."
echo

# -machine pc      : i440FX + SeaBIOS = legacy BIOS (MBR install). No UEFI.
# -boot d          : boot the CD-ROM (installer) first.
# index=0,media=disk: the target HDD, seen by SeaBIOS as drive 0x80 - the
#                    same drive VMBR reads via int 0x13.
# default display  : a window (Windows setup is graphical - do NOT use
#                    -nographic here, unlike `make run`).
# -usb -device usb-tablet: usable mouse in the installer.
set -x
"$QEMU" \
    -machine pc \
    -m "$MEM" \
    -smp "$SMP" \
    -enable-kvm -cpu host \
    -drive file="$DISK",format="$DISK_FMT",index=0,media=disk \
    -cdrom "$ISO" \
    -boot d \
    -vga std \
    -usb -device usb-tablet \
    -rtc base=localtime
set +x

echo
echo "== Install session ended =="
echo "Next: boot the installed disk UNDER VMBR with"
echo
echo "    make run DISK_IMG=\"$DISK\" DISK_FMT=$DISK_FMT MEM=$MEM"
echo
echo "(VMBR will load that disk's MBR at 0x7C00 and hand it to the VMX guest.)"
