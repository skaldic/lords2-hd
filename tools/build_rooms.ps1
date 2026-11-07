# Builds the wider rooms from the 2x paintings, after build_screens.ps1: <game>\lords2hd\wide\<name>.bin
# -Preview also writes 1x previews to hd\shots\wide\<name>.bmp.
param([switch]$Preview)
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$ErrorActionPreference = 'Stop'
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs"), (Join-Path $PSScriptRoot "Room.cs"), (Join-Path $PSScriptRoot "KeyArt.cs") -ReferencedAssemblies System.Drawing
$wide = Join-Path $game "lords2hd\wide"
[Room]::Build((Join-Path $game "lords2hd\screens"), $wide)

# Screens whose sides were painted by tools\paint_sides.ps1: the chosen picture of each, upscaled.
$painted = @{ merchant = 'merchant-gen6'; start = 'start-gen2f'; lords2 = 'lords2-gen1'; gateway = 'gateway-gen1'; score1 = 'score1-gen2f' }
$art = Join-Path $root "assets\wide"; $pin = Join-Path $art "pick"; $pup = Join-Path $art "pick_x4"
Remove-Item $pin, $pup -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $pin, $pup | Out-Null
foreach ($n in $painted.Keys) { $src = Join-Path $art "$($painted[$n]).png"; if (Test-Path $src) { Copy-Item $src (Join-Path $pin "$n.png") } }
$ErrorActionPreference = 'Continue'
& (Join-Path $tools "realesrgan\realesrgan-ncnn-vulkan.exe") -i $pin -o $pup -n realesrgan-x4plus -s 4 -f png -m (Join-Path $tools "realesrgan\models") 2>$null | Out-Null
$ErrorActionPreference = 'Stop'
foreach ($f in Get-ChildItem $pup -Filter *.png) {
    [Room]::Painted((Join-Path $game "lords2hd\screens\$($f.BaseName).bin"), $f.FullName, (Join-Path $wide "$($f.BaseName).bin"))
}
# Panel screens: their own stone repainted by tools\paint_texture.ps1 (already 2x).
foreach ($n in 'skircust', 'skirmish', 'custom', 'cas_back') {
    $src = Join-Path $art "$n-tex.png"
    if (Test-Path $src) { [Room]::Painted((Join-Path $game "lords2hd\screens\$n.bin"), $src, (Join-Path $wide "$n.bin")) }
}

# The skirmish screens' fighting pair (misc_ske frames 33-44) has its own background.
$c = 0; $h = ""
$ske = [Pl8]::Read((Join-Path $game "misc_ske.pl8"), [ref]$c, [ref]$h)
$fighters = New-Object 'byte[][]' 12
for ($k = 0; $k -lt 12; $k++) { $fighters[$k] = $ske[33 + $k].Pixels }
$fw = $ske[33].Width; $fh = $ske[33].Height
$bg = $null; $mark = $null
[Room]::KeyBackground($fighters, $fw, $fh, [ref]$bg, [ref]$mark)
$pal = [Pl8]::ReadPalette((Join-Path $game "SKIRMISH.256"))
$in = Join-Path $root "assets\fighters\in"; $up = Join-Path $root "assets\fighters\x4"
Remove-Item $in, $up -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $in, $up | Out-Null
for ($k = 0; $k -lt 12; $k++) { [KeyArt]::SaveBled($fighters[$k], $bg, $mark, $fw, $fh, $pal, (Join-Path $in ("{0:D2}.png" -f $k))) }
$ErrorActionPreference = 'Continue'
& (Join-Path $tools "realesrgan\realesrgan-ncnn-vulkan.exe") -i $in -o $up -n realesrgan-x4plus -s 4 -f png -m (Join-Path $tools "realesrgan\models") 2>$null | Out-Null
$ErrorActionPreference = 'Stop'
$hd = New-Object 'byte[][]' 12
for ($k = 0; $k -lt 12; $k++) { $hd[$k] = [KeyArt]::Load((Join-Path $up ("{0:D2}.png" -f $k)), $fw, $fh) }
foreach ($n in 'skirmish', 'skircust') { [Room]::AppendKey((Join-Path $wide "$n.bin"), $fighters, $fw, $fh, $hd) }

if ($Preview) {
    $shots = Join-Path $root "hd\shots\wide"
    New-Item -ItemType Directory -Force $shots | Out-Null
    foreach ($f in Get-ChildItem $wide -Filter *.bin) { [Room]::Preview($f.FullName, (Join-Path $shots "$($f.BaseName).bmp")) }
}
Get-ChildItem $wide | Select-Object Name, Length
