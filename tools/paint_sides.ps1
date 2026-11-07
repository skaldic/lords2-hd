# Paints the sides of a 640x480 screen for widescreen with an SDXL inpainting model: the picture stays
# whole in the middle, the model carries it on for -Margin columns each side. Also writes the plain
# mirrored and stretched versions, to compare. Results: assets\wide\<name>-<kind>.png
#   .\tools\paint_sides.ps1 -Name armoury -Prompt "a dark medieval stone armoury ..." -Seed 1,2
param(
    [Parameter(Mandatory)][string]$Name,
    [Parameter(Mandatory)][string]$Prompt,
    [string]$Negative = "text, letters, watermark, signature, frame border, blurry, people, photo",
    [int[]]$Seed = @(1),
    [int]$Margin = 107,
    [int]$Overlap = 12,
    [int]$Steps = 32,
    [double]$Cfg = 6.5
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
$sd = Join-Path $tools "sdcpp\bin\sd-cli.exe"
$model = Join-Path $tools "sdcpp\models\sdxl_inpaint.safetensors"
$vae = Join-Path $tools "sdcpp\models\sdxl_vae_fp16_fix.safetensors"
Add-Type -AssemblyName System.Drawing
$work = Join-Path $root "assets\wide"
New-Item -ItemType Directory -Force $work | Out-Null
$W0 = 640; $H = 480; $W = $W0 + 2 * $Margin
$orig = [Drawing.Bitmap]::FromFile((Join-Path $root "assets\pl8\$Name\000.png"))

function New-Canvas($w, $h) { New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format24bppRgb) }
function Rect($x, $y, $w, $h) { New-Object Drawing.Rectangle $x, $y, $w, $h }

# Mirrored
$m = New-Canvas $W $H; $g = [Drawing.Graphics]::FromImage($m)
$g.DrawImage($orig, $Margin, 0, $W0, $H)
for ($x = 0; $x -lt $Margin; $x++) {
    $g.DrawImage($orig, (Rect ($Margin - 1 - $x) 0 1 $H), (Rect $x 0 1 $H), 'Pixel')
    $g.DrawImage($orig, (Rect ($Margin + $W0 + $x) 0 1 $H), (Rect ($W0 - 1 - $x) 0 1 $H), 'Pixel')
}
$g.Dispose(); $m.Save((Join-Path $work "$Name-mirror.png"));

# Stretched: the outer columns drawn wider, the middle untouched
$s = New-Canvas $W $H; $g = [Drawing.Graphics]::FromImage($s)
$g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
$band = 40
$g.DrawImage($orig, (Rect 0 0 ($Margin + $band) $H), (Rect 0 0 $band $H), 'Pixel')
$g.DrawImage($orig, (Rect ($W - $Margin - $band) 0 ($Margin + $band) $H), (Rect ($W0 - $band) 0 $band $H), 'Pixel')
$g.DrawImage($orig, (Rect ($Margin + $band) 0 ($W0 - 2 * $band) $H), (Rect $band 0 ($W0 - 2 * $band) $H), 'Pixel')
$g.Dispose(); $s.Save((Join-Path $work "$Name-stretch.png")); $s.Dispose()

# Generated: each side as a close-up with part of the picture beside it, so the model carries on
# what the edge cuts. The mirrored canvas, blurred by the model's own noise, is the colour hint.
$CW = 416; $GW = $CW * 2; $GH = $H * 2
foreach ($sdSeed in $Seed) {
    $wide = New-Canvas $W $H; $wg = [Drawing.Graphics]::FromImage($wide); $wg.DrawImage($m, 0, 0, $W, $H)
    foreach ($side in 'left', 'right') {
        $cx = if ($side -eq 'left') { 0 } else { $W - $CW }
        $init = New-Canvas $GW $GH; $ig = [Drawing.Graphics]::FromImage($init)
        $ig.InterpolationMode = 'HighQualityBicubic'; $ig.PixelOffsetMode = 'HighQuality'
        $ig.DrawImage($m, (Rect 0 0 $GW $GH), (Rect $cx 0 $CW $H), 'Pixel'); $ig.Dispose()
        $initPath = Join-Path $work "_$Name-$side-init.png"; $init.Save($initPath); $init.Dispose()
        $mask = New-Canvas $GW $GH; $mg = [Drawing.Graphics]::FromImage($mask); $mg.Clear([Drawing.Color]::Black)
        $mw = ($Margin + $Overlap) * 2; $mx = if ($side -eq 'left') { 0 } else { $GW - $mw }
        $mg.FillRectangle([Drawing.Brushes]::White, $mx, 0, $mw, $GH); $mg.Dispose()
        $maskPath = Join-Path $work "_$Name-$side-mask.png"; $mask.Save($maskPath); $mask.Dispose()
        $out = Join-Path $work "_$Name-$side-$sdSeed.png"
        $ErrorActionPreference = 'Continue'
        & $sd -m $model --vae $vae -p $Prompt -n $Negative -i $initPath --mask $maskPath --strength 1.0 -W $GW -H $GH --steps $Steps --cfg-scale $Cfg --sampling-method dpm++2m -s $sdSeed --vae-tiling -o $out 2>&1 | ForEach-Object { "$_" } | Select-String -Pattern 'error|fail' | ForEach-Object Line
        $ErrorActionPreference = 'Stop'
        if (-not (Test-Path $out)) { throw "no image for seed $sdSeed ($side)" }
        $gen = [Drawing.Bitmap]::FromFile($out)
        $wg.InterpolationMode = 'HighQualityBicubic'; $wg.PixelOffsetMode = 'HighQuality'
        $wg.DrawImage($gen, (Rect $cx 0 $CW $H)); $gen.Dispose()
    }
    # The original over the middle, faded in across the overlap
    $wg.InterpolationMode = 'NearestNeighbor'
    $wg.DrawImage($orig, (Rect ($Margin + $Overlap) 0 ($W0 - 2 * $Overlap) $H), (Rect $Overlap 0 ($W0 - 2 * $Overlap) $H), 'Pixel')
    $wg.Dispose()
    for ($k = 0; $k -lt $Overlap; $k++) {
        $a = ($k + 1) / ($Overlap + 1)
        for ($y = 0; $y -lt $H; $y++) {
            foreach ($pair in @(@(($Margin + $k), $k, $a), @(($W - $Margin - 1 - $k), ($W0 - 1 - $k), $a))) {
                $p = $wide.GetPixel($pair[0], $y); $o = $orig.GetPixel($pair[1], $y); $t = $pair[2]
                $wide.SetPixel($pair[0], $y, [Drawing.Color]::FromArgb([int]($p.R * (1 - $t) + $o.R * $t), [int]($p.G * (1 - $t) + $o.G * $t), [int]($p.B * (1 - $t) + $o.B * $t)))
            }
        }
    }
    $wide.Save((Join-Path $work "$Name-gen$sdSeed.png")); $wide.Dispose()
    "seed $sdSeed done"
}
$m.Dispose(); $orig.Dispose()
