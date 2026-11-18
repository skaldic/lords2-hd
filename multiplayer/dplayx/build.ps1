# Builds dplayx.dll (32-bit) with llvm-mingw on Windows, the same toolchain as hd\build.ps1.
# (build.sh is the authors' original Docker/gcc build.) Output: dplayx.dll beside this file.
$tools = & "$PSScriptRoot\..\..\tools\tools_dir.ps1"
$cc = Get-ChildItem -Path $tools -Filter "clang-*.exe" -Recurse -Depth 2 -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^clang-\d+\.exe$' } | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $cc) { throw "llvm-mingw clang not found: put llvm-mingw in the tools folder (tools\tools_dir.ps1)" }
$out = "$PSScriptRoot\dplayx.dll"
Push-Location $PSScriptRoot
try {
    & $cc --target=i686-w64-mingw32 -fuse-ld=lld -O2 -Wall -fstack-protector-strong -shared -o $out dplayx.c dplayx.def `
        -lws2_32 -static-libgcc -lssp "-Wl,--kill-at,--dynamicbase,--nxcompat"
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
} finally { Pop-Location }
"built $out ($((Get-Item $out).Length) bytes), sha256 $((Get-FileHash $out).Hash.ToLower())"
