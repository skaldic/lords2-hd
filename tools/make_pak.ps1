# Packs the HD art in <game>\lords2hd (screens, wide, tiles, sprites; sound, videos and the HD video
# frames in videohd if present) into one file, lords2hd.pak,
# which the DLL reads (loose files in lords2hd\ still win, for hand-made changes).
#   .\tools\make_pak.ps1 [-Out <path>]     default: dist\stage\lords2hd.pak
param([string]$Out = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. "$PSScriptRoot\game_folder.ps1"; $game = Get-GameFolder
if (-not $Out) { $Out = Join-Path $root "dist\stage\lords2hd.pak" }
New-Item -ItemType Directory -Force (Split-Path $Out -Parent) | Out-Null
$tools = & "$PSScriptRoot\tools_dir.ps1"
$cc = Get-ChildItem -Path $tools -Filter x86_64-w64-mingw32-clang.exe -Recurse -Depth 2 -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
$target = @()
if (-not $cc) { # the per-target wrapper missing: llvm-mingw's clang itself, told the target
    $cc = Get-ChildItem -Path $tools -Filter "clang-*.exe" -Recurse -Depth 2 -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^clang-\d+\.exe$' } |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    $target = @('--target=x86_64-w64-mingw32', '-fuse-ld=lld')
}
if (-not $cc) { throw "x86_64-w64-mingw32-clang.exe not found (llvm-mingw, tools\tools_dir.ps1)" }
$exe = Join-Path $env:TEMP "l2pak.exe"
& $cc @target -O2 -Wall -o $exe "$PSScriptRoot\l2pak.c"
if ($LASTEXITCODE -ne 0) { throw "l2pak build failed" }
& $exe (Join-Path $game "lords2hd") $Out
if ($LASTEXITCODE -ne 0) { throw "packing failed" }
