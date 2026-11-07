# Builds the wider rooms from the 2x paintings, after build_screens.ps1: <game>\lords2hd\wide\<name>.bin
# -Preview also writes 1x previews to hd\shots\wide\<name>.bmp.
param([switch]$Preview)
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs") -ReferencedAssemblies System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot "Room.cs")
$wide = Join-Path $game "lords2hd\wide"
[Room]::Build((Join-Path $game "lords2hd\screens"), $wide)

# The skirmish screens' fighting pair (misc_ske frames 33-44) has its own background.
$c = 0; $h = ""
$ske = [Pl8]::Read((Join-Path $game "misc_ske.pl8"), [ref]$c, [ref]$h)
$fighters = New-Object 'byte[][]' 12
for ($k = 0; $k -lt 12; $k++) { $fighters[$k] = $ske[33 + $k].Pixels }
foreach ($n in 'skirmish', 'skircust') { [Room]::AppendKey((Join-Path $wide "$n.bin"), $fighters, $ske[33].Width, $ske[33].Height) }

if ($Preview) {
    $shots = Join-Path $root "hd\shots\wide"
    New-Item -ItemType Directory -Force $shots | Out-Null
    foreach ($f in Get-ChildItem $wide -Filter *.bin) { [Room]::Preview($f.FullName, (Join-Path $shots "$($f.BaseName).bmp")) }
}
Get-ChildItem $wide | Select-Object Name, Length
