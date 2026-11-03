#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs (or updates) the HamLink relay service on Raspberry Pi OS (64-bit) or another Debian-based Linux.
#   curl -sSL https://raw.githubusercontent.com/MRC757/hamlink/master/hamlink/pi/install.sh | bash
# or, from a clone:  hamlink/pi/install.sh
# Run it again to update; the settings in /etc/hamlink are kept.
#
#   /opt/hamlink/src    the repository (shallow clone)
#   /opt/hamlink/lib    libhamlink_api.so (downloaded for ARM64, else built here)
#   /opt/hamlink/venv   Python environment
#   /etc/hamlink        settings (hamlinkd.json)
#   /var/lib/hamlink    recordings and other state
set -euo pipefail

REPO_URL="${HAMLINK_REPO:-https://github.com/MRC757/hamlink.git}"
BRANCH="${HAMLINK_BRANCH:-master}"
LIB_URL="${HAMLINK_LIB_URL:-https://github.com/MRC757/hamlink/releases/latest/download/libhamlink_api-linux-aarch64.so}"
PREFIX=/opt/hamlink
SUDO=""
[ "$(id -u)" -eq 0 ] || SUDO=sudo

say() { printf '\n== %s\n' "$*"; }

say "System packages"
$SUDO apt-get update -q
$SUDO apt-get install -y -q git python3 python3-venv python3-numpy python3-serial python3-cffi libportaudio2 \
    bluez curl ca-certificates

say "Service user"
if ! id hamlink >/dev/null 2>&1; then
    $SUDO useradd --system --home-dir /var/lib/hamlink --shell /usr/sbin/nologin hamlink
fi
for g in audio dialout gpio bluetooth; do
    getent group "$g" >/dev/null && $SUDO usermod -aG "$g" hamlink
done
$SUDO install -d -o hamlink -g hamlink /var/lib/hamlink
$SUDO install -d /etc/hamlink

say "Source"
if [ -d "$PREFIX/src/.git" ]; then
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
    tmp=$(mktemp)
    if curl -fsSL -o "$tmp" "$LIB_URL"; then
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
$SUDO systemctl daemon-reload
$SUDO systemctl enable hamlinkd >/dev/null 2>&1

# The library must load and match the service's API version before the service is (re)started.
/usr/local/bin/hamlinkd --check

if [ -f /etc/hamlink/hamlinkd.json ] && /usr/local/bin/hamlinkd --check-config >/dev/null 2>&1; then
    $SUDO systemctl restart hamlinkd
    say "Done: hamlinkd restarted. Status: hamlinkd --status   Log: journalctl -u hamlinkd -f"
else
    say "Installed. Next: sudo hamlinkd --setup   then   sudo systemctl start hamlinkd"
fi
