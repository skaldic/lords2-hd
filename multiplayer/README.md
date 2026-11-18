# LotR2 Fix - multiplayer source

The multiplayer half of the LotR2 Fix. This zip is separate from the zoom and UI one, so a mod can ship either
without the other. Same target: the GOG `Lords2.exe` of 5 Jun 1997.

`bin/dplayx.dll` is the exact file in the LotR2 Fix 0.02 / 0.03 player zips (SHA-256 starts `dc6edb5a4f5efe7c`), built
from this `dplayx/` source. A rebuild differs from it in a few PE timestamp bytes, and players must all run the same
file, so ship this one unless you change the code.

## What is in it

| Folder | What it is |
|---|---|
| `dplayx/` | our own `dplayx.dll`, a drop-in DirectPlay over plain TCP. It replaces the Windows / Wine DirectPlay and fixes the game's multiplayer: per-turn sync check and resync, host checks on every message, host hand-over when the host leaves, and timing fixes in the exe (`mpfix`, on by default). Port 47700 |
| `ddraw/mpmenu/` | a `ddraw.dll` module: Multiplayer screens drawn inside the title menu (Create a game, Search local games, Connect by address) instead of Windows dialogs, plus the in-game "Exit the game?" screen. The Multiplayer screens need our `dplayx.dll` |
| `ddraw/mpwindow/` | a `ddraw.dll` module: in a multiplayer session the game never pauses when its window is unfocused, minimised or dragged. One paused player stalls everyone |

`dplayx.dll` is a separate file from `ddraw.dll`. It works with the stock ddraw, with our zoom/UI ddraw, and with
your Mod Menu ddraw. We ran your Mod Menu v1.1 ddraw with this dplayx on two real PCs: 212 of 212 sync checks
matched and a battle was played.

## Building

- `dplayx/`: `bash build.sh` (Docker, mingw-w64). `test/` holds the self-test (`selftest.c`, 31 checks without
  the game; `run_wine.sh` runs it under Wine).
- `ddraw/mpmenu/` and `ddraw/mpwindow/`: copy both folders into the `ddraw/` folder of the zoom/UI zip and run
  `bash build.sh` there. `out/ddraw.dll` then has every module. They are plain module folders
  (`ddraw/MODULES.md` in the other zip explains the module hooks).

## With your Mod Menu

- **`mpmenu` and your Mod Menu can not both be on.** Both hook the same two title renderers (0x41eaa3 buttons,
  0x41ea14 panel). Pick one per build. Without `mpmenu` the stock Multiplayer dialogs run, and our dplayx works
  with those too.
- `mpwindow` patches nothing in the exe; it only acts on window messages.
- Your MP Sync Debugger hooks and our dplayx exe patches sit side by side without trouble (yours at 0x4778a0, ours at
  0x4778a9; tested on two real PCs, see above).

## Players

All players in a game need the same `dplayx.dll` build, the same exe and the same `mpfix` setting. The handshake
refuses a mismatch and says so in `dplayx.log`.

## Credit

Our own code, written by Matthew Wolfe (LotR2 Fix). `mpmenu` draws the title screen the way your Mod Menu does, and
it learned that from you. Its source file says so.
