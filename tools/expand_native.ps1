# Like expand_sides.ps1, but each side is painted on its own at the painting's full 2x size (an
# 832x960 close-up: the new columns plus the 618 next to them), so the new parts are as sharp as the
# painting and need no enlarging. Slower: the model loads once per side.
# Results in assets\wide: _<name>-fxleft-<seed>.png / -fxright-, <name>-flux<seed>.bin and .png.
#   .\tools\expand_native.ps1 -Name gateway -Prompt "..." -Seed 11 -Count 2
[CmdletBinding(PositionalBinding = $false)]
param([string]$Name, [string]$Prompt, [int]$Seed = 11, [int]$Count = 2, [int]$Overlap = 24, [int]$Steps = 30, [double]$Guidance = 30, [switch]$ComposeOnly,
    [string]$From = "", [double]$Strength = 1.0) # -From: a 1708x960 picture of the whole wide screen to start the sides from (with -Strength below 1 the model keeps its layout and light and only redraws the detail)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
$sd = Join-Path $tools "sdcpp\bin\sd-cli.exe"
$models = Join-Path $tools "sdcpp\models"
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs"), (Join-Path $PSScriptRoot "Room.cs") -ReferencedAssemblies System.Drawing
$work = Join-Path $root "assets\wide"
function Rect($x, $y, $w, $h) { New-Object Drawing.Rectangle $x, $y, $w, $h }
$PW = 1280; $H = 960; $W = 1708; $M = 214; $CW = 832
$screen = Join-Path $game "lords2hd\screens\$Name.bin"
$bytes = [IO.File]::ReadAllBytes($screen)
$pic = New-Object Drawing.Bitmap $PW, $H, ([Drawing.Imaging.PixelFormat]::Format32bppRgb)
$d = $pic.LockBits((Rect 0 0 $PW $H), 'WriteOnly', $pic.PixelFormat)
[Runtime.InteropServices.Marshal]::Copy($bytes, 768, $d.Scan0, $PW * $H * 4); $pic.UnlockBits($d)
foreach ($side in 'left', 'right') {
    if ($ComposeOnly) { break }
    $x0 = if ($side -eq 'left') { 0 } else { $W - $CW }      # the canvas's first frame column
    $init = New-Object Drawing.Bitmap $CW, $H, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($init); $g.Clear([Drawing.Color]::Black)
    if ($From) { $fr = [Drawing.Image]::FromFile($From); $g.DrawImage($fr, (Rect (-$x0) 0 $W $H)); $fr.Dispose() }
    $g.DrawImage($pic, (Rect ($M - $x0) 0 $PW $H)); $g.Dispose()
    $mask = New-Object Drawing.Bitmap $CW, $H, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($mask); $g.Clear([Drawing.Color]::Black)
    $mw = $M + $Overlap; $mx = if ($side -eq 'left') { 0 } else { $CW - $mw }
    $g.FillRectangle([Drawing.Brushes]::White, $mx, 0, $mw, $H); $g.Dispose()
    $initPath = Join-Path $work "_$Name-n$side-init.png"; $maskPath = Join-Path $work "_$Name-n$side-mask.png"
    $init.Save($initPath); $mask.Save($maskPath); $init.Dispose(); $mask.Dispose()
    $log = Join-Path $work "_$Name-n$side.log"
    $a = @('--diffusion-model', (Join-Path $models "flux1-fill-dev-Q6_K.gguf"), '--vae', (Join-Path $models "flux_ae.safetensors"),
        '--clip_l', (Join-Path $models "clip_l.safetensors"), '--t5xxl', (Join-Path $models "t5xxl-Q8_0.gguf"), '--backend', 'te=cpu',
        '-p', $Prompt, '-i', $initPath, '--mask', $maskPath, '--strength', $Strength, '-W', $CW, '-H', $H, '--steps', $Steps,
        '--cfg-scale', '1.0', '--guidance', $Guidance, '--sampling-method', 'euler', '-s', $Seed, '-b', $Count,
        '--output-begin-idx', $Seed, '--vae-tiling', '-o', (Join-Path $work "_$Name-fx$side-%d.png"))
    $line = ($a | ForEach-Object { '"' + "$_" + '"' }) -join ' '
    $proc = Start-Process $sd -ArgumentList $line -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $proc.PriorityClass = 'BelowNormal'; $proc.WaitForExit()
}
foreach ($s in $Seed..($Seed + $Count - 1)) {
    $l = Join-Path $work "_$Name-fxleft-$s.png"; $r = Join-Path $work "_$Name-fxright-$s.png"
    if (-not ((Test-Path $l) -and (Test-Path $r))) { "no pictures for seed $s"; continue }
    $bin = Join-Path $work "$Name-flux$s.bin"
    [Room]::PaintedRaw($screen, $l, $r, $bin)
    $b = [IO.File]::ReadAllBytes($bin)
    $view = New-Object Drawing.Bitmap $W, $H, ([Drawing.Imaging.PixelFormat]::Format32bppRgb)
    $d = $view.LockBits((Rect 0 0 $W $H), 'WriteOnly', $view.PixelFormat)
    [Runtime.InteropServices.Marshal]::Copy($b, 20, $d.Scan0, $W * $H * 4); $view.UnlockBits($d)
    $view.Save((Join-Path $work "$Name-flux$s.png")); $view.Dispose()
    "$Name seed $s done"
}
$pic.Dispose()
