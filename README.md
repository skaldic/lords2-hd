# Lords of the Realm II HD

An unofficial fan patch for the GOG version of **Lords of the Realm II**. It makes the game fill a
modern monitor and shows its art at double detail. You need your own copy of the game; the patch
changes none of its files.

<!-- Screenshots go here once a release exists: they are hosted on the release, not in this repository. -->

## What it does

- Runs fullscreen at your monitor's resolution, without switching its display mode
- Fills widescreen monitors: the county map, the kingdom overview and battles show more of the world,
  and the full-screen paintings (merchant, armoury, castle builder, menus) are widened
- Double-detail paintings, ground, buildings, troops, minimap, panels and lettering
- Foam rolling in along the coast on the county map
- A smaller mouse cursor
- Every feature can be switched off in `lords2hd.ini`

## Install

Download the zip from the releases page and unzip it into the game's folder, the one holding
`Lords2.exe`. Start the game as usual. To remove the patch, delete `ddraw.dll`, `lords2hd.ini`,
`lords2hd.log` and the `lords2hd` folder.

The patch is a `ddraw.dll` the game loads by itself. If the game folder already holds a `ddraw.dll`
from another wrapper, this one replaces it.

## Settings and known limits

See [hd/INSTALL.txt](hd/INSTALL.txt), which is also the readme inside the zip.

## About the art

The HD art is the game's own art enlarged with an AI upscaler (Real-ESRGAN). The new left and right
parts of the widened paintings were painted by an AI image model (FLUX.1 Fill) from the game's own
pictures; the original 4:3 picture is unchanged in the middle. See [NOTICE.md](NOTICE.md).

## Building it yourself

The repository holds only original code: nothing extracted from the game is in it. The scripts in
`tools/` rebuild the art from your own copy of the game.

- `hd/build.ps1 -Install` builds `ddraw.dll` (32-bit, llvm-mingw) and copies it to the game folder
- `tools/build_screens.ps1`, `build_tiles.ps1`, `build_sprites.ps1`, `build_battle_ground.ps1` and
  `build_rooms.ps1` make the art in `<game>\lords2hd`
- `tools/make_release.ps1 -Version x.y.z` makes the release zip
- External tools (llvm-mingw, Real-ESRGAN, stable-diffusion.cpp and its models) are found through
  the `L2HD_TOOLS` environment variable, or `tools\external`

## Bugs

After a crash, keep `lords2hd.log` from the game's folder straight away: it is rewritten at each
start. Say which monitor you use and whether another `ddraw.dll` wrapper or an overlay is running.

## License

Code and tools: [PolyForm Noncommercial 1.0.0](LICENSE.md). The art is not covered by it; see
[NOTICE.md](NOTICE.md). This project is not affiliated with or endorsed by the owners of the game.
