# Lords of the Realm II HD

An unofficial fan patch for the GOG and Steam versions of **Lords of the Realm II**. It fills a
modern widescreen monitor, shows the game's art and videos at double detail, and fixes how battles
play. You need your own copy of the game; the patch changes none of its files.

Current version: **1.6.0** · [Download](https://github.com/skaldic/lords2-hd/releases/latest) ·
[Watch what's new (67 s)](https://github.com/skaldic/lords2-hd/releases/latest/download/lords2hd-1.6.0.mp4) ·
[Community Discord](https://discord.gg/uBzaMGjUR)

![The armoury, widened](https://github.com/skaldic/lords2-hd/releases/latest/download/armoury.jpg)

## New in 1.6.0

- **Enemy tactics: Hard and Legendary** (F10 → Battles). The computer stops switching between attack
  and defence every few seconds, keeps its archers behind its soldiers, charges together and storms a
  bridge you hold instead of pouring onto it. Legendary adds picked targets, regrouping and unit
  counters (pikemen against knights, swordsmen against pikemen). Normal, the game's own, stays the
  default; see [Game fixes](#game-fixes).
- **The Town Center follows the season**: spring green, autumn gold and winter snow, with new idle
  peasants, community art by AllYourBases and CaptainRoberts. F10 → Display → Seasonal Town
  Center; Off shows the original.
- **The mouse wheel zooms**: on the county map it shows the map twice as wide and tall, and in a
  zoomed-out battle it zooms in round the cursor, up to the normal size (or 4x).
- **Battles:** a formation next to stakes, a wall, the gate or the moat holds its line; archers don't
  chase after a fight, and behind stakes they stay put; only knights and macemen go a long way round in
  Mop-Up; a box selection brings its units together again.
- Cleaner shorelines and grass edges on the battlefield.
- The arrow keys scroll the map and the battlefield, and A scrolls left in battles; the Town Center
  window no longer leaves a strip of map beside it.

![The Town Center in spring, summer, autumn and winter](https://github.com/skaldic/lords2-hd/releases/latest/download/seasons.jpg)

![The county map, and zoomed out with the mouse wheel](https://github.com/skaldic/lords2-hd/releases/latest/download/countyzoom.jpg)

## New in 1.5.0 (and 1.5.1)

- **1.5.1:** zoomed-out battles no longer freeze on their first picture, and the Town Center window
  stays still when you move peasants.

- **Battles: improved pathfinding, cohesion and engagement.** Units no longer step back and forth when a friend is in the way, an army
  sent round a forest keeps together instead of trailing in single file, idle soldiers fight the
  enemy beside them, and archers step back from a charge and slip out of melee once a soldier takes
  over. Mop-Up keeps going until the field is clear. Both sides get the same rules, and each change
  has its own switch on F10's new **Battles** tab.
- **The videos are sharper:** the intro, the credits and the castle, battle and siege videos play
  with every frame upscaled.
- **New widened paintings:** the armoury's side passages lit by its torches, the castle builder's
  brick wall, and seamless sides on the custom game, battle setup, greatest nobles and results
  screens. The town keeps the original's grain in its grass and dirt.
- **Minimap outlines** (F10 → Battles): troops on the battle minimap get a thin dark ring, so they
  stand out, the black player's especially.
- Zoomed-out battles no longer freeze while scrolling over a forest, and clicks work across the whole
  battlefield.
- Widescreen is on by default, and 4:3 no longer crashes at a yes/no question.

![Original video and the upscaled frames](https://github.com/skaldic/lords2-hd/releases/latest/download/videos.jpg)

| | |
|---|---|
| ![Castle builder](https://github.com/skaldic/lords2-hd/releases/latest/download/castle.jpg) | ![The town](https://github.com/skaldic/lords2-hd/releases/latest/download/town.jpg) |
| ![A zoomed-out battle with minimap outlines](https://github.com/skaldic/lords2-hd/releases/latest/download/battle.jpg) | ![F10 → Battles, with Enemy tactics](https://github.com/skaldic/lords2-hd/releases/latest/download/tactics.jpg) |
| ![Custom game](https://github.com/skaldic/lords2-hd/releases/latest/download/custom.jpg) | ![Battle setup](https://github.com/skaldic/lords2-hd/releases/latest/download/skirmish.jpg) |
| ![County map](https://github.com/skaldic/lords2-hd/releases/latest/download/map.jpg) | ![Kingdom overview](https://github.com/skaldic/lords2-hd/releases/latest/download/overview.jpg) |

The screenshots are hosted on the release: this repository holds no game art.

## What it does

**Picture**
- Runs fullscreen at your monitor's resolution, without switching its display mode
- Fills widescreen monitors: the county map, the kingdom overview and battles show more of the world,
  and the full-screen paintings are widened
- Battles can zoom out to show four times the field, and the mouse wheel zooms back in round the
  cursor; on the county map the wheel shows four times as much of it
- The Town Center changes with the seasons (community art)
- Double-detail paintings, ground, buildings, troops, noble portraits, minimap, panels, lettering and
  videos
- Drawn by the graphics card (Direct3D 11) and upscaled with AMD FidelityFX Super Resolution 1, with
  the HD art or without it (the original art, scaled cleanly); a locked 60 frames a second in battles
  and on the map, with the game's own speed unchanged

**Battles**
- Units wait for a friend in the way instead of turning back, and stop beside a taken place instead of
  dancing round it
- Groups ordered round a forest, river or other obstacle keep their shape and their route, and your
  groups stay where you left them (the game no longer re-forms them every few seconds unordered)
- Idle soldiers fight an enemy within a few cells (knights a little further), but never wander more
  than 4 cells from where they stood and let a fleeing enemy go, so a group can't be baited away; and
  only when they aren't outnumbered by the enemy there and coming up behind it, so a few pikemen
  holding a gap stay in it; a formation standing next to stakes, stones, water, a castle wall, the gate
  or the moat holds that line and fights only an enemy right beside it, even after a fight
- Archers shoot the enemies fighting their soldiers first, never chase after a fight, step back from soldiers charging them and
  keep shooting (behind stakes, a hedge or water they stay put), and step out of melee once one of
  their soldiers reaches their enemy
- Mop-Up hunts down every enemy it can walk to, and your orders still take over; only knights and macemen go
  a long way round for one, slower troops take the enemy at hand
- A tougher computer if you want one: Hard and Legendary enemy tactics (see Game fixes)
- Each of these on its own switch, on F10's Battles tab
- Troops outlined on the battle minimap, if you like (F10 → Battles)

**Comfort**
- An in-game settings panel on F10, drawn as one of the game's scrolls; an FPS counter on Ctrl+F10
- An army scroll beside the side panel while one of your armies is selected
- W, A, S, D scroll the map and the battlefield; `,` and `.` (or the `<` `>` buttons) jump between
  your counties, or your armies
- Fresh diplomatic letters where the game left the letter empty
- Foam rolling in along the coast; message scrolls centred over the map or battlefield; a smaller
  mouse cursor; tutorial scrolls off, once, or every time
- The dialogue and unit voices restored to full quality (with Lords2Resounded), the disc-quality
  videos included, and the hi-fi soundtrack if you add it (below)
- Leaves the CPU mostly idle: the original redraws thousands of times a second; the patch runs it at
  120
- Every feature can be switched off, in the F10 panel or in `lords2hd.ini`

## Game fixes

Bugs of the original game are fixed in single player, each with its own switch under `[Game]` in
`lords2hd.ini`:

- **Mop-Up** (`MopUpFix`, F10 → Battles → Mop-Up fix): your troops keep fighting after their first
  kill instead of standing idle, and pick the nearest enemy they can actually reach
- **Improved pathfinding** (`JitterFix`): no stepping back and forth, no swapping places over and over
- **Group cohesion** (`GroupMoveFix`): groups keep together round obstacles
- **Improved engagement** (`EngageFix`): idle soldiers and archers react to enemies close by, as
  described above
- **The computer's castles** (`CastleCapFix`): a computer lord no longer starts a castle in every
  county at once (the Bishop's royal castles everywhere)
- **Wheat** (`WheatFix`): a wheat field lost after planting costs its grain once, not twice
- **Besieged castles** (`SiegeGarrisonFix`): no army can enter or strengthen a castle under siege

Not a fix but a choice: **Enemy tactics** (`BattleAI`, F10 → Battles). Normal is the game's own battle AI.
On Hard, in field battles, the computer picks attack or defence once instead of switching every few
seconds, keeps its archers behind its soldiers (shooting, stepping back from yours, and walking up into
range when its soldiers are fighting out of their reach), sends its
nearest soldiers to help archers you reach, and moves its soldiers up together and charges together.
When it attacks a bridge or other narrow crossing you hold, it no longer pours everyone onto it at once (the
ones behind used to set off on the long way round): its soldiers wait in a line on their bank while its
archers shoot at your men there from the side, and once they're all in place they storm it together when
your men there are down to half of its soldiers (after about 10 s of shooting), when it's free, or after
about a minute when it's the stronger army.
In sieges too, its soldiers step out of your burning oil and don't walk back in while it burns (they used
to stand in it until they died).
Legendary plays like Hard, and also:
- when it attacks, its knights ride for your archers and its pikemen for your knights, no more than two
  groups on one of yours;
- shot at from afar, it keeps closing in instead of charging at once;
- a beaten group falls back behind its archers while the battle goes badly, and comes back to finish you off;
- its archers shoot your archers first, unless your soldiers are close;
- a weaker army still attacks after a long wait, instead of waiting for you forever;
- units counter each other, for both sides: pikemen hit knights twice as hard and knights hit pikemen softer,
  swordsmen are strong against pikemen and maces against knights, and knights and swordsmen cut down the
  lightly armoured (peasants, archers and macemen, who hit hard but fall fast; crossbowmen are armoured).

In multiplayer they stay off, so everyone plays the same rules, unless every player sets
`MultiplayerFixes=1`.

## Multiplayer

The download includes `dplayx.dll`, the LotR2 Fix multiplayer layer by Wolfe and Socius of the
Lords of the Realm II community. It replaces Windows' DirectPlay with its own network code over TCP
port 47700, checks every turn that all players' games agree and repairs them if not, and fixes the
game's own multiplayer desyncs. Every player needs the same `dplayx.dll`.

- The host allows the Windows Firewall prompt; the others must reach the host's TCP port 47700 (same
  network, a port forward, or a VPN such as ZeroTier, Tailscale or Radmin). To join, choose Connect
  to a game and type the host's address.
- The game keeps running when you switch windows, so nobody waits for you.
- **Multiplayer logs:** each multiplayer game writes a folder `lords2hd-mp\<date and time>\` in the
  game's folder, with what's needed to debug the session. If a game desyncs or crashes, **every
  player** zips their folder for that game and posts it on the
  [community Discord](https://discord.gg/uBzaMGjUR) or in an issue. The ten newest games are kept.

## Install

Download `Lords2-HD-1.6.0-setup.exe` from the [releases page](https://github.com/skaldic/lords2-hd/releases)
and run it with the game closed. It finds the GOG or Steam game by itself (or asks for the folder
holding `Lords2.exe`), adds the patch, and leaves the game's own files alone. Uninstall it from
Windows' Apps list. Windows may warn that the publisher is unknown: the installer is not code-signed.

The patch is a `ddraw.dll` the game loads by itself, the multiplayer layer `dplayx.dll`, an ini with
the settings, and `lords2hd.pak`, one file with the HD art, the restored voices and the videos. If
the game folder already holds a `ddraw.dll` or `dplayx.dll` from another wrapper or multiplayer fix,
these replace them.

**Updating:** run the installer over the old version. Coming from 1.4.0 or older, your settings go
back to the 1.5.0 defaults once (widescreen, the battle fixes and the sharper videos on); the old
ones are kept as `lords2hd-before-1.5.0.ini` beside it. Later updates keep your settings.

Rather unzip by hand? `Lords2-HD-1.6.0.zip` holds the same files: unzip it into the game's folder,
replacing the old files (`lords2hd.ini` too, for the new defaults).

## Videos and music

- **Videos:** the disc-quality videos come with the patch (sharper and full-height; the GOG
  version's are interlaced), and play with upscaled frames drawn over them; the sound and timing
  are the game's own. `HDVideos=0` in the ini shows them at their original detail.
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

Press **F10** in the game: a scroll lists the settings on four tabs, Display, Performance, Quality
of life and Battles (Tab switches them); click a line (or use the arrow keys and Enter) to change
it, right-click to close. Most changes apply at once; those marked * (widescreen, battle zoom-out,
drawing on the graphics card or the processor) after a restart, and the battle fixes from the next
battle. **Ctrl+F10** switches the FPS counter: off, frames a second, or detailed timings.

| Tab | Setting | Choices | Default | In `lords2hd.ini` |
|---|---|---|---|---|
| Display | AI art: paintings | On, Off | On | `HDScreens` |
| Display | AI art: map, units | On, Off | On | `HDTiles` |
| Display | Sharp lettering | On, Off | On | `SharpText` |
| Display | Shore waves | On, Off | On | `ShoreWaves` |
| Display | Widescreen * | On, Off | On | `Wide` |
| Display | County map zoom-out | On, Off | On | `CountyZoom` |
| Display | Seasonal Town Center | On, Off (original) | On | `Seasons` |
| Performance | Upscaling | AMD FSR 1 (sharpest), Catmull-Rom (sharp), Bilinear (soft) | AMD FSR 1 | `Upscale` |
| Performance | Drawing * | Graphics card, Processor | Graphics card | `Renderer` |
| Performance | Frame limit | 120 a second, Off (as the original) | 120 a second | `FrameLimit` |
| Performance | FPS counter | Off, Simple, Detailed | Off | `FpsCounter` |
| Quality of life | Tutorial scrolls | Off, Once, Every time | Once | `TutorialOnce` |
| Quality of life | WASD scrolling | On, Off | On | `WASD` |
| Quality of life | Arrow key scrolling | On, Off | On | `ArrowKeys` |
| Quality of life | , and . cycle | On, Off | On | `CycleKeys` |
| Quality of life | Army panel | On, Off | On | `ArmyPanel` |
| Quality of life | Fresh letters | On, Off | On | `Letters` |
| Quality of life | Lords2Resounded | On, Off | On | `Resounded` |
| Quality of life | Jen With a B's Mods | On (F11: its panel), Off | On | `JenMods` |
| Battles | Enemy tactics | Normal, Hard, Legendary | Normal | `BattleAI` |
| Battles | Improved pathfinding | On, Off | On | `JitterFix` |
| Battles | Group cohesion | On, Off | On | `GroupMoveFix` |
| Battles | Improved engagement | On, Off | On | `EngageFix` |
| Battles | Mop-Up fix | On, Off | On | `MopUpFix` |
| Battles | Battle zoom-out * | On, Off | On | `BattleZoom` |
| Battles | Wheel zoom | Up to 2x (normal size), Up to 4x, Off | Up to 2x | `WheelZoom` |
| Battles | Minimap outlines | On, Off | Off | `MinimapPop` |
| Fixes | Food ration fix | On, Off | On | `RationFix` |
| Fixes | Multiplayer messages | On, Off | On | `MpMessageFix` |
| Castles | Weather wrecks fields | On (original), Off | On | `FieldDestruction` |
| Castles | Pillaging | On (original), Off | On | `Pillaging` |
| Castles | Inner castle door | 1000, 2500, 5000, 10000, 20000, 50000, 100000 | 5000 | `InnerDoor` |
| Castles | Outer castle door | 5000, 10000, 20000, 40000, 80000, 100000 | 20000 | `OuterDoor` |
| Castles | Field reclaim work | 200, 400, 800, 1600, 3200 | 800 | `ReclaimWork` |
| Castles | Reclaim work a season | 100, 200, 400, 800 | 200 | `ReclaimPerSeason` |
| Castles | Castle garrisons | Original (150 to 600), Army size (1500 each) | Original | `GarrisonLimits` |
| Sieges | Wooden palisade | Original, River Island | Original | `PalisadeDesign` |
| Sieges | Motte and bailey | Original, Wooden Palace | Original | `MotteDesign` |
| Sieges | Norman keep | Original, Leeds Castle, Expanded Keep | Original | `KeepDesign` |
| Sieges | Stone castle | Original, Curtain Wall, The Ell, Portchester | Original | `StoneCastleDesign` |
| Sieges | Royal castle | Original, Chambers, Island Castle, Goodrich | Original | `RoyalCastleDesign` |

The four battle fixes, like the other game fixes, stay off in multiplayer unless every player sets
`MultiplayerFixes=1`.

The Fixes, Castles and Sieges tabs are on a panel of their own, Jen With a B's Mods: F11 opens it while Quality of
life → Jen With a B's Mods is On (or click the line under that tab's rows); F10 goes back to the HD settings.
It holds the fixes and mods from Jen With a B's Lords II Mod Manager, rewritten for this patch with
Jen's consent; with the switch Off the game is as without them. Its switches work at once. Weather
wrecks fields: droughts and storms destroy fields. Pillaging: armies in an enemy county destroy its
fields; Off also stops computer lords raising the people of a threatened county that has no castle,
as in Jen's. The castle doors take that many blows before they give way in a siege. A destroyed
field needs the reclaim work in all, and a season puts at most the second number into it. Castle
garrisons: every castle holds as many men as an army may; computer lords still plan their garrisons
by the original sizes. The Castles tab works in single player games only: in a multiplayer game
everyone plays the original rules. In `lords2hd.ini` the door and reclaim numbers can be anything
from 1 to 100000 (doors) or 10000 (reclaim).

Sieges (Jen's Castle Design): a siege on each kind of castle can be fought on one of the game's own
skirmish castles instead of its usual map, from the next siege. Each kind offers the ones Jen picked as
fitting it; only the map changes, the castle's garrison and doors stay its kind's. The maps come from the
game's STNFIEL2.PL8 (it is off if the file is missing). Single player campaign games only, like the
Castles tab.

![The Performance tab and the FPS counter](https://github.com/skaldic/lords2-hd/releases/latest/download/performance.jpg)

Everything is saved in `lords2hd.ini` next to the exe; [hd/INSTALL.txt](hd/INSTALL.txt) lists all
its keys and is also the readme that comes with the patch.

## Requirements

- The GOG or Steam version of Lords of the Realm II (they share the same game files)
- Windows, a screen of 720p (1280x720) or larger, and about 2 GB of disk space
- For drawing on the graphics card: Direct3D 11 (feature level 11.0) graphics from any maker, such as
  NVIDIA GeForce GTX 400 or newer, AMD Radeon HD 5000 or newer, or Intel HD Graphics 4000 or newer.
  AMD FSR 1 is not limited to AMD cards. On older graphics (Intel HD 2000/3000, for example) the patch
  draws on the processor by itself, and FSR still works there, on the processor

## About the art

The HD art is the game's own art enlarged with an AI upscaler (Real-ESRGAN). The videos' frames are
upscaled the same way, blended half and half with the original frame and gently de-dithered, keeping
the original's colours.

The new left and right parts of the widened paintings:
- **Built from the painting's own pixels (new in 1.5.0):** the castle builder's sides are laid with
  bricks cut from its own wall; the armoury's side passages are traced through a model of the room
  and lit by its torch; the custom game, battle setup and results screens carry on their own stone and
  plaster; the greatest nobles' sky is made of its own clouds; the merchant's left side, with the cow
  and barrel, is drawn from the painting's own yard
- **Painted by an AI image model** (FLUX.1 Fill) from the game's own pictures: the remaining sides,
  such as the merchant's right side and the title screen
- The original 4:3 picture is unchanged in the middle

The seasonal Town Center (spring, summer, autumn and winter) and its new idle peasants are community
art by AllYourBases and CaptainRoberts (CaptainRoberts made the Town Center and the idle serfs): the
Town Center pictures were made with an AI image model, the idle serfs drawn by hand. Both were fitted to the game's pictures by the patch's tools; with the switch off the Town Center
is the upscaled original.

The town screen has the original's grain put back over the upscaled picture. The voices are the
game's own recordings, restored with [Lords2Resounded](https://github.com/gqzomer/Lords2Resounded)
with its author's permission. See [NOTICE.md](NOTICE.md).

**Improve the paintings:** `tools/wide_art.ps1 -Export` turns every full-screen painting into PNGs
(widened, double size and the original) and `-Import` turns edited PNGs back into the patch's files.
Better versions are welcome on the Discord.

## Thanks

- **[Lords2Resounded](https://github.com/gqzomer/Lords2Resounded)** by gqzomer: the restored dialogue
  and unit voices were made with its pipeline (LavaSR and DPDFNet), used with its author's
  permission, and its hand-picked loop points make the hi-fi music loop seamlessly. Go give it a star.
- **Wolfe** and **Socius**, for the LotR2 Fix multiplayer layer included as `dplayx.dll` (built from
  `multiplayer/`, see its `CREDITS.md`); Wolfe also for the ideas behind the battle wheel zoom and the
  county map zoom-out (his ZoomUI).
- **Jen With a B** (jenwithab), for the fixes and mods of the Lords II Mod Manager behind Jen With a B's Mods,
  brought in with Jen's consent.
- **AllYourBases** and **CaptainRoberts**, for the seasonal Town Center art; CaptainRoberts also for
  the new idle serfs.
- The Discord community, for the bug reports and ideas behind this release (the minimap outlines
  among them).
- The member of the GOG community who shared the disc-quality videos in the SMK_HIGH thread.
- Keith Zizza, for publishing the original recordings of his soundtrack.
- AMD, for FidelityFX Super Resolution 1 (MIT license, see [NOTICE.md](NOTICE.md)).

## Building it yourself

The repository holds only original code: nothing extracted from the game is in it. The scripts in
`tools/` rebuild the art from your own copy of the game.

- `hd/build.ps1 -Install` builds `ddraw.dll` (32-bit, llvm-mingw) and copies it to the game folder
- `tools/build_screens.ps1`, `build_tiles.ps1`, `build_sprites.ps1`, `build_battle_ground.ps1`,
  `likeness.ps1` and `build_rooms.ps1` make the art in `<game>\lords2hd`; `tools/build_videos.py`
  makes the videos' upscaled frames in `lords2hd\videohd`
- `tools/expand_sides.ps1` and `expand_all.ps1` paint the sides of the widened screens;
  `tools/wide_art.ps1` exports and imports them as PNGs
- `tools/make_pak.ps1` packs the art, voices, videos and video frames into `lords2hd.pak`;
  `tools/make_release.ps1 -Version x.y.z` makes the installer (Inno Setup 6, `tools/lords2hd.iss`)
  and the zip, with `dplayx.dll` built from `multiplayer/` (`multiplayer/dplayx/build.ps1`)
- The scripts find the game folder (the one with `Lords2.exe`) from `-Game`, the `L2HD_GAME`
  environment variable, a `.game-folder` file in the repository's root holding its path, or the
  folder the repository sits in (`tools/game_folder.ps1`)
- External tools (llvm-mingw, Real-ESRGAN, stable-diffusion.cpp and its models) are found through
  the `L2HD_TOOLS` environment variable, or `tools\external`
- `NOTES.md` holds the reverse-engineering notes

Testing uses a hidden, muted mode (`[Debug] Remote=1` in the ini) driven by `hd/bgdrive.ps1`, so
it never takes the mouse or the speakers. `[Debug] BattleLog=1` writes every battle's moves to
`lords2hd-battle.txt`, and `BattleSeed=1` makes battles repeat exactly, for comparing changes.

## Bugs

Report bugs in an issue or on the [community Discord](https://discord.gg/uBzaMGjUR). For a multiplayer
problem, every player sends their `lords2hd-mp` folder of that game (see Multiplayer). After a crash,
keep `lords2hd.log` from the game's folder straight away: it is rewritten at each start. Say which
monitor you use and whether another `ddraw.dll` wrapper or an overlay is running.

## License

Code and tools: [PolyForm Noncommercial 1.0.0](LICENSE.md). The art is not covered by it; see
[NOTICE.md](NOTICE.md). This project is not affiliated with or endorsed by the owners of the game.
