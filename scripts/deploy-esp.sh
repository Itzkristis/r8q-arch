#!/usr/bin/env bash
# Copy the kernel Image to the ESP as BOOTAA64.EFI.
# Phone must be in Mu-Silicium MASS-STORAGE mode.
# The ESP is the phone's `cache` partition, reformatted vfat (label R8QESP).
set -euo pipefail
IMG="${1:?usage: deploy-esp.sh path/to/Image}"
ESP="$(lsblk -o NAME,PARTLABEL -rn | awk '$2=="cache"{print "/dev/"$1}')"
[ -n "$ESP" ] || { echo "!! ESP (cache/R8QESP) not found — is the phone in mass storage?"; exit 1; }
MNT="$(mktemp -d)"; sudo mount "$ESP" "$MNT"
sudo mkdir -p "$MNT/EFI/BOOT"
sudo cp "$IMG" "$MNT/EFI/BOOT/BOOTAA64.EFI"; sync
sudo umount "$MNT"
echo "[+] staged $IMG -> ESP:/EFI/BOOT/BOOTAA64.EFI"
