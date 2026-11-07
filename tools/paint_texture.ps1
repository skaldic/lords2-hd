# Sides for screens laid out as panels (skirmish, castle builder): outpainting copied their buttons
# and parchments, so instead a clean patch of the screen's own texture is tiled and repainted by the
# image model at moderate strength, one strip per side, into assets\wide\<name>-tex.png (1708x960,
# already 2x: build_rooms.ps1 uses it as it is).
#   .\tools\paint_texture.ps1 -Name skircust -Patch 226,180,212,190 -Prompt "teal green stone wall"
param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][int[]]$Patch,      # x, y, w, h in the 640x480 picture
    [Parameter(Mandatory)][string]$Prompt,
    [string]$Negative = "text, letters, buttons, panels, frames, parchment, paper, icons, watermark, blurry, photo",
    [double]$Strength = 0.55,
    [int[]]$Seed = @(1, 2),
    [int]$Steps = 30
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
$sd = Join-Path $tools "sdcpp\bin\sd-cli.exe"
$model = Join-Path $tools "sdcpp\models\sdxl_inpaint.safetensors"
$vae = Join-Path $tools "sdcpp\models\sdxl_vae_fp16_fix.safetensors"
Add-Type -Path (Join-Path $PSScriptRoot "Room.cs") -ReferencedAssemblies System.Drawing
$work = Join-Path $root "assets\wide"
New-Item -ItemType Directory -Force $work | Out-Null
$bin = Join-Path $game "lords2hd\screens\$Name.bin"
$strips = @()
# The model is an inpainting one: without a mask it paints noise. All white = repaint everything.
Add-Type -AssemblyName System.Drawing
$mask = Join-Path $work "_tex-mask.png"
$m = New-Object Drawing.Bitmap 1024, 960; $g = [Drawing.Graphics]::FromImage($m); $g.Clear([Drawing.Color]::White); $g.Dispose(); $m.Save($mask); $m.Dispose()
foreach ($i in 0, 1) {
    $init = Join-Path $work "_$Name-tex$i-init.png"
    [Room]::TilePatch($bin, $Patch[0], $Patch[1], $Patch[2], $Patch[3], 1024, 960, 37 * $i, $init)
    $out = Join-Path $work "_$Name-tex$i.png"
    $ErrorActionPreference = 'Continue'
    & $sd -m $model --vae $vae -p $Prompt -n $Negative -i $init --mask $mask --strength $Strength -W 1024 -H 960 --steps $Steps --cfg-scale 6 --sampling-method dpm++2m -s $Seed[$i] --vae-tiling -o $out 2>&1 | ForEach-Object { "$_" } | Select-String -Pattern 'error|fail' | ForEach-Object Line
    $ErrorActionPreference = 'Stop'
    if (-not (Test-Path $out)) { throw "no strip $i" }
    # SDXL works at about 1024 pixels: a strip is cut from the middle of a full-size picture.
    $full = New-Object Drawing.Bitmap $out; $strip = $full.Clone((New-Object Drawing.Rectangle 384, 0, 256, 960), $full.PixelFormat); $full.Dispose()
    $cut = Join-Path $work "_$Name-tex$i-strip.png"; $strip.Save($cut); $strip.Dispose()
    $strips += $cut
}
[Room]::Strips($strips[0], $strips[1], (Join-Path $work "$Name-tex.png"))
"$Name-tex.png"
