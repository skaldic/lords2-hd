# Lords of the Realm II HD

An unofficial fan patch for the GOG version of **Lords of the Realm II**. It makes the game fill a
modern monitor and shows its art at double detail. You need your own copy of the game; the patch
changes none of its files.

Current version: **1.2.0**.

**New in 1.2.0: press F8 in the game** for the settings panel. Switch the AI art on or off, choose
sharp (Catmull-Rom) or soft upscaling, and more: most changes show at once and are saved for next time.

![County map](https://github.com/skaldic/lords2-hd/releases/latest/download/map.jpg)

| | |
|---|---|
| ![Merchant](https://github.com/skaldic/lords2-hd/releases/latest/download/merchant.jpg) | ![Armoury](https://github.com/skaldic/lords2-hd/releases/latest/download/armoury.jpg) |
| ![Battle](https://github.com/skaldic/lords2-hd/releases/latest/download/battle.jpg) | ![Kingdom overview](https://github.com/skaldic/lords2-hd/releases/latest/download/overview.jpg) |

The screenshots are hosted on the release: this repository holds no game art.

## What it does

- Runs fullscreen at your monitor's resolution, without switching its display mode
- Fills widescreen monitors: the county map, the kingdom overview and battles show more of the world,
  and the full-screen paintings (merchant, armoury, castle builder, skirmish and score screens) are
  widened
- Battles can zoom out to show four times the field
- Double-detail paintings, ground, buildings, troops, noble portraits, minimap, panels and lettering
- Sharp Catmull-Rom upscaling, with the AI art or without it (the original art, scaled cleanly)
- An in-game settings panel on F8
- Foam rolling in along the coast on the county map
- Message scrolls, yes/no questions and the end-of-battle scroll centred over the map or battlefield
- A smaller mouse cursor
- Tutorial scrolls shown once, not every time a game is started or loaded
- Leaves the CPU mostly idle: the original game redraws thousands of times a second; the patch runs
  it at 120 frames a second with the game's own speed unchanged
- Plays the disc-quality videos and the remastered soundtrack if you add them (below)
- Every feature can be switched off, in the F8 panel or in `lords2hd.ini`

## Install

Download `Lords2-HD-1.2.0.zip` from the [releases page](https://github.com/skaldic/lords2-hd/releases)
and unzip it into the game's folder, the one holding `Lords2.exe`. Start the game as usual. To
remove the patch, delete `ddraw.dll`, `lords2hd.ini`, `lords2hd.log` and the `lords2hd` folder.

The patch is a `ddraw.dll` the game loads by itself. If the game folder already holds a `ddraw.dll`
from another wrapper, this one replaces it. The download is about 450 MB and unpacks to about
900 MB of art.

Updating from 1.1.1: replacing `ddraw.dll` with the one attached to the release is enough (the art
has not changed). From an older version, unzip the new zip over it.

## Better videos and music (optional)

These are not part of the download; the patch uses them when you add them.

- **Videos:** the original discs held sharper, full-height videos than the GOG version. A fan made
  them available in the
  [SMK_HIGH thread on the GOG forum](https://www.gog.com/forum/lords_of_the_realm_lords_of_magic_series/lords_of_the_realm_ii_gog_smk_high_patch_official_higher_quality_videos).
  Copy its `.smk` files over the ones in the game's `videos` folder (keep a backup). The larger event
  clips (the hanging, the jail, the merchant cart) then fit their frame and show their full detail.
- **Music:** in 2020 the composer, Keith Zizza, released a remastered version of the soundtrack. Put
  its MP3 files, as named in that release (for example `07. Scroll 1 (1996, Remastered).mp3`), in a
  folder `lords2hd\music` inside the game's folder. At the next start the patch converts them to
  WAV in the background and the game plays them instead of its own 11 kHz tracks. "Siege Pack Setup
  2" is used for the game's third setup tune; the intro, credits, trailer and win tracks are not
  used (the game plays those sounds from its videos). WAV files named like the game's own
  (`Battle1.wav`, `Scroll3.wav`, ...) work too.

## Settings

Press **F8** in the game: a panel lists the settings; click a line (or use the arrow keys and Enter)
to change it. Most changes apply at once; those marked * (widescreen, battle zoom-out) after a
restart. Everything is saved in `lords2hd.ini` next to the exe; [hd/INSTALL.txt](hd/INSTALL.txt)
lists all its keys and is also the readme inside the zip.

## Requirements

- The GOG version of Lords of the Realm II
- Windows, and a screen of 720p (1280x720) or larger; it fills any size from there up
## About the art

The HD art is the game's own art enlarged with an AI upscaler (Real-ESRGAN). The new left and right
parts of most widened paintings were painted by an AI image model (FLUX.1 Fill) from the game's own
pictures; the original 4:3 picture is unchanged in the middle. Where a model's result did not look
like the original (the greatest-nobles battlement, the menu stone), the original's own stonework is
repeated instead. See [NOTICE.md](NOTICE.md).

## Building it yourself

The repository holds only original code: nothing extracted from the game is in it. The scripts in
`tools/` rebuild the art from your own copy of the game.

- `hd/build.ps1 -Install` builds `ddraw.dll` (32-bit, llvm-mingw) and copies it to the game folder
- `tools/build_screens.ps1`, `build_tiles.ps1`, `build_sprites.ps1`, `build_battle_ground.ps1`,
  `likeness.ps1` and `build_rooms.ps1` make the art in `<game>\lords2hd`
- `tools/expand_sides.ps1` and `expand_all.ps1` paint the sides of the widened screens
- `tools/make_release.ps1 -Version x.y.z` makes the release zip
- External tools (llvm-mingw, Real-ESRGAN, stable-diffusion.cpp and its models) are found through
  the `L2HD_TOOLS` environment variable, or `tools\external`
- `NOTES.md` holds the reverse-engineering notes

Testing uses a hidden, muted mode (`[Debug] Remote=1` in the ini) driven by `hd/bgdrive.ps1`, so
it never takes the mouse or the speakers.

## Bugs

After a crash, keep `lords2hd.log` from the game's folder straight away: it is rewritten at each
start. Say which monitor you use and whether another `ddraw.dll` wrapper or an overlay is running.

## License

Code and tools: [PolyForm Noncommercial 1.0.0](LICENSE.md). The art is not covered by it; see
[NOTICE.md](NOTICE.md). This project is not affiliated with or endorsed by the owners of the game.
