# Builds the HD DLL. Pass -Install to also copy it next to the game as ddraw.dll.
# Needs llvm-mingw's 32-bit clang (i686-w64-mingw32-clang): on PATH, or in an llvm-mingw-*
# folder of the external tools folder (tools\tools_dir.ps1).
param([switch]$Install)

$c = & "$PSScriptRoot\..\tools\find_cc.ps1"   # (shared with tests\run.ps1)
$cc = $c.cc; $target = $c.args
$out = "$PSScriptRoot\ddraw.dll"

& $cc @target -O2 -Wall -shared -static -o $out "$PSScriptRoot\lords2hd.c" "$PSScriptRoot\lords2hd.def" -luser32 -lgdi32 -lwinmm -lole32 -lmfuuid -lwindowscodecs -luuid
if ($LASTEXITCODE -ne 0) { throw "build failed" }
"built $out ($((Get-Item $out).Length) bytes)"

if ($Install) {
    Copy-Item $out "$PSScriptRoot\..\..\ddraw.dll" -Force -ErrorAction Stop
    "installed to game folder"
}
