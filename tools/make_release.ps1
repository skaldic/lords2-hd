# Builds the release: dist\Lords2-HD-<version>-setup.exe (Inno Setup) and the same files as a zip,
# dist\Lords2-HD-<version>.zip, to be unzipped into the game's folder. Both hold the DLL (built fresh),
# the default ini, the HD art packed into lords2hd.pak, the readme for players, the license and notice.
#   .\tools\make_release.ps1 -Version 0.9.0 -GameExe <a test copy's Lords2.exe>
#       [-Pak <an existing lords2hd.pak to ship instead of packing the art again>]
#       [-Integration <a copy of the game> -RefDll <the last release's ddraw.dll>]   (the slow checks too, ~30 min)
# The tests run first (tests\run.ps1, the hook tests on GameExe included); nothing is built if one fails.
param([Parameter(Mandatory)][string]$Version, [string]$Pak = "", [string]$GameExe = $env:L2HD_GAME_EXE,
    [string]$Integration, [string]$RefDll)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

if (-not $GameExe -or -not (Test-Path $GameExe)) {
    throw "make_release: give -GameExe (or L2HD_GAME_EXE) a test copy's Lords2.exe: the hook tests must run before a release"
}
$ta = @{ GameExe = $GameExe }
if ($Integration) { $ta.Integration = $Integration; if ($RefDll) { $ta.RefDll = $RefDll } }
& "$root\tests\run.ps1" @ta
if ($LASTEXITCODE -ne 0) { throw "make_release: the tests failed (exit $LASTEXITCODE): nothing built" }
"make_release: the tests passed"
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stage = Join-Path $root "dist\stage"
$zip = Join-Path $root "dist\Lords2-HD-$Version.zip"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\lords2hd" | Out-Null

& "$root\hd\build.ps1"
Copy-Item "$root\hd\ddraw.dll" $stage
# the multiplayer layer (multiplayer\: Wolfe's and Socius's LotR2 Fix dplayx.dll), built fresh
& "$root\multiplayer\dplayx\build.ps1"
Copy-Item "$root\multiplayer\dplayx\dplayx.dll" $stage
Copy-Item "$root\multiplayer\CREDITS.md" "$stage\lords2hd\MULTIPLAYER-CREDITS.md"
$ini = Get-Content "$root\hd\lords2hd.ini" -Raw
if ($ini -notmatch '(?m)^Wide=1') { throw "the ini template must ship Wide=1" }
Set-Content "$stage\lords2hd.ini" $ini -Encoding ascii -NoNewline
Copy-Item "$root\LICENSE.md", "$root\NOTICE.md" "$stage\lords2hd"
Copy-Item "$root\hd\INSTALL.txt" "$stage\lords2hd\README.txt"
# the art: one pack, read by the DLL (tools\make_pak.ps1)
if ($Pak) { Copy-Item $Pak "$stage\lords2hd.pak" } else { & "$PSScriptRoot\make_pak.ps1" -Out "$stage\lords2hd.pak" }
New-Item -ItemType Directory -Force "$stage\lords2hd\music" | Out-Null
Set-Content "$stage\lords2hd\music\put the remastered MP3s here.txt" @'
Put the MP3s of the composer's 2020 remastered soundtrack here, with their release names
(for example "07. Scroll 1 (1996, Remastered).mp3"). At the next start the patch converts them
to WAV and the game plays them instead of its own tracks. Delete the files to go back.
'@ -Encoding ascii
Remove-Item $zip -ErrorAction SilentlyContinue
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
"{0} ({1:N0} MB)" -f $zip, ((Get-Item $zip).Length / 1MB)
# the installer (Inno Setup 6), from the same staged files
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe", (Join-Path (& "$PSScriptRoot\tools_dir.ps1") "InnoSetup\ISCC.exe")) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { "Inno Setup 6 not found: no installer made"; return }
& $iscc /Q "/DAppVer=$Version" "$PSScriptRoot\lords2hd.iss"
if ($LASTEXITCODE -ne 0) { throw "installer build failed" }
$setup = Join-Path $root "dist\Lords2-HD-$Version-setup.exe"
"{0} ({1:N0} MB)" -f $setup, ((Get-Item $setup).Length / 1MB)
