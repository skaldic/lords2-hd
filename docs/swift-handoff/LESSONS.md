# Lessons

Most of these are mistakes that were made on the HD patch.

## Working with the owner

- Say the plan before doing it. For anything visual, make a sheet of options and let the owner
  pick; they judge by eye and reject what looks invented.
- Do not call art finished from the file alone. Check it in the running game, against the original,
  at full size.
- Report what is not verified as plainly as what is. "Checked in files only" is a different claim
  from "seen in the game".
- Never test on the owner's saves, never take over their mouse or sound, and check whether the game
  is running before a test script closes it (a test once killed the owner's session).
- Push only when asked. Commits carry only the owner as author.

## Art

- Each picture file was drawn for one palette; the wrong one looks almost right. See FORMATS.md.
- Mirroring a painting to widen it is noticed at once.
- Image models invent. Use them for scenes, give them the whole picture, and keep them away from
  regular geometry. See ART-PIPELINE.md.
- The upscaler changes faces. Put the original's tones back.
- Subtle changes are invisible to the owner; preview several strengths.
- Art built in one overnight batch must still be reviewed picture by picture.

## Game behaviour worth knowing

- The original runs at 640x480 with 256 colours and spins one CPU core at 100% in its main loop.
- It redraws the county map hundreds of times a second while idle; scrolling and animation run at
  the game's own step rate, not the display's.
- Starting a new game overwrites `lastturn.sav` and `lords2.inf`.
- Screen layout facts (640-wide game coordinates): the side panel starts at x = 478; the top bar is
  24 rows; battle cells are 32 pixels; the county minimap is 128x128.
- In a battle the "Battle Paused" label and its arrow are drawn beside the hourglass button at the
  bottom of the panel.

## Tooling (Windows side)

- PowerShell 5.1: no `&&`; `Set-Content` defaults to ANSI; `Add-Type` cannot load a changed C# file
  twice in one session, so run art scripts in fresh processes; long jobs go to the background.
- Hard disks: loading a 10 GB model takes minutes; one sequential read beats many small files.
- A 32-bit game that is not large-address-aware has 2 GB: cap every cache.
- Measure before optimising: a timing line in the log (pictures per second, cost per picture, cost
  of the game's own redraws) answered "is it laggy?" in ten minutes.
