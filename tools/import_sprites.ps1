# Converts a folder of <set>\NN.png (2x, RGBA) into <Game>\lords2hd\sprites\<set><Suffix>.bin, starting from the
# pak's file for its 1x frames. -Suffix _winter etc. writes a set's season file (tools\season_ground.py).
#   .\tools\import_sprites.ps1 -In <folder> -Game <game folder> -Pak <game>\lords2hd.pak [-Sets vill,villani2] [-Suffix _winter]
param([string]$In, [string]$Game, [string]$Pak, [string[]]$Sets = @('vill', 'villtops', 'villani1', 'villani2', 'iconvill', 'hearth'), [string]$Suffix = '')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $PSScriptRoot "SpriteArt.cs") -ReferencedAssemblies System.Drawing
$out = Join-Path $Game "lords2hd\sprites"; New-Item -ItemType Directory -Force $out | Out-Null
foreach ($n in $Sets) {
    $bin = [SpriteArt]::FromPak($Pak, "sprites/$n.bin"); $done = 0; $skip = @()
    for ($i = 0; $i -lt [SpriteArt]::Frames($bin); $i++) {
        $png = Join-Path $In ("$n\{0:d2}.png" -f $i)
        if (-not (Test-Path $png)) { continue }
        if (-not [SpriteArt]::Offset($bin, $i)) { $skip += "$i (no HD slot)"; continue }
        $e = [SpriteArt]::Import($bin, $i, $png)
        if ($e) { $skip += "$i ($e)" } else { $done++ }
    }
    [IO.File]::WriteAllBytes((Join-Path $out "$n$Suffix.bin"), $bin)
    "{0}: {1} frames imported{2}" -f $n, $done, $(if ($skip) { "; skipped " + ($skip -join ', ') } else { "" })
}
