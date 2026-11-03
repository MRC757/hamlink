# SPDX-License-Identifier: GPL-3.0-or-later
"""Pairing a radio's Bluetooth TNC through bluetoothctl (BlueZ). Used by the web page and `hamlinkd --pair-tnc`.

Radios such as the UV-PRO pair without a PIN ("just works"). After pairing, the TNC link uses tnc_port
bt:<address>, which connects straight to the radio over RFCOMM; no rfcomm binding is needed.
"""
import re
import subprocess

ADDR = re.compile(r"^([0-9A-F]{2}:){5}[0-9A-F]{2}$")


def _ctl(*args, timeout=30):
    try:
        r = subprocess.run(["bluetoothctl", *args], capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError:
        raise RuntimeError("bluetoothctl not found (install bluez)")
    return r.stdout + r.stderr


def _devices(text):
    out = []
    for line in text.splitlines():
        m = re.search(r"Device (([0-9A-F]{2}:){5}[0-9A-F]{2}) (.*)$", line.strip())
        if m:
            out.append({"address": m.group(1), "name": m.group(3).strip()})
    return out


def known():
    """Devices BlueZ knows, each with paired True/False."""
    paired = {d["address"] for d in _devices(_ctl("devices", "Paired"))}
    return [dict(d, paired=d["address"] in paired) for d in _devices(_ctl("devices"))]


def scan(seconds=12):
    """Scans for nearby devices (put the radio in pairing mode first), then lists everything known."""
    _ctl("power", "on")
    _ctl("--timeout", str(seconds), "scan", "on", timeout=seconds + 10)
    return known()


def pair(address):
    address = address.strip().upper()
    if not ADDR.match(address):
        raise ValueError("not a Bluetooth address")
    out = _ctl("pair", address, timeout=40)
    if "Pairing successful" not in out and "AlreadyExists" not in out:
        raise RuntimeError("pairing failed: " + (out.strip().splitlines() or ["no answer"])[-1])
    _ctl("trust", address)


def pair_interactive():
    """`hamlinkd --pair-tnc`: scan, choose, pair, and print the tnc_port value."""
    print("Put the radio in Bluetooth pairing mode. Scanning for 12 s...")
    devs = scan(12)
    if not devs:
        print("Nothing found.")
        return 1
    for i, d in enumerate(devs, 1):
        print(f"  {i}. {d['name']}  {d['address']}{'  (paired)' if d['paired'] else ''}")
    choice = input("Number of the radio: ").strip()
    if not choice.isdigit() or not 1 <= int(choice) <= len(devs):
        return 1
    d = devs[int(choice) - 1]
    if not d["paired"]:
        pair(d["address"])
    print(f"Paired. Settings: link tnc, tnc_port bt:{d['address']}  (sudo hamlinkd --setup, or the web page)")
    return 0
