# Lords of the Realm II — reverse-engineering notes

Target: GOG `Lords2.exe`, 1,031,680 bytes, PE timestamp 1997-06-05 (0x3396EFAA),
SHA-256 `6FEC043E59ED3D7AD5DE5B313093B8E2EE2D7832CCC224705E010BFE164D004A`.
MSVC 4.2, image base 0x400000, has `.reloc`, **not** large-address-aware.

## Rendering

- DirectDraw (`DirectDrawCreate` only), exclusive 640x480x8 with a 256-colour palette.
  No GDI drawing: all text is drawn from font pictures (`fnt_8`, `fntl2_9`, `font_10`,
  `fntl2_14`, `fntl2_22`, `system*.pl8`).
- No `ddraw.dll` wrapper ships with the GOG release.
- The game writes `status.txt` next to the exe; its messages are in the exe and anchor the
  functions below (`strings.csv` from `StringRefs.java` maps every string to its function).

| Address | What | Anchor |
|---|---|---|
| 0042e5c0 | DirectDraw init: cooperative level, set mode | `DD SetCooperative Level`, `DD Set resolution` |
| 0042ec91 | Surface creation (one VRAM / VRAM + system) | `DD Creating one Vram only` |
| 0042eb7c | Clear surfaces | `DD Clearing Fill failed` |
| 0042eaa6 | DirectDraw release | `DD Released` |
| 0042f1df | Build and set the DirectDraw palette | `Palette create failed` |
| 004b284e | Gfx enable / flipping surface | `GFX DirectDraw enabled` |
| 004b2730 | Restore lost surfaces | `Can't restore front buffer` |
| 004b29be | Window messages (move/activate/minimise) | `Window moved` |
| 0042da18.. | Smacker video playback | `SMK starting smack` |
| 004071a0.. 0040a9b0 | Sprite blitters: top_it, gen_frame, gen_blank, mos_frame, mos_blank, mos_24blank, gen_sprite, write_sprite, write_c_sprite, place_sprite, w_gen_sprite, w_gen_h_sprite, w_gen_f_sprite | `ERR:<name> bad data` |
| 004af8d9 | Load file into buffer (name, dest, max size, ?) | used by every loader |
| 004afab9 | Write a status.txt line | |
| 00499859 | Load system gfx (table at 0x4D9F48: 13 x {char name[16]; u32 size}) | `Sys Gfx loaded OK` |
| 00499a1c | Load system2.pl8 / system.pl8 | `Sys2 Gfx loaded OK` |
| 004984dc | Load county data | `Data loaded OK` |
| 004987b7 | Load battle data | `BATTLE Data loaded OK` |
| 0040d1bc | Select a screen window/viewport (x, y, kind) | |
| 004b3ec0 / 004b3f0a | Copy a block to / from the frame | |

Globals: `DAT_004ea1a8` frame buffer pointer, `DAT_004eb274` frame pitch (= screen width),
`DAT_004ea8a0` screen palette as loaded from a `.256` (768 bytes, 6-bit),
`DAT_004ea1b0` palette fed to DirectDraw, `DAT_004eabec` scratch picture buffer.

File sets (exe .data, 20-byte records {char name[16]; u32 size}): county sets at 0xD8250
(`base1a..d` seasons a-d, zoom 1 and 2), battle sets at 0xD8750 (terrain, then troops
`a2<colour>_<unit>` / `a3...` per player colour w r y k p b).

## Picture files

See `tools/Pl8.cs`. Header: u8 type, u8 flags, u16 frames, u16 ?, u8 0, u8 ?; then 16-byte
frame entries {u16 w, h; u32 offset; i16 x, y; u32 extra}. Frames are raw rows, row
commands (`00 n` skip, `n` literal), or isometric diamonds (58x30 / 10x6), chosen by the
bytes each frame has. 21,340 of 21,344 frames decode; the 4 left are `*grid*` pictures
(probably hit-test maps).

## Proxy DLL (hd/lords2hd.c) — what testing showed

- The game only ever takes its fullscreen path on a modern desktop (`DAT_0053f228` starts at 1;
  it is the windowed flag's opposite). Its window is `WinLords2Class`, WS_POPUP, sized from
  `GetSystemMetrics(SM_CXSCREEN/CYSCREEN)`.
- In fullscreen its window rectangle `DAT_004ea180` is set from SM_CXSCREEN/CYSCREEN on
  WM_MOVE/WM_SIZE and scales mouse positions and places frame copies: the DLL reports 640x480.
- `GetDeviceCaps(BITSPIXEL)` != 8 shows a "Non 256 colour display" warning at game start: the DLL
  reports 8.
- Esc on the main menu quits the game (WM_KEYDOWN VK_ESCAPE sets `DAT_005aeb68`).
- The main loop spins at 100% of one core (stock behaviour). The game re-copies its frame ~20
  times a second while idle; the presenter skips unchanged frames (~3 ms per real present at
  5120x2880).
- Cursors: class cursor is NULL; the game picks one of 8 resource cursors (`FUN_004b1d??`,
  `SetCursor`). Windows hands them over at 64x64 on a 200% desktop.
- **Starting a new game overwrites `lastturn.sav` and `lords2.inf`.** Back them up before any
  test run and restore after.

## Wide frame (phase 3, step 1 — working, Wide=0 by default until screens are widened)

The frame width is `DAT_004eb274` (set once at 0x4b2371), but 640 is also baked into code:

- 4,323 `[EDI + rows*640 + col]` operands in the blitters (rows 1..31, col -63..63): EDI is the
  frame. `[ESI + ...]` is the *source* picture — never rewrite it. Table: `PitchSites.java` →
  `tools/make_pitch_header.ps1` → `hd/pitch_sites.h`.
- Row skips `0x280 - w` in the 9 sprite blitters (`MOV EAX,0x280`), `ADD EDI,0x27f` in 004b3fe0.
- `y*640` as `LEA r,[r+r*4]; SHL r,7` (`tools/find_mul640.ps1`): only the rectangle darkener
  004b13ed (2 sites) is the frame; 00479e90 is the battle grid, 004c6340 is gmtime.
- Callers pass `640 - width` as the row skip to 004b3ec0/004b3f0a (save/restore under windows).
- Flat 640x480 writes: the raw screen loader 00408fcb (17 painted screens) and the LBM decoder
  004b061a — both wrapped to lay rows out at the frame width.
- Smacker destination pitch (two `PUSH 0x280`), the three 307,200-byte whole-frame loops.
- The game scales mouse x by frame width / SM_CXSCREEN, so SM_CXSCREEN reports the frame width.
- Not the frame, left alone: battle grid rows of 640 cells (0047c4ba, 0047dfe0, 00490616,
  00491b1f, 00479e90), screen-x maths (0042e50d, 0042e5c0, 00485675, 004b29be), text centred on
  640 (0041a166, 00421d09), the 640 clip of full-screen pictures (gen_frame/mos_frame).
- Dead code (no callers): 004b5ee8, 004b6034, 004b6075, 004b6112, 004b4085, 004b40b2, 004b40f9.
- Not handled yet: 00408f1a (debug screenshot writer), 0040902b (demo1.pl8 loader).

## Open questions

- Battle troop sprites (`a2*`, `a3*`) are not plain palette indices: no palette (including the
  battle palette logged in game, pal_005) makes them smooth, yet they draw correctly in game. The
  sprite blitter must map them through a table (team colours?). Needed for HD art (phase 4).
- `title.pl8` is never loaded by name (the title is `imptitle.smk`).
- Format of the `*grid*` pictures.
