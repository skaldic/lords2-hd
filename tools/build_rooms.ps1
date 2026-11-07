# Builds the wider rooms from the 2x paintings, after build_screens.ps1: <game>\lords2hd\wide\<name>.bin
# -Preview also writes 1x previews to hd\shots\wide\<name>.bmp.
param([switch]$Preview)
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
Add-Type -Path (Join-Path $PSScriptRoot "Room.cs")
$wide = Join-Path $game "lords2hd\wide"
[Room]::Build((Join-Path $game "lords2hd\screens"), $wide)
if ($Preview) {
    $shots = Join-Path $root "hd\shots\wide"
    New-Item -ItemType Directory -Force $shots | Out-Null
    foreach ($f in Get-ChildItem $wide -Filter *.bin) { [Room]::Preview($f.FullName, (Join-Path $shots "$($f.BaseName).bmp")) }
}
Get-ChildItem $wide | Select-Object Name, Length
