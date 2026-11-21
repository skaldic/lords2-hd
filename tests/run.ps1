# Builds and runs the patch's tests (see tests\README.md). Exit code 0 when every test passes.
#   tests\run.ps1                 all tests
#   tests\run.ps1 -Filter pak     only tests whose name contains "pak"
# Needs the same compiler as hd\build.ps1 (llvm-mingw, 32-bit). Output goes to tests\out (not in git).
param([string]$Filter = "")
$ErrorActionPreference = 'Stop'
$c = & "$PSScriptRoot\..\tools\find_cc.ps1"
$cc = $c.cc; $target = $c.args
$out = Join-Path $PSScriptRoot "out"
New-Item -ItemType Directory -Force $out | Out-Null

function Build-Part($what, [string[]]$ccArgs) {
    & $cc @target @ccArgs
    if ($LASTEXITCODE -ne 0) { throw "tests: building $what failed" }
}
# The host owns the game's address range: linked at the game's image base, never moved.
Build-Part "the host" @('-O2', '-Wall', '-o', "$out\l2hd_tests.exe", "$PSScriptRoot\host.c",
    '-Wl,--image-base,0x400000', '-Wl,--disable-dynamicbase')
# The packer, as tools\make_pak.ps1 builds it (the pack tests run it).
Build-Part "l2pak" @('-O2', '-Wall', '-o', "$out\l2pak.exe", "$PSScriptRoot\..\tools\l2pak.c")
# The patch's source with the tests, at the release build's optimisation.
Build-Part "the tests" @('-O2', '-Wall', '-Wno-unused-function', '-shared', '-o', "$out\l2hd_tests.dll", "$PSScriptRoot\tests.c",
    '-luser32', '-lgdi32', '-lwinmm', '-lole32', '-lmfuuid', '-lwindowscodecs', '-luuid', '-lshell32')

$t0 = Get-Date
& "$out\l2hd_tests.exe" $Filter
$code = $LASTEXITCODE
"tests: {0:N1} s, exit {1}" -f ((Get-Date) - $t0).TotalSeconds, $code
exit $code
