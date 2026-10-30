# Builds a comparison sheet of upscaling options from real game art, for choosing by eye:
#   assets\preview\sheet_<sample>.png  columns: game's current smoothing / x4plus / anime / x4plus + grain
param([string]$Palette = "assets\palettes\pal_005.256")
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tools = if ($env:L2HD_TOOLS) { $env:L2HD_TOOLS } else { "D:\Tools" }
$esr = Join-Path $tools "realesrgan\realesrgan-ncnn-vulkan.exe"
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs") -ReferencedAssemblies System.Drawing
Add-Type -AssemblyName System.Drawing
$out = Join-Path $root "assets\preview"; $in = Join-Path $out "in"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $in | Out-Null

function Frames($name) { $c = 0; $h = ""; [Pl8]::Read((Join-Path $game "$name.pl8"), [ref]$c, [ref]$h) }

# Draws frames onto a canvas: @(frame, x, y) lists, opaque background colour.
function Compose($file, $w, $h, $pal, $items, $bg) {
    $bmp = New-Object Drawing.Bitmap $w, $h
    $g = [Drawing.Graphics]::FromImage($bmp); $g.Clear($bg)
    $tmp = Join-Path $in "_t.png"
    foreach ($it in $items) {
        [Pl8]::SavePng($it[0], $pal, $tmp, $false)
        $im = [Drawing.Image]::FromFile($tmp)
        $g.DrawImage($im, (New-Object Drawing.Rectangle $it[1], $it[2], $im.Width, $im.Height))
        $im.Dispose()
    }
    $g.Dispose(); $bmp.Save((Join-Path $in "$file.png")); $bmp.Dispose(); Remove-Item $tmp
}

$county = [Pl8]::ReadPalette((Join-Path $game "Base01.256"))
$battle = [Pl8]::ReadPalette((Join-Path $root $Palette))
$castle = [Pl8]::ReadPalette((Join-Path $game "Cas_back.256"))
$grassBg = [Drawing.Color]::FromArgb(70, 95, 40)

# Terrain: an isometric patch of grass frames from the first set, 60 x 15 pitch as the map draws it.
$b = Frames "base1a"
$items = @(); $n = 6
for ($r = 0; $r -lt 18; $r++) { for ($c = 0; $c -lt 4; $c++) {
    $items += , @($b[6 + ($n++ % 30)], ($c * 60 + $(if ($r % 2) { 30 } else { 0 }) - 20), ($r * 15 - 15))
} }
Compose "terrain" 200 150 $county $items ([Drawing.Color]::Black)

# Buildings: the largest frames of the county sprite sheet.
$s = Frames "sprite1a" | Where-Object { -not $_.Error } | Sort-Object { $_.Width * $_.Height } -Descending | Select-Object -First 3
Compose "buildings" 200 150 $county @(, @($s[0], 2, 2)) $grassBg

# Troops: the first frames of a unit sheet on battle grass, with the logged battle palette.
$t = Frames "a2r_arch"
Compose "troops" 200 150 $battle @(@($t[0], 10, 10), @($t[1], 70, 10), @($t[2], 130, 10), @($t[8], 10, 80), @($t[9], 70, 80), @($t[10], 130, 80)) $grassBg

# Painted screen: a crop of the castle screen.
$p = (Frames "cas_back")[0]
Compose "painting" 640 480 $castle @(, @($p, 0, 0)) ([Drawing.Color]::Black)
$src = [Drawing.Image]::FromFile((Join-Path $in "painting.png"))
$crop = New-Object Drawing.Bitmap 200, 150
$g = [Drawing.Graphics]::FromImage($crop)
$g.DrawImage($src, (New-Object Drawing.Rectangle 0, 0, 200, 150), (New-Object Drawing.Rectangle 200, 120, 200, 150), [Drawing.GraphicsUnit]::Pixel)
$g.Dispose(); $src.Dispose(); $crop.Save((Join-Path $in "painting2.png")); $crop.Dispose()
Remove-Item (Join-Path $in "painting.png"); Rename-Item (Join-Path $in "painting2.png") "painting.png"

foreach ($m in 'realesrgan-x4plus', 'realesrgan-x4plus-anime') {
    $o = Join-Path $out $m; New-Item -ItemType Directory -Force $o | Out-Null
    & $esr -i $in -o $o -n $m -s 4 -f png -m (Join-Path $tools "realesrgan\models") | Out-Null
}

$font = New-Object Drawing.Font "Arial", 14, ([Drawing.FontStyle]::Bold)
foreach ($f in Get-ChildItem $in -Filter *.png) {
    $w = 400; $h = 300
    $sheet = New-Object Drawing.Bitmap (($w + 8) * 4), ($h + 28)
    $g = [Drawing.Graphics]::FromImage($sheet); $g.Clear([Drawing.Color]::FromArgb(30, 30, 34))
    $orig = [Drawing.Image]::FromFile($f.FullName)
    $x4 = [Drawing.Image]::FromFile((Join-Path $out "realesrgan-x4plus\$($f.Name)"))
    $an = [Drawing.Image]::FromFile((Join-Path $out "realesrgan-x4plus-anime\$($f.Name)"))
    $cols = @(@('Now (smooth 2x)', $orig, 'Bilinear', 1.0), @('Upscaled: x4plus', $x4, 'HighQualityBicubic', 1.0), @('Upscaled: anime', $an, 'HighQualityBicubic', 1.0), @('x4plus + 50% grain', $x4, 'HighQualityBicubic', 0.5))
    for ($i = 0; $i -lt 4; $i++) {
        $X = $i * ($w + 8) + 4
        $g.DrawString($cols[$i][0], $font, [Drawing.Brushes]::White, $X, 3)
        $g.InterpolationMode = $cols[$i][2]
        $rect = New-Object Drawing.Rectangle $X, 26, $w, $h
        $g.DrawImage($cols[$i][1], $rect)
        if ($cols[$i][3] -lt 1) {
            $g.InterpolationMode = 'NearestNeighbor'
            $cm = New-Object Drawing.Imaging.ColorMatrix; $cm.Matrix33 = 0.5
            $ia = New-Object Drawing.Imaging.ImageAttributes; $ia.SetColorMatrix($cm)
            $g.DrawImage($orig, $rect, 0, 0, $orig.Width, $orig.Height, [Drawing.GraphicsUnit]::Pixel, $ia)
        }
    }
    $orig.Dispose(); $x4.Dispose(); $an.Dispose(); $g.Dispose()
    $sheet.Save((Join-Path $out "sheet_$($f.BaseName).png")); $sheet.Dispose()
    "sheet_$($f.BaseName).png"
}
