#!/bin/bash
# Build selftest.exe (32-bit) with the same mingw image as ../build.sh. Output: selftest.exe beside this file (gitignored).
#   bash build.sh      then run it in a folder holding dplayx.dll:  selftest.exe 8000
set -e
export MSYS_NO_PATHCONV=1
D="$(cd "$(dirname "$0")" && pwd -W 2>/dev/null || pwd)"
docker run --rm -v "$D:/src" -w /src lotr2fix-mingw:bookworm \
  i686-w64-mingw32-gcc -O2 -Wall -o selftest.exe selftest.c -lws2_32 -static-libgcc
ls -l "$D/selftest.exe"
