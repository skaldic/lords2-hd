#!/usr/bin/env bash
# Run selftest.exe against ../dplayx.dll under Wine in a throwaway lotr2-harness-mp:1 container (no game, no Windows
# Firewall prompt). Usage: bash run_wine.sh [arg]   (arg = selftest's expected version, default 9000).
# Output: selftest console on stdout; the run's dplayx.log is copied to wine-dplayx.log beside this file.
set -u
export MSYS_NO_PATHCONV=1
D="$(cd "$(dirname "$0")" && pwd -W 2>/dev/null || pwd)"
ARG="${1:-9000}"
docker run --rm -v "$D/..:/src:ro" -v "$D:/out" --entrypoint sh lotr2-harness-mp:1 -c "
  mkdir -p /t && cp /src/dplayx.dll /src/test/selftest.exe /t/ && cd /t &&
  (WINEDLLOVERRIDES=dplayx=n timeout 600 xvfb-run -a wine selftest.exe $ARG; echo \"exit \$?\") 2>&1 | grep -v '^[0-9a-f]*:fixme' ;
  cp /t/dplayx.log /out/wine-dplayx.log 2>/dev/null; true"
