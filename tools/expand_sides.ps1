# Widens a 640x480 screen to 16:9 with FLUX.1 Fill (stable-diffusion.cpp): the model sees the whole 2x
# picture with empty sides and carries the scene on. The original stays untouched in the middle.
# Results in assets\wide: <name>-flux<seed>.bin (a wide room file) and .png (the same, to look at),
# and the side canvases _<name>-fxleft-<seed>.png / -fxright- that build_rooms.ps1 uses.
#   .\tools\expand_sides.ps1 -Name merchant -Prompt "a medieval market yard ..." -Seed 1 -Count 3
# Models (tools dir, sdcpp\models): flux1-fill-dev-Q6_K.gguf, flux_ae.safetensors, clip_l.safetensors,
# t5xxl-Q8_0.gguf. FLUX.1 Fill [dev] is licensed for non-commercial use.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Name,
    [string]$Prompt,
    [int]$Seed = 1,       # first seed; -Count images are made in one run (the model loads once)
    [int]$Count = 1,
    [int]$Overlap = 16,
    [int]$Steps = 30,
    [double]$Guidance = 30,
    [int]$GenW = 1136,
    [int]$GenH = 640,
    [switch]$ComposeOnly   # no painting: build again from the model's pictures already in assets\wide
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
$sd = Join-Path $tools "sdcpp\bin\sd-cli.exe"
$models = Join-Path $tools "sdcpp\models"
Add-Type -AssemblyName System.Drawing
$work = Join-Path $root "assets\wide"
New-Item -ItemType Directory -Force $work | Out-Null
function Rect($x, $y, $w, $h) { New-Object Drawing.Rectangle $x, $y, $w, $h }

# The 2x painting the game shows (lords2hd\screens\<name>.bin: 768 bytes of palette, 1280x960 BGRX).
$PW = 1280; $H = 960; $W = 1712; $M = ($W - $PW) / 2   # 1712: the model wants multiples of 16
$bytes = [IO.File]::ReadAllBytes((Join-Path $game "lords2hd\screens\$Name.bin"))
$pic = New-Object Drawing.Bitmap $PW, $H, ([Drawing.Imaging.PixelFormat]::Format32bppRgb)
$d = $pic.LockBits((Rect 0 0 $PW $H), 'WriteOnly', $pic.PixelFormat)
[Runtime.InteropServices.Marshal]::Copy($bytes, 768, $d.Scan0, $PW * $H * 4); $pic.UnlockBits($d)

$init = New-Object Drawing.Bitmap $W, $H, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [Drawing.Graphics]::FromImage($init); $g.Clear([Drawing.Color]::Black); $g.DrawImage($pic, (Rect $M 0 $PW $H)); $g.Dispose()
# The model paints at -GenW x -GenH (the full size does not fit the card's buffers); its sides are enlarged back.
$small = New-Object Drawing.Bitmap $init, $GenW, $GenH
$initPath = Join-Path $work "_$Name-flux-init.png"; $small.Save($initPath); $small.Dispose()
$mask = New-Object Drawing.Bitmap $W, $H, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [Drawing.Graphics]::FromImage($mask); $g.Clear([Drawing.Color]::Black)
$g.FillRectangle([Drawing.Brushes]::White, 0, 0, $M + $Overlap, $H); $g.FillRectangle([Drawing.Brushes]::White, $W - $M - $Overlap, 0, $M + $Overlap, $H); $g.Dispose()
$small = New-Object Drawing.Bitmap $mask, $GenW, $GenH
$maskPath = Join-Path $work "_$Name-flux-mask.png"; $small.Save($maskPath); $small.Dispose(); $mask.Dispose()

# sd-cli runs at low priority with its output going straight to a log that can be read while it works.
$log = Join-Path $work "_$Name-flux.log"
$a = @('--diffusion-model', (Join-Path $models "flux1-fill-dev-Q6_K.gguf"), '--vae', (Join-Path $models "flux_ae.safetensors"),
    '--clip_l', (Join-Path $models "clip_l.safetensors"), '--t5xxl', (Join-Path $models "t5xxl-Q8_0.gguf"), '--backend', 'te=cpu',
    '-p', $Prompt, '-i', $initPath, '--mask', $maskPath, '--strength', '1.0', '-W', $GenW, '-H', $GenH, '--steps', $Steps,
    '--cfg-scale', '1.0', '--guidance', $Guidance, '--sampling-method', 'euler', '-s', $Seed, '-b', $Count,
    '--output-begin-idx', $Seed, '--vae-tiling', '-o', (Join-Path $work "_$Name-flux-raw%d.png"))
$line = ($a | ForEach-Object { '"' + "$_" + '"' }) -join ' '
if (-not $ComposeOnly) {
    $proc = Start-Process $sd -ArgumentList $line -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $proc.PriorityClass = 'BelowNormal'
    $proc.WaitForExit()
}
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs"), (Join-Path $PSScriptRoot "Room.cs") -ReferencedAssemblies System.Drawing
$esr = Join-Path $tools "realesrgan\realesrgan-ncnn-vulkan.exe"
foreach ($s in $Seed..($Seed + $Count - 1)) {
    $raw = Join-Path $work "_$Name-flux-raw$s.png"
    if (-not (Test-Path $raw)) { throw "no image for seed $s - see _$Name-flux.log" }
    # The model's picture enlarged the same way as the painting in the middle (Real-ESRGAN), then cut
    # into the two side canvases Room.PaintedRaw takes: it matches their colour to the painting row
    # by row and fades them in at the joins. The original stays untouched in the middle.
    $x4 = Join-Path $work "_$Name-flux-x4-$s.png"
    $ErrorActionPreference = 'Continue'
    & $esr -i $raw -o $x4 -n realesrgan-x4plus -s 4 -f png -m (Join-Path $tools "realesrgan\models") 2>$null | Out-Null
    $ErrorActionPreference = 'Stop'
    $gen = [Drawing.Bitmap]::FromFile($(if (Test-Path $x4) { $x4 } else { $raw }))
    $full = New-Object Drawing.Bitmap $W, $H, ([Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [Drawing.Graphics]::FromImage($full); $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.DrawImage($gen, (Rect 0 0 $W $H)); $g.Dispose(); $gen.Dispose()
    $left = Join-Path $work "_$Name-fxleft-$s.png"; $right = Join-Path $work "_$Name-fxright-$s.png"
    $c = $full.Clone((Rect 2 0 832 $H), $full.PixelFormat); $c.Save($left); $c.Dispose()
    $c = $full.Clone((Rect ($W - 2 - 832) 0 832 $H), $full.PixelFormat); $c.Save($right); $c.Dispose(); $full.Dispose()
    $bin = Join-Path $work "$Name-flux$s.bin"
    [Room]::PaintedRaw((Join-Path $game "lords2hd\screens\$Name.bin"), $left, $right, $bin)
    # a full-size picture of the result, to look at
    $b = [IO.File]::ReadAllBytes($bin)
    $view = New-Object Drawing.Bitmap 1708, $H, ([Drawing.Imaging.PixelFormat]::Format32bppRgb)
    $d = $view.LockBits((Rect 0 0 1708 $H), 'WriteOnly', $view.PixelFormat)
    [Runtime.InteropServices.Marshal]::Copy($b, 20, $d.Scan0, 1708 * $H * 4); $view.UnlockBits($d)
    $view.Save((Join-Path $work "$Name-flux$s.png")); $view.Dispose()
    "$Name seed $s done"
}
$pic.Dispose(); $init.Dispose()