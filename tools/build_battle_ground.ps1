# Builds 2x battle ground: the t32_* sheets, in the sprite format, with part of the original kept
# so grass does not turn to plastic. Transparent pixels stay transparent.
param([string[]]$Sets = @('t32_bat1','t32_stn1','t32_stn2','t32_wod1','t32_wod2'), [double]$Plain = 0.25, [string]$Palette = "")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
$esr = Join-Path $tools "realesrgan\realesrgan-ncnn-vulkan.exe"
Add-Type -AssemblyName System.Drawing
$work = Join-Path $root "assets\battleground"; $in = Join-Path $work "in"; $up = Join-Path $work "x4"
$out = Join-Path $game "lords2hd\sprites"
Remove-Item $in, $up -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $in, $up, $out | Out-Null
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs"), (Join-Path $PSScriptRoot "Bleed.cs") -ReferencedAssemblies System.Drawing
$all = @{}; $pals = @{}
foreach ($n in $Sets) {
    $c = 0; $h = ""
    $f = [Pl8]::Read((Join-Path $game "$n.pl8"), [ref]$c, [ref]$h)
    $all[$n] = $f
    $palName = if ($Palette) { $Palette } elseif ($n -match 'bat') { 'T32_bat1.256' } else { 'T32_stn1.256' }
    $pals[$n] = [Pl8]::ReadPalette((Join-Path $game $palName))
    for ($i = 0; $i -lt $f.Length; $i++) {
        if (-not $f[$i].Error -and $f[$i].Width -ge 6 -and $f[$i].Height -ge 6) { [Bleed]::Save($f[$i], $pals[$n], (Join-Path $in ("{0}_{1:D3}.png" -f $n, $i))) }
    }
}
"{0} frames to upscale" -f (Get-ChildItem $in).Count
$ErrorActionPreference = 'Continue'
& $esr -i $in -o $up -n realesrgan-x4plus -s 4 -f png -m (Join-Path $tools "realesrgan\models") 2>$null | Out-Null
$ErrorActionPreference = 'Stop'
if ((Get-ChildItem $up).Count -ne (Get-ChildItem $in).Count) { throw "the upscaler did not finish" }
foreach ($n in $Sets) {
    $f = $all[$n]; $pal = $pals[$n]; $wc = [PixelWater]::WaterColours($f, $pal)
    $s = [IO.File]::Create((Join-Path $out "$n.bin"))
    $s.Write([BitConverter]::GetBytes([int]$f.Length), 0, 4)
    $s.Write((New-Object byte[] (4 * $f.Length)), 0, 4 * $f.Length)
    $table = New-Object int[] $f.Length
    for ($i = 0; $i -lt $f.Length; $i++) {
        $u = Join-Path $up ("{0}_{1:D3}.png" -f $n, $i)
        if (-not (Test-Path $u)) { continue }
        $table[$i] = [int]$s.Position
        $b = [Bleed]::Frame($f[$i], $u)
        if ([PixelWater]::IsWater($f[$i], $pal)) { [PixelWater]::Apply($b, 0, $f[$i], $pal) }
        else {
            [BlendBack]::Apply($b, $f[$i], $pal, $Plain)
            if ([PixelWater]::IsShore($f[$i], $wc)) { [PixelWater]::ApplyShore($b, 0, $f[$i], $pal, $wc) }
        }
        $s.Write($b, 0, $b.Length)
    }
    $s.Position = 4
    foreach ($o in $table) { $s.Write([BitConverter]::GetBytes($o), 0, 4) }
    $s.Close()
    "{0}: {1:N1} MB" -f $n, ((Get-Item (Join-Path $out "$n.bin")).Length / 1MB)
}
