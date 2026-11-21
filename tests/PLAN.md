# Unit tests for Lords of the Realm II HD (1.6.0): plan

Goal: catch the kind of bugs that reached players in 1.4.0 and 1.5.0 (the 4:3 yes/no crash from a null trampoline, the
minimap crash on freed battle buffers) before a release, with tests that run in seconds on this PC.

## Status

- **Phase 1 is done:** the harness and 24 core tests, which run in about 2 s.
- **First finding, fixed:** `mem_readable`'s check for a range that wraps round the address space was compiled away,
  because it compared pointers and C assumes pointer arithmetic never wraps. It now compares integers. The minimap also
  requires the game's cell stride of 8.
- **Phase 2 is done:** 48 tests on fake game memory (72 in all, about 3 s). They cover:
  - walking distances
  - Mop-Up, engagement and archers
  - group routes and the jitter fixes
  - the minimap regression
  - the F10 Battles switches, including `install_game_fixes` run on fake code to check when a restart is needed
  - the wheat, castle cap and siege fixes
- **The tests catch real bugs.** Three bugs planted in a copy of the patch were each caught:
  - undoing the stride check
  - a wider engage leash
  - dropping the freed-table guard, whose crash was caught and reported
- **Phase 3 is done:** 8 settings tests (80 in all, about 5 s). They check:
  - every setting the code reads (found by scanning the source) is in the shipped ini, apart from four left out on
    purpose
  - the code's defaults match the shipped ini
  - the installer's IniVersion marker
  - every F10 item is on exactly one tab and has a label
  - every F10 change is saved under the key the patch reads at start-up and goes back round to its first value
  - the README's F10 table matches the panel: labels, order, defaults after an install, and the key each one writes
  - the restart marks
- **Second finding, fixed:** the code's default for `[Display] Upscale` was `catmullrom`, while the shipped ini, the
  README and INSTALL.txt all said FSR. It is now FSR. `settings_without_an_ini_are_the_documented_defaults` covers it.
- **Phase 4 is done:** 4 hook tests (85 in all, about 6 s). They run the patch's real `install()` on a local
  Lords2.exe whose sections are copied into the fake memory. The exe is only read, never changed, and none of it goes
  into the repository. The host was moved down to 0x300000 so its fake memory covers the game's whole image.
  - With Wide=0 and with Wide=1, every hook site holds the expected code and every hook goes in (all pitch, frame and
    y*640 sites in widescreen).
  - Every trampoline left NULL in a mode (29 in 4:3, 2 in widescreen) is never called, unguarded, from code the patch
    runs itself. A run.ps1 step lists all 83 trampolines at build time.
  - **These tests catch real bugs:**
    - with the 1.4.0 yes/no guard removed, they flag `h_yesno_draw` calling a NULL `o_yesno_draw` in 4:3, at the
      exact line
    - a hook expecting different code shows up as not hooked
  - Without a game exe given (`-GameExe` or `L2HD_GAME_EXE`) these 4 tests say so and pass.
  - The only test-only change to how the patch builds: in the test build, `GetModuleHandleA(NULL)` gives the copied
    exe while it is mapped.
- **Not tested:** the F10 labels' pixel width. It needs the game's own font.
- **Not tested:** the packer's 4 GB check. Testing it would need a 4 GB file.
- **Not tested:** `h_order` ending Mop-Up. It depends on the caller's return address in the game.

## How it works (proven by a spike on 2026-11-21)

- **The tests compile the patch's own source.** A test DLL does `#define DllMain l2hd_DllMain_off` and then
  `#include "../hd/lords2hd.c"`, so every `static` function can be called directly. The patch's start-up (install) never
  runs, and **the shipped code is unchanged** (no test hooks, no `#ifdef`s).
- **Fake game memory.** The battle and map code reads the game's data at fixed addresses (`FIG(i)` at 0x00554480, the
  battlefield grid at 0x005440e0, `FIG_CUR` at 0x0053e8f8). A tiny host exe at a fixed image base has a 2 MB zero
  array that covers 0x0040b000-0x0060b000, then loads the test DLL. Tests write a made-up battlefield there and call the
  patch's functions on it.
  - A test exe that only moved its own image away does not work: Windows has already put heaps there before `main`.
- **Spike results:**
  - `dm_from` gave 10 steps across an open field and 20 round a wall, as expected.
  - Built with the release compiler (llvm-mingw, 32-bit).
  - ASan is not in this llvm-mingw for 32-bit, so memory checks rely on `mem_readable` tests and guard pages.

## Layout

```
tests/
  host.c        the host exe (fixed base, owns the game's address range, loads the test DLL, prints the summary)
  t.h           a small CHECK / TEST framework (our own, ~60 lines)
  game_mem.h    helpers to build a fake battlefield: clear, place a unit, set terrain, heights, owners
  t_core.c      tier 1 tests     t_battle.c  tier 2     t_settings.c  ini and F10 tests
  t_hooks.c     tier 3 (needs a local Lords2.exe; skipped without one)
  run.ps1       builds host + test DLL with hd\build.ps1's compiler search, runs, exit 1 on any failure
  README.md     how to run and add a test
```

- **Fixtures are synthetic only:** no game exe bytes, saves or art in the repo. Tier 3 reads `Lords2.exe` read-only
  from a test copy (`D:\L2HD-test...`) and skips when it isn't there.
- Each test starts from a reset state: the fake memory is cleared, and the patch's battle statics (`g_dscratch`,
  `g_btick`, `g_cAvoid`, ...) are cleared too.
- Build flags: `-Wno-unused-function`.
- `run.ps1` never redirects compiler stderr. That combination is what broke `make_release` in 1.5.0.

## Tests

### Tier 1: no game memory (fast, pure logic)

