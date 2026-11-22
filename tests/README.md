# Tests

Unit tests for the HD patch (`hd\lords2hd.c`) and its tools: 146 tests in about 8 seconds. They need only the compiler
that builds the patch (llvm-mingw, 32-bit). The five hook tests also need a copy of the game's exe.

```powershell
tests\run.ps1                # build and run everything; exit code 0 = all passed
tests\run.ps1 -Filter pak    # only the tests whose name contains "pak"
tests\run.ps1 -GameExe "D:\L2HD-test\Lords of the Realm II\Lords2.exe"   # with the hook tests (or set L2HD_GAME_EXE)
```

`tools\make_release.ps1` runs these first and builds nothing if a test fails; it needs `-GameExe` so the hook tests
run too, and takes `-Integration` and `-RefDll` for the slow checks.

Before a release, the slow checks too (about 30 minutes, in a COPY of the game, never your own):

```powershell
tests\run.ps1 -GameExe "D:\L2HD-test\Lords of the Realm II\Lords2.exe" -Integration "D:\L2HD-test\Lords of the Realm II" -RefDll <last release's ddraw.dll>
```

- `integration.ps1`: four seeded battles played hidden, each twice. They must match each other (determinism) and the
  reference DLL; a battle you changed on purpose shows as differing from some second on.
- `mp_selftest.ps1`: the multiplayer layer's own self-test (34 cases), built natively.
- `ai_battles.ps1 -Game <copy>`: the computer battle AI's numbers, not a pass/fail check. It plays maps × odds
  (the custom battle screen's advantage arrows: computer stronger, even, weaker), seeded and hidden. Per battle it
  prints how often the computer switched between attack and defence, its charges, the first contact, the seconds its
  archers stood isolated or in melee, each side's shots, and the men left on each side. `-Ini Game.BattleAI=1`
  plays Hard and `Game.BattleAI=2` Legendary; `Debug.AiParts` (a sum of 1 steady mode, 2 archers, 4 melee, 8 unit
  counters, 16 target picks, 32 advancing under fire, 64 regrouping, 128 the archers' target, 256 the weaker
  defender's late attack) runs only those rules.
  - `-Roles cpuatt,cpudef`: the computer attacks (you defend, the screen's default) or defends; `-Players
    stand,charge`: you stand, or Mop-Up at the start; maps include the sieges `palisade`, `acre` and `keep`.
  - Battles end at a wall-clock cut, so under load they reach different battle seconds: `-Logs <saved logs>
    -AtSec 150` summarises saved battle logs at one battle second, playing nothing. Compare two runs of the same
    battle second by second over the seconds both have, not by their last hash.

Set `L2HD_TOOLS` to the tools folder if llvm-mingw is not on PATH (as for `hd\build.ps1`). Output goes to
`tests\out` (not in git). A failing run keeps its temp folder and prints where the patch's log of the run is.

## What is tested

| File | What |
|---|---|
| `t_core.c` | the memory guard, the art pack, the upscalers, HD video frames, WAV files and the music cut, CRC-32 |
| `t_battle.c` | walking distances, Mop-Up, engagement, archers, group routes, the jitter fixes, the battle minimap, the F10 Battles switches |
| `t_ai.c` | the battle log's computer AI lines (each group's decisions, the archers' cover count); enemy tactics on Hard: the steady mode, the archers, soldiers helping attacked archers, the melee advancing smoothly and charging together, soldiers stepping out of burning oil (sieges too); on Legendary: the unit counters, target picks, advancing under fire, regrouping, the archers' target |
| `t_campaign.c` | the wheat, castle cap and besieged castle fixes |
| `t_settings.c` | the code's defaults and the shipped ini; F10 changes saved and read back; the README's F10 table |
| `t_input.c` | the scroll keys (WASD, arrow keys), A in battles, frame copy numbering, the half-drawn guard, moved windows |
| `t_hooks.c` | `install()` on a real Lords2.exe: every hook goes in (4:3 and widescreen); no NULL trampoline called |

## How it works

- `tests.c` includes the patch's own source, so the tests can call its `static` functions directly.
  - The patch's `DllMain` is renamed away there, so its start-up (settings, hooks into the game) never runs.
  - `GetModuleHandleA(NULL)` gives the copied game exe while the hook tests have one mapped.
  - The shipped DLL is built from `hd\lords2hd.c` alone and is not changed by the tests.
- The patch reads the game's data and patches its code at fixed addresses (`FIG(i)` at 0x00554480 and others).
  - `host.c` is linked at 0x00300000, and its zero-filled array covers the game's whole image (0x00400000-0x005e9000).
  - It loads the test DLL, so tests can build a made-up game state there (`game_mem.h`), or copy in a real exe's
    sections (`t_hooks.c`).
  - Game memory from 0x00410000 is cleared before each test.
- The patch's hooks call the game's own functions through `o_*` pointers. The tests point those at small stand-ins.
- `run.ps1` lists every `o_*` trampoline in the source into `tests\out\tramps.h`, so the hook tests can see which
  stay NULL.
- Each test gets a fresh game folder in `%TEMP%\l2hd-tests-<pid>\<test name>` (`g_gameDir` points there).
- `t.h` is the framework:
  - `TEST(name) { ... }` registers a test.
  - `CHECK(cond)` and `CHECK_EQ(a, b)` record a failure and go on; `REQUIRE(cond)` ends the test.
  - A crash inside a test is caught and reported, and the run goes on.

## Adding a test

Put it in the file for its area as `TEST(what_it_checks) { ... }`. Use made-up data only: no game exe bytes, saves or
art in the repository. When a test finds a bug in the patch, the fix and its test go in together.

The plan and its status are in `PLAN.md`.
