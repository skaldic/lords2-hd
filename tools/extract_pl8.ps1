# Extracts every .pl8 picture of the game to assets\pl8\<file>\<frame>.png and writes
# assets\pl8\index.csv (file, frame, size, offset, palette, error).
# Palette: the .256 of the same name when there is one, else -DefaultPalette.
param(
    [string]$Game = (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent),
    [string]$Out = (Join-Path (Split-Path $PSScriptRoot -Parent) "assets\pl8"),
    [string]$DefaultPalette = "Lords2.256",
    [string]$Filter = "*.pl8",
    [switch]$KeyMagenta
)
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs") -ReferencedAssemblies System.Drawing
New-Item -ItemType Directory -Force $Out | Out-Null

$palettes = @{}
Get-ChildItem $Game -Filter *.256 | ForEach-Object { $palettes[$_.BaseName.ToLower()] = $_.FullName }
$cache = @{}
function Palette($name) {
    if (-not $cache.ContainsKey($name)) { $cache[$name] = [Pl8]::ReadPalette($palettes[$name]) }
    $cache[$name]
}

$rows = New-Object System.Collections.Generic.List[string]
$rows.Add("file,frame,compressed,width,height,x,y,extra,offset,palette,error")
$bad = 0; $total = 0
foreach ($file in Get-ChildItem $Game -Filter $Filter) {
    $base = $file.BaseName.ToLower()
    $palName = if ($palettes.ContainsKey($base)) { $base } else { [IO.Path]::GetFileNameWithoutExtension($DefaultPalette).ToLower() }
    $pal = Palette $palName
    $c = 0; $h = ""
    $frames = [Pl8]::Read($file.FullName, [ref]$c, [ref]$h)
    $dir = Join-Path $Out $base
    New-Item -ItemType Directory -Force $dir | Out-Null
    for ($i = 0; $i -lt $frames.Length; $i++) {
        $f = $frames[$i]
        $total++
        if ($f.Error) { $bad++ } else { [Pl8]::SavePng($f, $pal, (Join-Path $dir ("{0:D3}.png" -f $i)), $KeyMagenta.IsPresent) }
        $rows.Add(("{0},{1},{2},{3},{4},{5},{6},{7:X8},{8},{9},{10}" -f $base, $i, $c, $f.Width, $f.Height, $f.X, $f.Y, $f.Extra, $f.Offset, $palName, $f.Error))
    }
}
[IO.File]::WriteAllLines((Join-Path $Out "index.csv"), $rows)
"{0} frames, {1} failed to decode -> {2}" -f $total, $bad, $Out
