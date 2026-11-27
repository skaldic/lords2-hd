# Credits

This folder is the LotR2 Fix multiplayer source, by members of the Lords of the Realm II community Discord:

- **Wolfe**: the replacement DirectPlay layer (`dplayx/`): network transport, per-turn sync check and
  resync, host checks on every message, host hand-over, the timing and desync fixes in the game
  (`exepatch.c`, `mpfix_patches.h`, `victory.c`), and the LotR2 Fix ddraw modules `mpmenu` and `mpwindow`.
- **Socius**: GameRanger support (the host-ready call and room auto-setup in `dplayx/dplayx.c`), and the
  v12 diagnostics and session logs (`diag.c`, `sesslog.c`).

Imported from Socius's LotR2-Fix-Multiplayer-Source-0.04 (Wolfe's 0.03 plus the GameRanger changes),
with Wolfe's and Socius's permission to include it in this repository. Lords of the Realm II HD added
`dplayx/build.ps1` (a Windows build with llvm-mingw) and this file. Since then the layer is developed as
part of Lords2HD only: the `ddraw/mpmenu` and `ddraw/mpwindow` modules and the title-menu exports of
`dplayx.dll` were removed (Lords2HD does not ship the LotR2 Fix ddraw), and its settings moved into
`lords2hd.ini`. The 0.04 source is in this repository's history.

Keep the authors' names with this code. Lords of the Realm II is © its rights holders; this is a
non-commercial fan project.
