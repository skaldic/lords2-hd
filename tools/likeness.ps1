# Portrait sprites keep their likeness: the original's tones under the upscaled detail, plus a share of
# the original's grain (Likeness.Patch in Bleed.cs). Always starts from the upscaler's own result, kept
# in assets\sprites\upscaled, so it can be run again with another -Grain.
#   .\tools\likeness.ps1 -Files faces -Grain 0.25
param([string[]]$Files = @('faces'), [string]$Palette = "Base01.256", [double]$Grain = 0.25)
$ErrorActionPreference = 'Stop'
$Files = $Files | ForEach-Object { $_ -split ',' }
$root = Split-Path $PSScriptRoot -Parent; $game = Split-Path $root -Parent
$store = Join-Path $root "assets\sprites\upscaled"; New-Item -ItemType Directory -Force $store | Out-Null
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs"), (Join-Path $PSScriptRoot "Bleed.cs") -ReferencedAssemblies System.Drawing
$pal = [Pl8]::ReadPalette((Join-Path $game $Palette))
foreach ($n in $Files) {
    $bin = Join-Path $game "lords2hd\sprites\$n.bin"; $keep = Join-Path $store "$n.bin"
    if (Test-Path $keep) { Copy-Item $keep $bin -Force } else { Copy-Item $bin $keep }
    "{0}: {1} frames" -f $n, [Likeness]::Patch($bin, $pal, $Grain)
}
