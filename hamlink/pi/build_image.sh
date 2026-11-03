#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the ready-to-flash relay image: Raspberry Pi OS Lite (64-bit) + hamlinkd (install.sh) + harden.sh.
# Runs as root on an ARM64 Linux machine (GitHub's ubuntu-24.04-arm runner), so no emulation is needed.
#
#   sudo hamlink/pi/build_image.sh <output dir> <version> <libhamlink_api.so>
#
# The image boots into the relay's hotspot (hamlink-relay / hamlink-setup) with the web page on http://10.42.0.1/,
# not configured yet: the first visitor sets the page password, then the callsign and radio link.
set -euxo pipefail

OUT=$(realpath "$1"); VERSION="$2"; LIB=$(realpath "$3")
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OS_URL="${HAMLINK_OS_URL:-https://downloads.raspberrypi.com/raspios_lite_arm64_latest}"
work=$(mktemp -d)
mnt="$work/mnt"
dev=""

cleanup() {
    set +e
    for m in dev/pts dev proc sys boot/firmware ""; do mountpoint -q "$mnt/$m" && umount -l "$mnt/$m"; done
    [ -n "$dev" ] && losetup -d "$dev"
}
trap cleanup EXIT

apt-get install -y -q parted e2fsprogs xz-utils >/dev/null
curl -fsSL -o "$work/os.img.xz" "$OS_URL"
xz -dT0 "$work/os.img.xz"
img="$work/os.img"

# Room for the packages and the source tree.
truncate -s +1536M "$img"
parted -s "$img" resizepart 2 100%
dev=$(losetup -fP --show "$img")
e2fsck -fy "${dev}p2" || true
resize2fs "${dev}p2"

mkdir -p "$mnt"
mount "${dev}p2" "$mnt"
mount "${dev}p1" "$mnt/boot/firmware"
mount --bind /dev "$mnt/dev"
mount --bind /dev/pts "$mnt/dev/pts"
mount -t proc proc "$mnt/proc"
mount -t sysfs sys "$mnt/sys"

cp -L --remove-destination /etc/resolv.conf "$mnt/etc/resolv.conf.hamlink"
mv "$mnt/etc/resolv.conf" "$mnt/etc/resolv.conf.orig" 2>/dev/null || true
cp "$mnt/etc/resolv.conf.hamlink" "$mnt/etc/resolv.conf"

mkdir -p "$mnt/tmp/hamlink-src"
cp -a "$REPO/." "$mnt/tmp/hamlink-src/"
cp "$LIB" "$mnt/tmp/libhamlink_api.so"
chroot "$mnt" env HAMLINK_SRC_DIR=/tmp/hamlink-src HAMLINK_LIB_URL=file:///tmp/libhamlink_api.so HAMLINK_NO_START=1 \
    bash /tmp/hamlink-src/hamlink/pi/install.sh
HAMLINK_ROOT="$mnt" bash "$REPO/hamlink/pi/harden.sh"
echo "$VERSION" > "$mnt/opt/hamlink/IMAGE_VERSION"

chroot "$mnt" apt-get clean
rm -rf "$mnt/tmp/hamlink-src" "$mnt/tmp/libhamlink_api.so" "$mnt/etc/resolv.conf.hamlink"
rm -f "$mnt/etc/resolv.conf"
mv "$mnt/etc/resolv.conf.orig" "$mnt/etc/resolv.conf" 2>/dev/null || true

cleanup
dev=""
trap - EXIT

mkdir -p "$OUT"
name="hamlink-relay-$VERSION.img.xz"
xz -T0 -6 -c "$img" > "$OUT/$name"
(cd "$OUT" && sha256sum "$name" > "$name.sha256")
rm -rf "$work"
ls -l "$OUT"
