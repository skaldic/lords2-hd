# Builds the release archive: dist\Lords2-HD-<version>.zip, to be unzipped into the game's folder.
# It holds the DLL (built fresh), the default ini and the HD art in <game>\lords2hd\, plus the readme
# for players, the license and the notice.
#   .\tools\make_release.ps1 -Version 0.9.0
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stage = Join-Path $root "dist\stage"
$zip = Join-Path $root "dist\Lords2-HD-$Version.zip"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\lords2hd" | Out-Null

& "$root\hd\build.ps1"
Copy-Item "$root\hd\ddraw.dll" $stage
$ini = Get-Content "$root\hd\lords2hd.ini" -Raw
if ($ini -notmatch '(?m)^Wide=0') { throw "the ini template has no Wide=0" }
Set-Content "$stage\lords2hd.ini" ($ini -replace '(?m)^Wide=0', 'Wide=1') -Encoding ascii -NoNewline
Copy-Item "$root\LICENSE.md", "$root\NOTICE.md" "$stage\lords2hd"
Copy-Item "$root\hd\INSTALL.txt" "$stage\lords2hd\README.txt"
foreach ($dir in 'screens', 'wide', 'tiles', 'sprites') {
    $src = Join-Path $game "lords2hd\$dir"
    if (-not (Get-ChildItem $src -Filter *.bin)) { throw "no art in $src" }
    New-Item -ItemType Directory -Force "$stage\lords2hd\$dir" | Out-Null
    Copy-Item "$src\*.bin", "$src\*.pal" "$stage\lords2hd\$dir" -ErrorAction SilentlyContinue
}
New-Item -ItemType Directory -Force "$stage\lords2hd\music" | Out-Null
Set-Content "$stage\lords2hd\music\put the remastered MP3s here.txt" @'
Put the MP3s of the composer's 2020 remastered soundtrack here, with their release names
(for example "07. Scroll 1 (1996, Remastered).mp3"). At the next start the patch converts them
to WAV and the game plays them instead of its own tracks. Delete the files to go back.
'@ -Encoding ascii
Remove-Item $zip -ErrorAction SilentlyContinue
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
"{0} ({1:N0} MB)" -f $zip, ((Get-Item $zip).Length / 1MB)
