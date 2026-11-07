# Greatest nobles: the image model widens the picture inside the frame (sky and battlement) on each
# side, into assets\wide\grtnoble-left<seed>.png / -right<seed>.png (832x832, 2x). build_rooms.ps1
# rebuilds the frame around them (Room.Nobles).
param([int]$Seed = 1, [int]$Steps = 32)
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
$bin = Join-Path $game "lords2hd\screens\grtnoble.bin"
$prompt = "cloudy blue grey sky above a mossy green stone castle battlement with square merlons and arrow slits, medieval, 1990s 3d render"
$negative = "frame, border, picture frame, text, letters, buttons, icons, parchment, people, modern objects, blurry, photo"
foreach ($side in 'left', 'right') {
    $s = if ($side -eq 'left') { -1 } else { 1 }
    $init = Join-Path $work "_grtnoble-$side-init.png"; $mask = Join-Path $work "_grtnoble-$side-mask.png"
    [Room]::NoblesCanvas($bin, $s, $init, $mask)
    $out = Join-Path $work "grtnoble-$side$Seed.png"
    $ErrorActionPreference = 'Continue'
    & $sd -m $model --vae $vae -p $prompt -n $negative -i $init --mask $mask --strength 1.0 -W 832 -H 832 --steps $Steps --cfg-scale 6.5 --sampling-method dpm++2m -s $Seed --vae-tiling -o $out 2>&1 | ForEach-Object { "$_" } | Select-String -Pattern 'error|fail' | ForEach-Object Line
    $ErrorActionPreference = 'Stop'
    if (-not (Test-Path $out)) { throw "no picture for $side" }
}
"seed $Seed done"
