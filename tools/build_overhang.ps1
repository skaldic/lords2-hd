# Builds 2x art for the map tiles' overhang (trees, mountains, buildings, sheep). The layout comes
# from <game>\lords2hd_overhang.txt, written by the DLL with [Debug] DumpOverhang=1. Each frame with
# overhang is composed on a 58x66 canvas, the cell's top at row 36; the diamond is there only as
# upscaling context. Output goes to lords2hd\sprites\<set>.bin, in the sprite format.
param([string[]]$Sets = @('base1a','base1b','base1c','base1d','roads1a','roads1b','roads1c','roads1d','mtns1a','mtns1b','mtns1c','mtns1d','town1a','town1b','town1c','town1d','castle1a','castle1b','castle1c','castle1d'),
      [string]$Model = 'realesrgan-x4plus')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
$esr = Join-Path $tools "realesrgan\realesrgan-ncnn-vulkan.exe"
Add-Type -AssemblyName System.Drawing
$work = Join-Path $root "assets\overhang"; $in = Join-Path $work "in"; $up = Join-Path $work "x4"
$out = Join-Path $game "lords2hd\sprites"
Remove-Item $in, $up -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $in, $up, $out | Out-Null
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs"), (Join-Path $PSScriptRoot "Bleed.cs"), (Join-Path $PSScriptRoot "Overhang.cs") -ReferencedAssemblies System.Drawing
[Overhang]::LoadLayout((Join-Path $game "lords2hd_overhang.txt"))
$pal = [Pl8]::ReadPalette((Join-Path $game "Base01.256"))
$frames = @{}
foreach ($set in $Sets) {
    $frames[$set] = [Overhang]::Compose((Join-Path $game "$set.pl8"))
    for ($i = 0; $i -lt $frames[$set].Length; $i += 2) {
        if ($frames[$set][$i]) { [Bleed]::Save($frames[$set][$i], $pal, (Join-Path $in ("{0}_{1:D3}.png" -f $set, ($i / 2)))) }
    }
}
"{0} overhang pictures to upscale" -f (Get-ChildItem $in).Count
$ErrorActionPreference = 'Continue'
& $esr -i $in -o $up -n $Model -s 4 -f png -m (Join-Path $tools "realesrgan\models") 2>$null | Out-Null
$ErrorActionPreference = 'Stop'
if ((Get-ChildItem $up).Count -ne (Get-ChildItem $in).Count) { throw "the upscaler did not finish" }
foreach ($set in $Sets) {
    $f = $frames[$set]; $n = $f.Length / 2
    $s = [IO.File]::Create((Join-Path $out "$set.bin"))
    $s.Write([BitConverter]::GetBytes([int]$n), 0, 4)
    $s.Write((New-Object byte[] (4 * $n)), 0, 4 * $n)
    $table = New-Object int[] $n
    for ($i = 0; $i -lt $n; $i++) {
        if ($f[2 * $i + 1]) { $table[$i] = [int]$s.Position; $b = [Bleed]::Frame($f[2 * $i + 1], (Join-Path $up ("{0}_{1:D3}.png" -f $set, $i))); $s.Write($b, 0, $b.Length) }
    }
    $s.Position = 4
    foreach ($o in $table) { $s.Write([BitConverter]::GetBytes($o), 0, 4) }
    $s.Close()
    "{0}: {1:N1} MB" -f $set, ((Get-Item (Join-Path $out "$set.bin")).Length / 1MB)
}
