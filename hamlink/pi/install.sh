#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs (or updates) the HamLink relay service on Raspberry Pi OS (64-bit) or another Debian-based Linux.
#   curl -sSL https://raw.githubusercontent.com/MRC757/hamlink/master/hamlink/pi/install.sh | bash
# or, from a clone:  hamlink/pi/install.sh
# Run it again to update; the settings in /etc/hamlink are kept.
#
# Environment: HAMLINK_SRC_DIR=<checkout> installs from a local copy instead of cloning (CI, the SD image);
# HAMLINK_BUILD=1 always builds the core here; HAMLINK_LIB_URL=<url> takes the library from elsewhere.
#
#   /opt/hamlink/src    the repository (shallow clone)
#   /opt/hamlink/lib    libhamlink_api.so (downloaded for ARM64, else built here)
#   /opt/hamlink/venv   Python environment
#   /etc/hamlink        settings (hamlinkd.json)
#   /var/lib/hamlink    recordings and other state
set -euo pipefail

REPO_URL="${HAMLINK_REPO:-https://github.com/MRC757/hamlink.git}"
BRANCH="${HAMLINK_BRANCH:-master}"
API_RELEASES="https://api.github.com/repos/MRC757/hamlink/releases?per_page=20"
LIB_ASSET="libhamlink_api-linux-aarch64.so"
PREFIX=/opt/hamlink
SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO=sudo

say() { printf '\n== %s\n' "$*"; }

say "System packages"
$SUDO apt-get update -q
$SUDO apt-get install -y -q git python3 python3-venv python3-numpy python3-serial python3-cffi libportaudio2 \
    bluez avahi-daemon curl ca-certificates

say "Service user"
if ! id hamlink >/dev/null 2>&1; then
    $SUDO useradd --system --home-dir /var/lib/hamlink --shell /usr/sbin/nologin hamlink
fi
for g in audio dialout gpio bluetooth; do
    getent group "$g" >/dev/null && $SUDO usermod -aG "$g" hamlink
done
$SUDO install -d -o hamlink -g hamlink /var/lib/hamlink
# The service saves settings from the web page, so it owns its settings folder.
$SUDO install -d -o hamlink -g hamlink -m 755 /etc/hamlink
$SUDO chown -R hamlink:hamlink /etc/hamlink

say "Source"
if [ -n "${HAMLINK_SRC_DIR:-}" ]; then
    $SUDO rm -rf "$PREFIX/src"
    $SUDO install -d "$PREFIX"
    $SUDO cp -a "$HAMLINK_SRC_DIR" "$PREFIX/src"
elif [ -d "$PREFIX/src/.git" ]; then
    $SUDO git -C "$PREFIX/src" fetch -q --depth 1 origin "$BRANCH"
    $SUDO git -C "$PREFIX/src" reset -q --hard FETCH_HEAD
else
    $SUDO install -d "$PREFIX"
    $SUDO git clone -q --depth 1 --branch "$BRANCH" "$REPO_URL" "$PREFIX/src"
fi
SRC="$PREFIX/src/hamlink"

say "Core library"
$SUDO install -d "$PREFIX/lib"
got=""
if [ "$(uname -m)" = "aarch64" ] && [ -z "${HAMLINK_BUILD:-}" ]; then
    # The newest release (pre-releases included) that carries the ARM64 library.
    url="${HAMLINK_LIB_URL:-$(curl -fsSL "$API_RELEASES" 2>/dev/null | python3 -c '
import json, sys
for r in json.load(sys.stdin):
    for a in r.get("assets", []):
        if a["name"] == sys.argv[1]:
            print(a["browser_download_url"]); sys.exit()
' "$LIB_ASSET" || true)}"
    tmp=$(mktemp)
    if [ -n "$url" ] && curl -fsSL -o "$tmp" "$url"; then
        $SUDO install -m 644 "$tmp" "$PREFIX/lib/libhamlink_api.so"
        got="downloaded"
    fi
    rm -f "$tmp"
fi
if [ -z "$got" ]; then
    echo "No prebuilt library for this machine; building it (several minutes on a Pi Zero 2 W)."
    $SUDO apt-get install -y -q build-essential cmake
    jobs=2
    [ "$(awk '/MemTotal/ {print $2}' /proc/meminfo)" -lt 1000000 ] && jobs=1  # 512 MB: one compiler at a time
    $SUDO cmake -S "$SRC" -B "$PREFIX/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    $SUDO cmake --build "$PREFIX/build" --target hamlink_api -j"$jobs"
    $SUDO install -m 644 "$PREFIX/build/core/libhamlink_api.so" "$PREFIX/lib/libhamlink_api.so"
    got="built"
fi
echo "libhamlink_api.so $got"

say "Python environment"
[ -x "$PREFIX/venv/bin/python" ] || $SUDO python3 -m venv --system-site-packages "$PREFIX/venv"
$SUDO "$PREFIX/venv/bin/python" -m pip install -q --upgrade sounddevice

say "Command and service"
$SUDO tee /usr/local/bin/hamlinkd >/dev/null <<EOF
#!/bin/sh
export PYTHONPATH="$SRC/pi"
exec "$PREFIX/venv/bin/python" -m hamlinkd --lib "$PREFIX/lib/libhamlink_api.so" "\$@"
EOF
$SUDO chmod 755 /usr/local/bin/hamlinkd
$SUDO install -m 644 "$SRC/pi/hamlinkd.service" /etc/systemd/system/hamlinkd.service
$SUDO systemctl daemon-reload || true  # (ignored inside an image build chroot)
$SUDO systemctl enable hamlinkd >/dev/null 2>&1

# The library must load and match the service's API version before the service is (re)started.
/usr/local/bin/hamlinkd --check

if [ -n "${HAMLINK_NO_START:-}" ] || [ ! -d /run/systemd/system ]; then
    say "Installed (not started: building an image)"
else
    $SUDO systemctl restart hamlinkd
    say "Done. Web page: http://$(hostname).local/ (set its password on the first visit), or sudo hamlinkd --setup.
   Status: hamlinkd --status   Log: journalctl -u hamlinkd -f"
fi
