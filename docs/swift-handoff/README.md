# Handoff: what the HD patch learned, for the Swift game

These notes carry over what was learned while building the Lords of the Realm II HD patch
(a proxy `ddraw.dll` for the GOG game) to a separate Swift game. They are written to be read at
the start of a new session that knows nothing about this one.

Read in this order:

1. [FORMATS.md](FORMATS.md) — the game's picture and palette files, and the HD art files the patch
   builds, which a Swift game can read directly.
2. [ART-PIPELINE.md](ART-PIPELINE.md) — how the HD art is made, what worked and what failed.
3. [LESSONS.md](LESSONS.md) — working rules and the mistakes that cost the most time.

Other sources in this repository:

- `NOTES.md` — reverse-engineering notes: exe build, function addresses, how the game draws.
- `tools/Pl8.cs` — a complete, tested reader for `.pl8` pictures and `.256` palettes (about 200
  lines of C#, easy to port to Swift).
- `tools/build_*.ps1`, `tools/Room.cs`, `tools/Bleed.cs` — the art builders.
- `hd/lords2hd.c` — the patch. Its comments say why each hook exists.

## Starting the Swift session

Tell the new session, in your own words:

- where the Swift project lives and what state it is in;
- where the game's files are (`D:\Games\Lords of the Realm II` on the Windows machine; the Swift
  project needs its own copy of the files it reads);
- that these notes exist, and to read them before touching art or file loading;
- whether the Swift game should read the original files, the HD art, or both.

## Rules that apply to the Swift project too

- The game's art, and anything made from it, is never committed to a repository. Scripts rebuild it
  from the owner's own copy of the game.
- The work is non-commercial. Models and tools under non-commercial licences are fine; AI-made art
  is disclosed.
- Commits carry only the owner as author. Anything public (pushing, releases, posts) is the owner's
  call.
- Say the plan first; show previews for anything visual and let the owner choose by eye.

## What these notes cannot tell you

- Nothing here was tested on macOS or in Swift.
- Game rules (economy, combat, AI) were not studied for the patch: it only changes how the game is
  shown. `NOTES.md` lists the functions that were identified; the decompiled source stays local on
  the Windows machine (`src\`, not in the repository).
