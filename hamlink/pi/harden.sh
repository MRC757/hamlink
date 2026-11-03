#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Prepares a Pi that already has hamlinkd (install.sh) for unattended use on a hill. Run as root:
#
#   sudo hamlink/pi/harden.sh                       watchdog, RAM-only logs, no swap, hotspot, hostname
#   sudo hamlink/pi/harden.sh --hotspot SSID PASS   change the hotspot name and password (8-63 characters)
#   sudo hamlink/pi/harden.sh --client SSID PASS    also join this Wi-Fi network when it is in range (preferred)
#   sudo hamlink/pi/harden.sh --read-only           root file system read-only (an overlay in RAM); reboot to apply
#   sudo hamlink/pi/harden.sh --writable            back to a normal file system; reboot to apply
#
# Read-only mode protects the SD card from power cuts, but every change (settings from the web page, the password,
# updates) is then lost at the next reboot: turn it off, change things, turn it on again.
#
# HAMLINK_ROOT=<dir> applies the files to a mounted image instead of this system (used by the SD image build).
# HAMLINK_COUNTRY=<ISO code> sets the Wi-Fi country (default US); the hotspot uses its channels and power limits.
set -euo pipefail

ROOT="${HAMLINK_ROOT:-}"
SSID_DEFAULT="hamlink-relay"
PASS_DEFAULT="hamlink-setup"
HOST_DEFAULT="hamlink-relay"
COUNTRY="${HAMLINK_COUNTRY:-US}"

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)"; exit 1; }
say() { printf '== %s\n' "$*"; }

hotspot() {  # NetworkManager access point; the Pi is 10.42.0.1 on it and hands out addresses
    local ssid="$1" pass="$2"
    [ ${#pass} -ge 8 ] && [ ${#pass} -le 63 ] || { echo "the hotspot password needs 8-63 characters"; exit 1; }
    local f="$ROOT/etc/NetworkManager/system-connections/hamlink-ap.nmconnection"
    install -d -m 700 "$(dirname "$f")"
    cat > "$f" <<EOF
[connection]
id=hamlink-ap
type=wifi
interface-name=wlan0
autoconnect=true
autoconnect-priority=0

[wifi]
mode=ap
ssid=$ssid
band=bg

[wifi-security]
key-mgmt=wpa-psk
psk=$pass
proto=rsn
pairwise=ccmp
group=ccmp

[ipv4]
method=shared
address1=10.42.0.1/24

[ipv6]
method=disabled
EOF
    chmod 600 "$f"
    say "hotspot: $ssid (the relay's page: http://10.42.0.1/ or http://$(cat "$ROOT/etc/hostname" 2>/dev/null || echo "$HOST_DEFAULT").local/)"
}

client() {  # a known network that wins over the hotspot whenever it is in range
    local ssid="$1" pass="$2"
    local f="$ROOT/etc/NetworkManager/system-connections/hamlink-client.nmconnection"
    install -d -m 700 "$(dirname "$f")"
    cat > "$f" <<EOF
[connection]
id=hamlink-client
type=wifi
interface-name=wlan0
autoconnect=true
autoconnect-priority=10

[wifi]
mode=infrastructure
ssid=$ssid

[wifi-security]
key-mgmt=wpa-psk
psk=$pass

[ipv4]
method=auto

[ipv6]
method=auto
EOF
    chmod 600 "$f"
    say "client network: $ssid (preferred when in range; otherwise the hotspot)"
}

reload_nm() {
    [ -z "$ROOT" ] && command -v nmcli >/dev/null && nmcli connection reload || true
}

case "${1:-}" in
--hotspot)
    hotspot "${2:?SSID}" "${3:?password}"; reload_nm
    echo "Takes effect at the next reboot (or: nmcli connection up hamlink-ap)."; exit 0 ;;
--client)
    client "${2:?SSID}" "${3:?password}"; reload_nm; exit 0 ;;
--read-only)
    command -v raspi-config >/dev/null || { echo "needs raspi-config (Raspberry Pi OS)"; exit 1; }
    raspi-config nonint do_overlayfs 0   # 0 = enable
    echo "Read-only from the next reboot: sudo reboot"; exit 0 ;;
--writable)
    command -v raspi-config >/dev/null || { echo "needs raspi-config (Raspberry Pi OS)"; exit 1; }
    raspi-config nonint do_overlayfs 1
    echo "Writable from the next reboot: sudo reboot"; exit 0 ;;
"") ;;
*) sed -n '3,15p' "$0"; exit 1 ;;
esac

say "hardware watchdog: the Pi reboots itself if the system hangs for 15 s"
install -d "$ROOT/etc/systemd/system.conf.d"
cat > "$ROOT/etc/systemd/system.conf.d/hamlink-watchdog.conf" <<'EOF'
[Manager]
RuntimeWatchdogSec=15
RebootWatchdogSec=2min
EOF

say "logs in RAM only (no SD card wear; journalctl still works until the next reboot)"
install -d "$ROOT/etc/systemd/journald.conf.d"
cat > "$ROOT/etc/systemd/journald.conf.d/hamlink.conf" <<'EOF'
[Journal]
Storage=volatile
RuntimeMaxUse=24M
EOF

say "no swap file (it wears the SD card; the relay needs about 60 MB)"
for unit in dphys-swapfile.service rpi-swap.service; do
    if [ -z "$ROOT" ]; then
        systemctl disable --now "$unit" >/dev/null 2>&1 || true
    else
        rm -f "$ROOT/etc/systemd/system/multi-user.target.wants/$unit"
    fi
done

if [ ! -s "$ROOT/etc/hostname" ] || grep -qx raspberrypi "$ROOT/etc/hostname"; then
    say "hostname: $HOST_DEFAULT"
    echo "$HOST_DEFAULT" > "$ROOT/etc/hostname"
    sed -i "s/\braspberrypi\b/$HOST_DEFAULT/g" "$ROOT/etc/hosts" 2>/dev/null || true
    [ -z "$ROOT" ] && hostname "$HOST_DEFAULT" || true
fi

say "Wi-Fi country $COUNTRY, and Wi-Fi unblocked at boot (Raspberry Pi OS blocks it until a country is set)"
for c in "$ROOT/boot/firmware/cmdline.txt" "$ROOT/boot/cmdline.txt"; do
    if [ -f "$c" ]; then
        sed -i -E 's/ ?cfg80211\.ieee80211_regdom=[A-Z]+//' "$c"
        sed -i -E "1s/\$/ cfg80211.ieee80211_regdom=$COUNTRY/" "$c"
        break
    fi
done
install -d "$ROOT/etc/systemd/system/multi-user.target.wants"
cat > "$ROOT/etc/systemd/system/hamlink-wifi-unblock.service" <<'EOF'
[Unit]
Description=Unblock Wi-Fi for the HamLink hotspot
Before=NetworkManager.service

[Service]
Type=oneshot
ExecStart=-/usr/sbin/rfkill unblock wifi

[Install]
WantedBy=multi-user.target
EOF
ln -sf /etc/systemd/system/hamlink-wifi-unblock.service "$ROOT/etc/systemd/system/multi-user.target.wants/"

if [ ! -f "$ROOT/etc/NetworkManager/system-connections/hamlink-ap.nmconnection" ]; then
    hotspot "$SSID_DEFAULT" "$PASS_DEFAULT"
    echo "   CHANGE the default hotspot password: sudo $0 --hotspot <name> <password>"
fi
reload_nm

say "done. Reboot to apply the watchdog and logging settings. Then, optionally: sudo $0 --read-only"
