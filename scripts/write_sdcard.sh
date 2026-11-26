#!/bin/sh

set -e

# SD card device
DISK="/dev/sde"

# Image files
IFS_IMAGE="/home/hegde/project/qnxworkspace/project_minimal/images/ifs-rpi4.ui"
ROOTFS_IMAGE="/home/hegde/project/qnxworkspace/project_minimal/images/rootfs.img"

echo "======================================"
echo "Writing IFS image to ${DISK}2..."
echo "======================================"

sudo dd if="$IFS_IMAGE" of="${DISK}2" bs=4M status=progress
sudo sync
echo "IFS image written successfully."

echo
echo "======================================"
echo "Writing RootFS image to ${DISK}4..."
echo "======================================"

sudo dd if="$ROOTFS_IMAGE" of="${DISK}4" bs=4M status=progress
sudo sync
echo "RootFS image written successfully."

echo
echo "Partition information:"
lsblk -o NAME,SIZE,FSTYPE,LABEL "$DISK"

echo
echo "Calculating IFS size..."

IFS_BYTES=$(stat -c%s "$IFS_IMAGE")
IFS_SECTORS=$(( (IFS_BYTES + 511) / 512 ))

echo "IFS size : ${IFS_BYTES} bytes = ${IFS_SECTORS} sectors"
printf "IFS size (hex sectors): 0x%X\n" "$IFS_SECTORS"

echo
echo "Done."
