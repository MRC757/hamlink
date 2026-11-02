#!/usr/bin/env bash
# Rebuild windows/native/hamlink_core.dll (x64) from core/ + third_party/ on Linux, macOS, or Windows (Git Bash).
# Uses zig as the cross compiler (pip install ziglang) or mingw-w64 if you have it installed.
# The COFDMTV and MT63 modems are C++17, so the link step uses the C++ driver (libc++ is linked statically).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/windows/native/hamlink_core.dll}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
INC=(-I"$ROOT/core" -I"$ROOT/third_party/codec2/src" -I"$ROOT/third_party/codec2/src/codec2" -I"$ROOT/third_party/aicodix")
CSRC=("$ROOT"/core/{hamlink_api,audiocheck,voice,hdlc,afsk,fx25,psk,mesh,tone,tnc,geo,nmea,report,mapmodel,schemas_gen}.c "$ROOT"/third_party/codec2/src/*.c)

if command -v x86_64-w64-mingw32-gcc >/dev/null; then
  CC=(x86_64-w64-mingw32-gcc); CXX=(x86_64-w64-mingw32-g++); LD=(x86_64-w64-mingw32-g++ -static-libgcc -static-libstdc++)
else
  # first Python that has zig; on Windows "python3" may be the Microsoft Store placeholder, which cannot import anything
  PY=""
  for c in python3 python; do
    if "$c" -c "import ziglang" >/dev/null 2>&1; then PY="$c"; break; fi
  done
  [ -n "$PY" ] || { echo "no compiler: pip install ziglang (or install mingw-w64)" >&2; exit 1; }
  CC=("$PY" -m ziglang cc -target x86_64-windows-gnu); CXX=("$PY" -m ziglang c++ -target x86_64-windows-gnu); LD=("${CXX[@]}")
fi

i=0; OBJS=()
for f in "${CSRC[@]}"; do
  o="$TMP/$((i++)).o"
  "${CC[@]}" -c -O2 -w -DCODEC2_VERSION_STR='"1.2.0"' "${INC[@]}" "$f" -o "$o"; OBJS+=("$o")
done
"${CXX[@]}" -c -std=c++17 -O2 -w "${INC[@]}" "$ROOT/core/ofdm.cpp" -o "$TMP/ofdm.o"; OBJS+=("$TMP/ofdm.o")
for f in "$ROOT/core/mt63.cpp" "$ROOT/third_party/mt63/dsp.cxx" "$ROOT/third_party/mt63/mt63base.cxx"; do # MT63-2000 (fldigi code)
  o="$TMP/$((i++)).o"
  # own include path: fldigi's <config.h> must not resolve to Codec2's config.h
  "${CXX[@]}" -c -std=c++17 -O2 -w -I"$ROOT/core" -I"$ROOT/third_party/mt63" "$f" -o "$o"; OBJS+=("$o")
done
"${LD[@]}" -shared "${OBJS[@]}" -o "$OUT"
rm -f "${OUT%.dll}.pdb" "$(dirname "$OUT")"/*.lib
echo "built $OUT"
