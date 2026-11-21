# Finds llvm-mingw's 32-bit C compiler for hd\build.ps1 and tests\run.ps1. Returns @{ cc = <path>; args = @(...) }:
# i686-w64-mingw32-clang on PATH or in an llvm-mingw-* folder of the external tools folder (tools\tools_dir.ps1),
# else that folder's plain clang, told the target.
$cc = (Get-Command i686-w64-mingw32-clang.exe -ErrorAction SilentlyContinue).Source
$tools = & "$PSScriptRoot\tools_dir.ps1"
if (-not $cc) {
    $cc = Get-ChildItem -Path $tools -Filter i686-w64-mingw32-clang.exe -Recurse -Depth 2 -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
$target = @()
if (-not $cc) { # llvm-mingw's per-target wrapper missing: its clang itself, told the target
    $cc = Get-ChildItem -Path $tools -Filter "clang-*.exe" -Recurse -Depth 2 -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^clang-\d+\.exe$' } |
        Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    $target = @('--target=i686-w64-mingw32', '-fuse-ld=lld')
}
if (-not $cc) { throw "i686-w64-mingw32-clang.exe not found: put llvm-mingw on PATH or in the tools folder (tools\tools_dir.ps1)" }
@{ cc = $cc; args = $target }
