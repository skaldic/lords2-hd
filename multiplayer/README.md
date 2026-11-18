# Multiplayer layer

Lords2HD's multiplayer: `dplayx/` builds our own `dplayx.dll`, a drop-in DirectPlay over plain TCP. It replaces the
Windows / Wine DirectPlay and fixes the game's multiplayer: per-turn sync check and resync, host checks on every
message, host hand-over when the host leaves, timing fixes in the exe (`mpfix`, on by default), GameRanger support and
per-game session logs. Port 47700. Same target as the rest of the mod: the GOG / Steam `Lords2.exe` of 5 Jun 1997.

It ships only as part of Lords2HD (`tools/make_release.ps1` builds and stages it). The HD ddraw provides what the
LotR2 Fix ddraw modules used to (the game keeps running when unfocused: `RunInBackground`), and calls two exports:
`LF_NetSessionOpen` and `LF_NetLogDir`.

## Building

- Windows: `dplayx\build.ps1` (llvm-mingw, the same toolchain as `hd\build.ps1`).
- Linux / Docker: `bash dplayx/build.sh` (mingw-w64 in a Debian container).
- `dplayx/test/` holds the self-test (`selftest.c`, the DLL without the game; `run_wine.sh` runs it under Wine).

## Players

All players in a game need the same `dplayx.dll` build, the same exe and the same `mpfix` setting. The handshake
refuses a mismatch and says so in `dplayx.log`.

## Credit

Written by Wolfe and Socius (LotR2 Fix); see `CREDITS.md`.
