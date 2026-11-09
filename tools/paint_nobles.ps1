# Greatest nobles: the image model widens the picture inside the frame (sky and battlement) on each
# side, into assets\wide\grtnoble-left<seed>.png / -right<seed>.png (832x832, 2x). build_rooms.ps1
# rebuilds the frame around them (Room.Nobles).
param([int]$Seed = 1, [int]$Steps = 30)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
$sd = Join-Path $tools "sdcpp\bin\sd-cli.exe"
$models = Join-Path $tools "sdcpp\models"   # FLUX.1 Fill [dev], as tools\expand_sides.ps1
Add-Type -Path (Join-Path $PSScriptRoot "Room.cs") -ReferencedAssemblies System.Drawing
$work = Join-Path $root "assets\wide"
New-Item -ItemType Directory -Force $work | Out-Null
$bin = Join-Path $game "lords2hd\screens\grtnoble.bin"
$prompt = "A pale blue-grey cloudy sky above a castle battlement of grey-green mossy stone blocks with evenly spaced square merlons and arrow slits, 1990s pre-rendered 3D computer game art. The sky and the battlement wall continue evenly to the edge, the merlons all alike."
foreach ($side in 'left', 'right') {
    $s = if ($side -eq 'left') { -1 } else { 1 }
    $init = Join-Path $work "_grtnoble-$side-init.png"; $mask = Join-Path $work "_grtnoble-$side-mask.png"
    [Room]::NoblesCanvas($bin, $s, $init, $mask)
    $out = Join-Path $work "grtnoble-$side$Seed.png"
    $a = @('--diffusion-model', (Join-Path $models "flux1-fill-dev-Q6_K.gguf"), '--vae', (Join-Path $models "flux_ae.safetensors"),
        '--clip_l', (Join-Path $models "clip_l.safetensors"), '--t5xxl', (Join-Path $models "t5xxl-Q8_0.gguf"), '--backend', 'te=cpu',
        '-p', $prompt, '-i', $init, '--mask', $mask, '--strength', '1.0', '-W', 832, '-H', 832, '--steps', $Steps,
        '--cfg-scale', '1.0', '--guidance', 30, '--sampling-method', 'euler', '-s', $Seed, '--vae-tiling', '-o', $out)
    $line = ($a | ForEach-Object { '"' + "$_" + '"' }) -join ' '
    $log = Join-Path $work "_grtnoble-$side-flux.log"
    $proc = Start-Process $sd -ArgumentList $line -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.err"
    $proc.PriorityClass = 'BelowNormal'; $proc.WaitForExit()
    if (-not (Test-Path $out)) { throw "no picture for $side" }
}
"seed $Seed done"
