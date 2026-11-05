#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Signs the unsigned release APK built by .github/workflows/android-release.yml with the HamLink release key, the way
# F-Droid's reproducible-build check needs: APK Signature Scheme v2 + v3 only (no v1 JAR files, no v4 .idsig), and the
# existing zip alignment preserved, so the signed APK is exactly the unsigned one plus apksigner's padding and signing
# block. F-Droid rebuilds the unsigned APK, copies this signature onto it, and publishes the result if it verifies.
#
#   gh run download <run-id> -n hamlink-android-unsigned          # the CI artifact for the release tag
#   hamlink/tools/sign_release_apk.sh app-release-unsigned.apk HamLink-0.2.4-android.apk
#
# The key and its password come from %USERPROFILE%/.hamlink-release/keystore.properties (or HAMLINK_KEYSTORE_PROPS);
# the password is passed to apksigner through the environment and never printed. Needs the Android SDK build-tools.
set -euo pipefail
in="${1:?unsigned APK}"; out="${2:?signed APK to write}"
props="${HAMLINK_KEYSTORE_PROPS:-$HOME/.hamlink-release/keystore.properties}"
[ -f "$props" ] || props="${USERPROFILE:-}/.hamlink-release/keystore.properties"
[ -f "$props" ] || { echo "keystore.properties not found" >&2; exit 1; }
get() { grep "^$1=" "$props" | cut -d= -f2-; }
sdk="${ANDROID_HOME:-${LOCALAPPDATA:-}/Android/Sdk}"
apksigner=$(ls "$sdk"/build-tools/*/apksigner* 2>/dev/null | grep -v '\.jar$' | sort | tail -1)
[ -n "$apksigner" ] || { echo "apksigner not found in $sdk/build-tools" >&2; exit 1; }
run() { case "$apksigner" in *.bat) cmd //c "$(cygpath -w "$apksigner")" "$@" ;; *) "$apksigner" "$@" ;; esac; }
ks="$(get storeFile)"; command -v cygpath >/dev/null && ks="$(cygpath -w "$ks")"
export HL_KS_PASS="$(get storePassword)" HL_KEY_PASS="$(get keyPassword)"
run sign --ks "$ks" --ks-key-alias "$(get keyAlias)" --ks-pass env:HL_KS_PASS --key-pass env:HL_KEY_PASS \
    --v1-signing-enabled false --v2-signing-enabled true --v3-signing-enabled true --v4-signing-enabled false \
    --alignment-preserved --out "$out" "$in"
unset HL_KS_PASS HL_KEY_PASS
run verify --verbose --print-certs "$out" | grep -E "^Verifies|v2 scheme|v3 scheme|certificate SHA-256"
sha256sum "$out"
