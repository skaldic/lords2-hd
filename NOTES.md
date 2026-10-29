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

## Open questions

- Palette of battle troop sprites and of `title.pl8` / `troop1.pl8`: none of the 27 `.256`
  files fits. `title.pl8` is never loaded by name (the title is `imptitle.smk`). Plan: log
  the live palette from the proxy DLL when each picture loads.
- Format of the `*grid*` pictures.
