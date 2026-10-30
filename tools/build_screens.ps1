# Builds the 2x versions of the full-screen paintings: each 640x480 picture with a palette of the
# same name goes through Real-ESRGAN x4plus, reduced to 1280x960.
#   <game>\lords2hd\screens\<name>.bin = 768 bytes palette (6-bit, as in the .256) + 1280x960 BGRX
param([string]$Upscaler = "")
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
if (-not $Upscaler) { $Upscaler = Join-Path $tools "realesrgan\realesrgan-ncnn-vulkan.exe" }
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs") -ReferencedAssemblies System.Drawing
Add-Type -AssemblyName System.Drawing
$in = Join-Path $root "assets\screens\in"; $up = Join-Path $root "assets\screens\x4"
$out = Join-Path $game "lords2hd\screens"
New-Item -ItemType Directory -Force $in, $up, $out | Out-Null

$pals = @{}
Get-ChildItem $game -Filter *.256 | ForEach-Object { $pals[$_.BaseName.ToLower()] = $_.FullName }
foreach ($file in Get-ChildItem $game -Filter *.pl8) {
    $name = $file.BaseName.ToLower()
    if (-not $pals.ContainsKey($name)) { continue }
    $c = 0; $h = ""
    $f = [Pl8]::Read($file.FullName, [ref]$c, [ref]$h)
    if ($f.Length -ne 1 -or $f[0].Width -ne 640 -or $f[0].Height -ne 480 -or $f[0].Error) { continue }
    [Pl8]::SavePng($f[0], [Pl8]::ReadPalette($pals[$name]), (Join-Path $in "$name.png"), $false)
    $name
}
& $Upscaler -i $in -o $up -n realesrgan-x4plus -s 4 -f png -m (Join-Path $tools "realesrgan\models") 2>$null | Out-Null

foreach ($png in Get-ChildItem $up -Filter *.png) {
    $src = [Drawing.Image]::FromFile($png.FullName)
    $bmp = New-Object Drawing.Bitmap 1280, 960, ([Drawing.Imaging.PixelFormat]::Format32bppRgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.DrawImage($src, (New-Object Drawing.Rectangle 0, 0, 1280, 960))
    $g.Dispose(); $src.Dispose()
    $d = $bmp.LockBits((New-Object Drawing.Rectangle 0, 0, 1280, 960), 'ReadOnly', $bmp.PixelFormat)
    $px = New-Object byte[] (1280 * 960 * 4)
    [Runtime.InteropServices.Marshal]::Copy($d.Scan0, $px, 0, $px.Length)
    $bmp.UnlockBits($d); $bmp.Dispose()
    $s = [IO.File]::Create((Join-Path $out "$($png.BaseName).bin"))
    $p = [IO.File]::ReadAllBytes($pals[$png.BaseName])
    $s.Write($p, 0, 768); $s.Write($px, 0, $px.Length); $s.Close()
}
"{0} screens in {1}" -f (Get-ChildItem $out).Count, $out
