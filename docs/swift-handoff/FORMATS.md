# File formats

All values are little-endian. Everything here was verified by decoding the GOG release's files
(21,340 of 21,344 frames decode; the four that don't are `*grid*` pictures, probably hit-test maps).

## Palettes: `.256`

768 bytes: 256 RGB triples with 6-bit components (0..63, the VGA DAC). To 8 bits:
`(v << 2) | (v >> 4)`.

A picture file has no palette of its own. Which palette it was drawn for:

| Pictures | Palette |
|---|---|
| County map: ground, towns, castles, roads, flags, map sprites, panels, icons, faces | `Base01.256` |
| Full-screen paintings (`Merchant.pl8`, `Armoury.pl8`, `Gateway.pl8`, ...) | the `.256` of the same name |
| Armoury items and the large troop pictures (`arm_*`, `trp_*`) | `Armitems.256` |
| Battle ground, battle panel, missiles, siege plans (`t32_*`, `batfiel2`, `misc_bat`, ...) | `T32_bat1.256` |
| Skirmish screens and their fighting pair (`misc_ske` frames 33-44) | `SKIRMISH.256` |

Getting this wrong gives speckled, wrongly coloured art that still looks "almost right" (the troop
pictures were built with `Base01.256` for a week). To find a picture's palette, measure the average
colour step between neighbouring pixels under each palette: the right one is the smoothest
(`Pl8.Roughness`).

The battle troop sprites (`a2<colour>_<unit>`, `a3...`) are an open question: no single palette
makes them smooth in a viewer, yet they draw correctly in the game, so the blitter probably maps
them through a table. Colours: w r y k p b g. Units: arch cros knig mace pike psnt swor.

## Pictures: `.pl8`

Header, 8 bytes: `u16 type` (0 or 2 = raw / diamonds, 1 = row commands), `u16 frames`, `u16 ?`,
`u8 0`, `u8 ?`.

Then one 16-byte entry per frame: `u16 width`, `u16 height`, `u32 data offset`, `i16 x`, `i16 y`,
`u32 extra`.

Frame data comes in three kinds. The header's type is only a hint; decide per frame from the bytes
the frame has (up to the next higher offset):

- **Raw**: `width * height` palette indices.
- **Row commands** (type 1): for each row, until the row is full: `00 n` skips n transparent
  pixels; `n` (1..127) is followed by n literal pixels. If a type-1 frame fails to decode and has
  exactly `width * height` bytes, it is raw.
- **Diamond** (isometric map tile): only the diamond is stored, row by row, each row centred. Rows
  widen by a fixed step from 2 pixels to the full width, then narrow again. Map tiles are 58x30
  (2, 6, ... 58, 58, ... 6, 2 = 900 bytes); the zoomed-out map uses 10x6. A 58x30 frame is always a
  diamond, sometimes with extra overhang data after it.

Index 0 is transparent in sprites.

## File sets

- County map, by season a-d: `base1a..d` (ground), `mtns1*`, `roads1*`, `town1*`, `castle1*`,
  `flags1*`, and `sprite1a/b`. The `2` sets (`base2a`, ...) are the zoomed-out map (10x6 tiles).
  The season files `base2b-d` and `flags1b-d` appear unused by the game.
- Minimaps: `map01-06`, `map11-15`, 128x128. Pixels holding 10..13 are four placeholder shades that
  the game repaints in each county's colour (10 is the border).
- Battles: 32-pixel cells; ground in `t32_*`; the field is about 80 x 80 cells.
- Text is drawn from font pictures (`fnt_8`, `fntl2_9`, `font_10`, `fntl2_14`, `fntl2_22`,
  `system*.pl8`), never by the operating system.
- Video: Smacker (`.smk`).

## HD art files the patch builds

These are plain, uncompressed and easy to read from Swift. They live in `<game>\lords2hd\`.

**`screens\<name>.bin`** â€” a painting at double size: 768 bytes (the palette the art was made
from), then 1280 x 960 pixels, 4 bytes each, B G R X.

**`wide\<name>.bin`** â€” the same painting widened to 16:9: five `i32` (`width` = 854, `cutL`,
`cutR`, `floorY`, `barY`), then `2*width` x 960 pixels B G R X. The original picture sits untouched
in the middle (columns 214..1493 of 1708). Two files (`skirmish`, `skircust`) carry extra animation
data after the pixels.

**`sprites\<file>.bin`** and **`tiles\<file>.bin`** â€” one per `.pl8` file: `u32 frames`; then
`u32 offset` per frame (0 = none); at each offset `u16 w`, `u16 h`, `w*h` bytes of the original
palette indices (0 = transparent), then `2w` x `2h` pixels B G R X. The index block is the shape:
a pixel of the 2x art belongs to the sprite where the index under it (x/2, y/2) is not 0.
`sprites\<file>.pal` (768 bytes), where present, is the palette the art was made with.

Sizes: about 900 MB in all (sprites 690 MB). For a Swift game, converting to PNG or a compressed
texture format at build time would shrink this a great deal.
