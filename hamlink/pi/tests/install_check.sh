#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# CI: after install.sh, checks the real systemd service end to end: first-run web password, settings from the web
# page start the radio, the CLI talks to it, the systemd watchdog stays fed, and harden.sh writes its files.
set -euo pipefail

fail() { echo "FAIL: $*"; sudo journalctl -u hamlinkd --no-pager | tail -60; exit 1; }
api() {  # api METHOD PATH [JSON]
    curl -sS -b /tmp/jar -c /tmp/jar -H 'X-HamLink: 1' -H 'Content-Type: application/json' \
        -X "$1" "http://127.0.0.1$2" ${3:+--data "$3"}
}

for _ in $(seq 30); do systemctl is-active -q hamlinkd && break; sleep 1; done
systemctl is-active -q hamlinkd || fail "service not active"
for _ in $(seq 30); do curl -sf http://127.0.0.1/api/session >/dev/null && break; sleep 1; done

api GET /api/session | grep -q '"password_set": false' || fail "fresh install should have no web password"
hamlinkd --status | grep -q '"state": "not configured"' || fail "should wait for settings"
api POST /api/setup-password '{"password": "ci-relay-pass"}' | grep -q '"ok": true' || fail "setting the password"
api POST /api/settings '{"values": {"callsign": "N0CI", "link": "tnc", "tnc_port": "/dev/ttyHAMLINKCI"}}' \
    | grep -q '"ok": true' || fail "saving settings"
for _ in $(seq 20); do hamlinkd --status | grep -q '"state": "running"' && break; sleep 1; done
hamlinkd --status | grep -q '"state": "running"' || fail "radio did not start from the web settings"
hamlinkd --send CQ "install check" | grep -q queued || fail "CLI send"
api GET /api/log | grep -q "install check" || fail "log over the web"
[ "$(stat -c %U /etc/hamlink/hamlinkd.json)" = hamlink ] || fail "settings file not owned by the service user"
[ "$(stat -c %a /etc/hamlink/web.json)" = 600 ] || fail "password file readable by others"

echo "waiting 45 s: the systemd watchdog (30 s) must not restart the service"
sleep 45
[ "$(systemctl show -p NRestarts --value hamlinkd)" = 0 ] || fail "service restarted"
[ "$(systemctl show -p WatchdogUSec --value hamlinkd)" = 30s ] || fail "watchdog not configured"

root=$(mktemp -d)
sudo HAMLINK_ROOT="$root" bash hamlink/pi/harden.sh >/dev/null
for f in etc/systemd/system.conf.d/hamlink-watchdog.conf etc/systemd/journald.conf.d/hamlink.conf \
         etc/NetworkManager/system-connections/hamlink-ap.nmconnection etc/hostname \
         etc/systemd/system/multi-user.target.wants/hamlink-wifi-unblock.service; do
    sudo test -e "$root/$f" -o -L "$root/$f" || fail "harden.sh did not write $f"  # (-L: links point into the target system)
done
sudo grep -q "ssid=hamlink-relay" "$root/etc/NetworkManager/system-connections/hamlink-ap.nmconnection" || fail "hotspot"

echo "install check passed"
