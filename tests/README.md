# Tests

Unit tests for the HD patch (`hd\lords2hd.c`) and its tools. They take a couple of seconds and need only the
compiler that builds the patch (llvm-mingw, 32-bit); no game files.

```powershell
tests\run.ps1                # build and run everything; exit code 0 = all passed
tests\run.ps1 -Filter pak    # only the tests whose name contains "pak"
```

Set `L2HD_TOOLS` to the tools folder if llvm-mingw is not on PATH (as for `hd\build.ps1`). Output goes to
`tests\out` (not in git). A failing run keeps its temp folder and prints where the patch's log of the run is.

## How it works

- `tests.c` includes the patch's own source, so the tests can call its `static` functions directly. The patch's
  `DllMain` is renamed away there, so its start-up (settings, hooks into the game) never runs. The shipped DLL is
  built from `hd\lords2hd.c` alone and is not changed by the tests.
- The patch reads the game's data at fixed addresses (`FIG(i)` at 0x00554480 and others). `host.c` is linked at the
  game's own image base, and its zero-filled array covers those addresses. It loads the test DLL, so tests can build
  a made-up game state there. The fake memory is cleared before each test.
- Each test gets a fresh game folder in `%TEMP%\l2hd-tests-<pid>\<test name>` (`g_gameDir` points there).
- `t.h` is the framework:
  - `TEST(name) { ... }` registers a test.
  - `CHECK(cond)` and `CHECK_EQ(a, b)` record a failure and go on; `REQUIRE(cond)` ends the test.
  - A crash inside a test is caught and reported, and the run goes on.

## Adding a test

Put it in the file for its area (`t_core.c`: no game memory needed), as `TEST(what_it_checks) { ... }`. Use made-up
data only: no game exe bytes, saves or art in the repository.

The plan for the rest (battles on fake game memory, settings, hook sites) is in `PLAN.md`.
