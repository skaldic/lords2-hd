# Lords of the Realm II HD

An unofficial fan patch for the GOG and Steam versions of **Lords of the Realm II**. It makes the
game fill a modern monitor and shows its art at double detail. You need your own copy of the game; the patch
changes none of its files.

Current version: **1.3.0**.

**New in 1.3.0:** an installer, restored voices, the disc-quality videos, an army scroll, WASD
scrolling, `,` and `.` to jump between your counties and armies, fresh diplomatic letters, and the
settings on **F10** as one of the game's scrolls.

[Watch the 40-second video of what's new](https://github.com/skaldic/lords2-hd/releases/latest/download/lords2hd-1.3.0.mp4)

![The F10 settings](https://github.com/skaldic/lords2-hd/releases/latest/download/f10.jpg)

![A diplomatic letter, with one of the new texts](https://github.com/skaldic/lords2-hd/releases/latest/download/letter.jpg)

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
- An in-game settings panel on F10, drawn as one of the game's scrolls
- The dialogue and unit voices restored to full quality (with Lords2Resounded; see below)
- An army scroll beside the side panel while one of your armies is selected: its troops and men
- W, A, S, D scroll the map and the battlefield; `,` and `.` (or the `<` `>` buttons) jump between
  your counties, or your armies
- Fresh diplomatic letters, dealt like a shuffled deck, where the game left the letter empty
- Foam rolling in along the coast on the county map
- Message scrolls, yes/no questions and the end-of-battle scroll centred over the map or battlefield
- A smaller mouse cursor
- Tutorial scrolls off, shown once, or every time, as you choose
- Leaves the CPU mostly idle: the original game redraws thousands of times a second; the patch runs
  it at 120 frames a second with the game's own speed unchanged
- Plays the disc-quality videos (included), and the hi-fi soundtrack if you add it (below)
- Every feature can be switched off, in the F10 panel or in `lords2hd.ini`

## Install

Download `Lords2-HD-1.3.0-setup.exe` from the [releases page](https://github.com/skaldic/lords2-hd/releases)
and run it. It finds the GOG or Steam game by itself (or asks for the folder holding `Lords2.exe`),
adds the patch, and leaves the game's own files alone. Uninstall it from Windows' Apps list. Close the game
first. Windows may warn that the publisher is unknown: the installer is not code-signed.

The patch is a `ddraw.dll` the game loads by itself, an ini with the settings, and `lords2hd.pak`,
one file with the HD art, the restored voices and the videos. If the game folder already holds a `ddraw.dll`
from another wrapper, this one replaces it. Updating from an older version: run the installer over
it; your settings are kept and the loose art folders of the zip versions are removed.

Rather unzip by hand? `Lords2-HD-1.3.0.zip` holds the same files: unzip it into the game's folder.

## Videos and music

The videos come with the patch; the hi-fi music you add yourself (it is the composer's, not ours to give).

- **Videos:** now part of the download: the disc-quality videos, sharper and full-height (the GOG
  version's are interlaced), play from `lords2hd.pak`. The larger event clips (the hanging, the jail,
  the merchant cart) fit their frame and show their full detail.
- **Music:** the composer, Keith Zizza, has published the original recordings of the soundtrack,
  far clearer than the game's 11 kHz tracks. They are his, so they are not in the download, but
  adding them takes a minute.

### Adding the hi-fi music

**With the MP3s** of the 2020 "Lords of the Realm II (Remastered)" release:

1. Get the MP3s of that release (the composer's own, for example from his Bandcamp or YouTube).
2. In the game's folder (the one holding `Lords2.exe`), open `lords2hd\music`. The installer makes
   it; otherwise create it.
3. Put the MP3s in, keeping their names (for example `07. Scroll 1 (1996, Remastered).mp3`).
4. Start the game. The patch converts them in the background, about a minute the first time,
   and cuts each one at the same points as
   [Lords2Resounded](https://github.com/gqzomer/Lords2Resounded) does, so the tracks loop seamlessly
   instead of fading out. From the next track on, the game plays them.

"Siege Pack Setup 2" is the game's third setup tune. The intro, credits, trailer and win tracks are
not used (the game plays those from its videos). To go back to the game's own music, delete the
files in `lords2hd\music`.

**With Lords2Resounded:** run [its tool](https://github.com/gqzomer/Lords2Resounded) on a copy of the
game's sound files; it fetches the composer's recordings and edits them for seamless loops. Copy the
music WAVs it makes (`Battle1.wav`, `Scroll3.wav`, ...) into `lords2hd\music`; the patch plays them
as they are.
## Settings

Press **F10** in the game: a scroll lists the settings on two tabs, Display and Quality of life (Tab
switches them); click a line (or use the arrow keys and Enter) to change it, right-click to close.
Most changes apply at once; those marked * (widescreen, battle zoom-out) after a restart.
Everything is saved in `lords2hd.ini` next to the exe; [hd/INSTALL.txt](hd/INSTALL.txt) lists all
its keys and is also the readme that comes with the patch.

## Requirements

- The GOG or Steam version of Lords of the Realm II (they share the same game files)
- Windows, and a screen of 720p (1280x720) or larger; it fills any size from there up

## About the art

The HD art is the game's own art enlarged with an AI upscaler (Real-ESRGAN). The new left and right
parts of most widened paintings were painted by an AI image model (FLUX.1 Fill) from the game's own
pictures; the original 4:3 picture is unchanged in the middle. Where a model's result did not look
like the original (the greatest-nobles battlement, the menu stone), the original's own stonework is
repeated instead. The voices are the game's own recordings, restored with
[Lords2Resounded](https://github.com/gqzomer/Lords2Resounded) with its author's permission. The
videos are the game's own disc versions, unchanged. See [NOTICE.md](NOTICE.md).

## Thanks

- **[Lords2Resounded](https://github.com/gqzomer/Lords2Resounded)** by gqzomer: the restored dialogue
  and unit voices in this release were made with its pipeline (LavaSR and DPDFNet), used with its
  author's permission, and its hand-picked loop points make the hi-fi music loop seamlessly. Go give
  it a star.
- The member of the GOG community who shared the disc-quality videos in the SMK_HIGH thread.
- Keith Zizza, for publishing the original recordings of his soundtrack.

## Building it yourself

The repository holds only original code: nothing extracted from the game is in it. The scripts in
`tools/` rebuild the art from your own copy of the game.

- `hd/build.ps1 -Install` builds `ddraw.dll` (32-bit, llvm-mingw) and copies it to the game folder
- `tools/build_screens.ps1`, `build_tiles.ps1`, `build_sprites.ps1`, `build_battle_ground.ps1`,
  `likeness.ps1` and `build_rooms.ps1` make the art in `<game>\lords2hd`
- `tools/expand_sides.ps1` and `expand_all.ps1` paint the sides of the widened screens
- `tools/make_pak.ps1` packs the art into `lords2hd.pak`; `tools/make_release.ps1 -Version x.y.z`
  makes the installer (Inno Setup 6, `tools/lords2hd.iss`) and the zip
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
