# Drives the game in remote-testing mode ([Debug] Remote=1 in lords2hd.ini: its window is cloaked and
# click-through, its sound muted) by posting messages to it: nothing touches the real mouse, keyboard
# or screen. Positions are the game's own coordinates (640x480; on the widened map x runs negative
# over the spare columns, as drive.ps1's wclick).
#   wait:<seconds>  click:<x>,<y>  rclick:<x>,<y>  move:<x>,<y>  key:{ESC}|{ENTER}|...  shot:<name>
# -Start launches the game first (and waits for its window); -Stop closes it at the end.
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Steps,
    [switch]$Start,
    [switch]$Stop
)
$game = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$shots = Join-Path $PSScriptRoot "shots"
Add-Type -AssemblyName System.Drawing
Add-Type -Namespace BG -Name M -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l); [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);'
$info = Join-Path $game "lords2hd-remote.txt"
if ($Start) {
    Get-Process Lords2 -ErrorAction SilentlyContinue | Stop-Process -Force
    Remove-Item $info -ErrorAction SilentlyContinue
    Start-Process (Join-Path $game "Lords2.exe") -WorkingDirectory $game
    $t = 0; while (-not (Test-Path $info) -and $t -lt 200) { Start-Sleep -Milliseconds 100; $t++ }
}
$view = [IntPtr][long]((Get-Content $info -ErrorAction Stop) -split ' ')[0]
$base = 0x8100   # WM_REMOTE in lords2hd.c
function Post([int]$n, [long]$w, [long]$l) { [void][BG.M]::PostMessage($view, $base + $n, [IntPtr]$w, [IntPtr]$l) }
function XY([string]$a) { $x, $y = $a -split ','; return ([long][int]$y -shl 16) -bor ([int]$x -band 0xffff) }
$keys = @{ '{ESC}' = 0x1b; '{ENTER}' = 0x0d; '{LEFT}' = 0x25; '{UP}' = 0x26; '{RIGHT}' = 0x27; '{DOWN}' = 0x28; '{TAB}' = 0x09; '{BACKSPACE}' = 0x08; '{SPACE}' = 0x20 }
New-Item -ItemType Directory -Force $shots | Out-Null
foreach ($step in $Steps) {
    if (-not [BG.M]::IsWindow($view)) { "the game's window is gone (step '$step')"; break }
    $verb, $arg = $step -split ':', 2
    switch ($verb) {
        'wait'   { Start-Sleep -Milliseconds ([int]([double]$arg * 1000)) }
        'move'   { Post 1 0 (XY $arg); Start-Sleep -Milliseconds 60 }
        'click'  { $l = XY $arg; Post 1 0 $l; Start-Sleep -Milliseconds 120; Post 2 0 $l; Start-Sleep -Milliseconds 80; Post 3 0 $l; Start-Sleep -Milliseconds 60 }
        'rclick' { $l = XY $arg; Post 1 0 $l; Start-Sleep -Milliseconds 120; Post 5 0 $l; Start-Sleep -Milliseconds 80; Post 6 0 $l; Start-Sleep -Milliseconds 60 }
        'key'    {
            if ($keys.ContainsKey($arg)) { Post 4 $keys[$arg] 0 }
            else { foreach ($c in $arg.ToCharArray()) { Post 4 ([int][char]([string]$c).ToUpper()) 0; Start-Sleep -Milliseconds 40 } }
        }
        'shot'   {
            $f = Join-Path $game "lords2hd-shot.bmp"; Remove-Item $f -ErrorAction SilentlyContinue
            Post 0x10 0 0
            $t = 0; while (-not (Test-Path $f) -and $t -lt 50) { Start-Sleep -Milliseconds 100; $t++ }
            if (Test-Path $f) {
                Start-Sleep -Milliseconds 100
                $b = [Drawing.Bitmap]::FromFile($f); $b.Save((Join-Path $shots "$arg.png"), [Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
                Remove-Item $f; "saved $arg.png"
            } else { "no frame for $arg" }
        }
        default  { "unknown step '$step'" }
    }
}
if ($Stop) { Get-Process Lords2 -ErrorAction SilentlyContinue | Stop-Process -Force }