- **`mem_readable`:** freed memory, a `PAGE_NOACCESS` page, a guard page, a range that runs off the end of a block,
  and size 0. This is the guard behind the 1.5.0 crash fix.
- **The pak** (`tools\l2pak.c` + `pak_open`/`pak_find`/`afopen`):
  - pack a temp folder and read every file back byte for byte
  - a missing file
  - a loose file wins over the pak
  - name case
  - the 4 GB index check, with a faked large size
- **The scalers:**
  - `cr_axis_make` weights sum to 1
  - a flat image stays flat and 1x is the identity, through `resize_cr`, `fsr_easu` and `fsr_rcas`
  - odd sizes don't write outside the output (canary bytes)
- **Video frames** (`hv_open`/`hv_decode`): a tiny generated frame file opens; a truncated file or bad offsets fail
  cleanly and `g_hv.smk` stays NULL (the 1.5.0 review fix).
- **Sound** (`wav_format`, `wav_data`, `best_offset`): generated WAV headers, good and broken.
- **`crc_block`:** standard CRC-32 test vectors (multiplayer logs compare these).
- **Small helpers:** `cheb`, `mix4`, `mix_rgb`, `key_px`, `menu_tab_count`.

### Tier 2: fake game memory (battles, minimap, game fixes)

- **Distance search** (`dm_from`, `dm_dist`, `dm_pick`):
  - open field, walls, height steps (the climb rule)
  - unreachable targets give `DM_FAR`
  - archers count at half distance
  - avoided targets are skipped until their time runs out
  - the depth cap
  - the per-step cap on full searches
- **Group cohesion:** `gr_line_clear` and `gr_plan` round a forest or river keep the group's shape and don't route
  through blocked cells. `gr_follow` and `gr_step` stay on the planned route.
- **Engagement:**
  - `engage_idle` attacks within its range and never past 4 cells from the unit's post
  - a fleeing enemy is let go
  - knights reach further
  - `archer_step_away` backs off from a charger and never steps into a blocked or taken cell
- **Mop-Up:** `nearest_enemy` and `h_target` pick reachable enemies, and the player's own order still wins.
- **Minimap regression (the 1.5.0 crash)** with `minimap_dots_paint`:
  - freed and unreadable cell and sprite buffers
  - a frame offset ≤0, ≥4096 or unreadable
  - unit numbers 0 and 0x51
  - owner 7
  - each case must return without a fault and without drawing
  - `h_mini_paint` with an origin that would draw off the buffer
- **Battle switches:** `battle_reset` turns each fix on only when its F10 switch is on and its hooks are in place, and
  turns all of them off in a multiplayer game unless `MultiplayerFixes=1`. A switch change takes effect only at the next
  battle.
- **Campaign fixes on made-up counties:**
  - `h_wheat_scale`: one lost field is charged once (the save-2 example: 11 fields, one lost)
  - `h_castle_start`: the count goes up
  - `county_besieged`/`siege_marks`/`siege_unmark`
  - these test arithmetic only; no rule changes

### Settings and F10

- **Defaults match the shipped ini:** `read_settings` on an empty ini gives the same values as on `hd\lords2hd.ini`. A
  mismatch is reported as a finding to discuss, not silently fixed.
- **Ini keys:** every key the patch reads is in the shipped ini (or listed as hidden on purpose, like `[Debug]`). The
  `kBfKey` names and `IniVersion` match the installer's marker.
- **F10 layout:** every tab's labels fit the panel width (`MS_W`), no item appears on two tabs, and every item has a
  label.

### Tier 3: hook sites against a local Lords2.exe (local only)

- The host's address range is moved down (base 0x300000) so it covers 0x00400000-0x00600000. The test copy's exe
  sections are copied into it, and `install()` runs with `Wide=0` and `Wide=1`.
- It then checks that:
  - every hook and patch site matched
  - no trampoline is NULL, which turns the manual 87-hook scan into a test (the 1.4.0 4:3 crash class)
  - every `g_bfOk` flag ends up on
- **Risk:** `install()` also patches the game's import table and calls Windows. Those parts may need stubs, so this tier
  starts with a half-day spike. If it doesn't hold up, the fallback is a static check of each site's expected bytes
  against the exe.

### Integration (slow, on demand, uses what already exists)

- Seeded battle hashes (`BattleSeed` + `pfrun.ps1`) in a hidden test copy, all fixes on and each off. They must match the
  recorded hashes. This also shows that the release DLL is unchanged by the test work.
- The multiplayer `selftest.c`: check whether it builds with llvm-mingw natively; today it builds in Docker only.
- `run.ps1 -Integration` runs these. Plain `run.ps1` stays at seconds.

## Phases (each its own commit on v1.6.0, signed by you, nothing pushed without your OK)

1. Harness (`host.c`, `t.h`, `run.ps1`, README) + tier 1.
2. `game_mem.h` + tier 2, starting with the minimap regression and the distance search.
3. Settings and F10 tests. Any default mismatch is reported to you first.
4. Tier 3 spike, then the hook-site test if it holds.
5. Integration wrapper + final pass: the release DLL's battle hashes are unchanged.

**Out of scope:** changing the shipped DLL. If a test needs a seam (for example pulling the `g_bfOk` logic out of
`install_game_fixes`), it's proposed to you as its own step first. No game-rule changes.

## Your decisions

- **Run the tests in `tools\make_release.ps1`** so a failing test stops a release build (recommended).
- **A GitHub Actions workflow** that runs tiers 1-2 on each push (no game files needed). This adds a workflow file to
  the repo.
- **Branch name:** `v1.6.0` will clash with the release tag again, as `v1.5.0` did. Simplest is to delete the local
  branch after tagging.
