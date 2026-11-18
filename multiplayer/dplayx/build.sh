#!/bin/bash
# Cross-compile dplayx.dll (32-bit) with mingw-w64 in a throwaway Debian container.  Output: dplayx.dll beside this file.
# Hardening (review F25): ASLR (--dynamicbase), DEP (--nxcompat), stack canaries (libssp linked in statically).
#   bash build.sh
set -e
export MSYS_NO_PATHCONV=1
D="$(cd "$(dirname "$0")" && pwd -W 2>/dev/null || pwd)"
# same compiler image as ../ddraw/build.sh, built once and reused
IMG=lotr2fix-mingw:bookworm
docker image inspect "$IMG" >/dev/null 2>&1 || printf 'FROM debian:bookworm-slim
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc-mingw-w64-i686 binutils-mingw-w64-i686 && rm -rf /var/lib/apt/lists/*
' | docker build -q -t "$IMG" -
docker run --rm -v "$D:/src" -w /src "$IMG" sh -c '
  i686-w64-mingw32-gcc -O2 -Wall -fstack-protector-strong -shared -o dplayx.dll dplayx.c dplayx.def -lws2_32     -static-libgcc -l:libssp.a -Wl,--kill-at,--dynamicbase,--nxcompat &&
  i686-w64-mingw32-objdump -p dplayx.dll | sed -n "/Ordinal\/Name Pointer/,/^$/p;/DLL Name/p;/DllCharacteristics/p" &&
  if i686-w64-mingw32-objdump -p dplayx.dll | grep -qi "DLL Name: libssp"; then echo "FAIL: imports libssp-0.dll (would not load on a player PC)"; exit 1; fi'
ls -l "$D/dplayx.dll"
# F25: the hash every report and rig row quotes
sha256sum "$D/dplayx.dll"
