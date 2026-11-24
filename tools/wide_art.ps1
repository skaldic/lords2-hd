# The full-screen paintings as PNGs for the community to improve, and back.
#   Export: .\tools\wide_art.ps1 -Export -Art <a lords2hd folder with screens\ and wide\> -Out <folder>
#     wide\<name>.png        the widened painting the patch shows, 1708x960 (the 4:3 painting at x 214-1493)
#     hd-4x3\<name>.png      the 4:3 painting at double size, 1280x960 (Real-ESRGAN)
#     original\<name>.png    the game's own picture, 640x480 (from <game>\<name>.pl8 and .256)
#     data\<name>.meta       the room file's header and animation data (keep it next to the PNG)
#   Import: .\tools\wide_art.ps1 -Import -In <that folder> -Art <a lords2hd folder to write to>
#     writes wide\<name>.bin from wide\<name>.png and screens\<name>.bin from hd-4x3\<name>.png
[CmdletBinding(PositionalBinding = $false)]
param([switch]$Export, [switch]$Import, [string]$Art = "", [string]$Out = "", [string]$In = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
if (-not $Art) { $Art = Join-Path $game "lords2hd" }
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs") -ReferencedAssemblies System.Drawing
$fmt = [Drawing.Imaging.PixelFormat]::Format32bppRgb
function Rect($w, $h) { New-Object Drawing.Rectangle 0, 0, $w, $h }

function Save-Png([byte[]]$bytes, [int]$off, [int]$w, [int]$h, [string]$path) {
    $b = New-Object Drawing.Bitmap $w, $h, $fmt
    $d = $b.LockBits((Rect $w $h), 'WriteOnly', $fmt)
    [Runtime.InteropServices.Marshal]::Copy($bytes, $off, $d.Scan0, $w * $h * 4); $b.UnlockBits($d)
    $b.Save($path, [Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
}
function Read-Png([string]$path, [int]$w, [int]$h) {
    $src = [Drawing.Image]::FromFile($path)
    if ($src.Width -ne $w -or $src.Height -ne $h) { $src.Dispose(); throw "$path is $($src.Width)x$($src.Height), must be ${w}x$h" }
    $b = New-Object Drawing.Bitmap $w, $h, $fmt
    $g = [Drawing.Graphics]::FromImage($b); $g.DrawImageUnscaled($src, 0, 0); $g.Dispose(); $src.Dispose()
    $d = $b.LockBits((Rect $w $h), 'ReadOnly', $fmt)
    $px = New-Object byte[] ($w * $h * 4)
    [Runtime.InteropServices.Marshal]::Copy($d.Scan0, $px, 0, $px.Length); $b.UnlockBits($d); $b.Dispose()
    for ($i = 3; $i -lt $px.Length; $i += 4) { $px[$i] = 0 }   # the patch's files keep the 4th byte 0
    $px
}

if ($Export) {
    if (-not $Out) { throw "-Out is needed" }
    foreach ($s in 'wide', 'hd-4x3', 'original', 'data') { New-Item -ItemType Directory -Force (Join-Path $Out $s) | Out-Null }
    foreach ($f in Get-ChildItem (Join-Path $Art "wide") -Filter *.bin) {
        $n = $f.BaseName; $b = [IO.File]::ReadAllBytes($f.FullName)
        $w = [BitConverter]::ToInt32($b, 0) * 2; $len = 20 + $w * 960 * 4
        Save-Png $b 20 $w 960 (Join-Path $Out "wide\$n.png")
        $meta = New-Object byte[] (20 + $b.Length - $len)
        [Array]::Copy($b, 0, $meta, 0, 20); [Array]::Copy($b, $len, $meta, 20, $b.Length - $len)
        [IO.File]::WriteAllBytes((Join-Path $Out "data\$n.meta"), $meta)
        "wide $n ($w x 960)"
    }
    foreach ($f in Get-ChildItem (Join-Path $Art "screens") -Filter *.bin) {
        $n = $f.BaseName
        Save-Png ([IO.File]::ReadAllBytes($f.FullName)) 768 1280 960 (Join-Path $Out "hd-4x3\$n.png")
        $pl8 = Join-Path $game "$n.pl8"; $pal = Join-Path $game "$n.256"
        if ((Test-Path $pl8) -and (Test-Path $pal)) {
            $c = 0; $h = ""
            $fr = [Pl8]::Read($pl8, [ref]$c, [ref]$h)
            [Pl8]::SavePng($fr[0], [Pl8]::ReadPalette($pal), (Join-Path $Out "original\$n.png"), $false)
        }
        "4:3 $n"
    }
}

if ($Import) {
    if (-not $In) { throw "-In is needed" }
    foreach ($p in Get-ChildItem (Join-Path $In "wide") -Filter *.png -ErrorAction SilentlyContinue) {
        $n = $p.BaseName; $meta = [IO.File]::ReadAllBytes((Join-Path $In "data\$n.meta"))
        $w = [BitConverter]::ToInt32($meta, 0) * 2
        $px = Read-Png $p.FullName $w 960
        New-Item -ItemType Directory -Force (Join-Path $Art "wide") | Out-Null
        $s = [IO.File]::Create((Join-Path $Art "wide\$n.bin"))
        $s.Write($meta, 0, 20); $s.Write($px, 0, $px.Length); $s.Write($meta, 20, $meta.Length - 20); $s.Close()
        "wide $n"
    }
    foreach ($p in Get-ChildItem (Join-Path $In "hd-4x3") -Filter *.png -ErrorAction SilentlyContinue) {
        $n = $p.BaseName; $pal = Join-Path $game "$n.256"
        if (-not (Test-Path $pal)) { "skipped $n (no $n.256 in the game folder)"; continue }
        $px = Read-Png $p.FullName 1280 960
        New-Item -ItemType Directory -Force (Join-Path $Art "screens") | Out-Null
        $s = [IO.File]::Create((Join-Path $Art "screens\$n.bin"))
        $s.Write([IO.File]::ReadAllBytes($pal), 0, 768); $s.Write($px, 0, $px.Length); $s.Close()
        "4:3 $n"
    }
}
