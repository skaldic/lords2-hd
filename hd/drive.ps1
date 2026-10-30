# Launches the game and plays back a list of steps, for testing without a person at the mouse.
#   wait:<seconds>      pause
#   click:<x>,<y>       left-click at a point given in 640x480 game coordinates
#   rclick:<x>,<y>      right-click there
#   move:<x>,<y>        move the mouse there without clicking
#   wclick / wrclick / wmove:<x>,<y>   the same on a widened screen (county map in Wide=1): x is
#                       the game's, -208..639 on a 16:9 monitor (the spare columns are negative)
#   key:<sendkeys>      press a key, e.g. key:{ESC}
#   shot:<name>         save a half-size screenshot as shots\<name>.png
#   crop:<name>         save a full-resolution crop of the centre of the screen
# Example: .\drive.ps1 wait:9 key:{ESC} wait:3 shot:menu
param(
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Steps,
    [switch]$KeepRunning,
    [switch]$Attach     # play the steps on the game that is already running instead of starting it
)
$GameW = 640; $GameH = 480

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type -Namespace Native -Name Drive -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
[StructLayout(LayoutKind.Sequential)] public struct CURSORINFO { public int cbSize; public int flags; public IntPtr hCursor; public int x; public int y; }
[StructLayout(LayoutKind.Sequential)] public struct ICONINFO { public bool fIcon; public int xHotspot; public int yHotspot; public IntPtr hbmMask; public IntPtr hbmColor; }
[DllImport("user32.dll")] public static extern bool GetCursorInfo(ref CURSORINFO ci);
[DllImport("user32.dll")] public static extern bool GetIconInfo(IntPtr icon, out ICONINFO ii);
[DllImport("user32.dll")] public static extern bool DrawIconEx(IntPtr dc, int x, int y, IntPtr icon, int w, int h, int step, IntPtr brush, int flags);
[DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
'@

# Screen captures leave the cursor out; draw it in, at the size Windows shows it.
function Add-Cursor($graphics) {
    $ci = New-Object Native.Drive+CURSORINFO
    $ci.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($ci)
    if (-not [Native.Drive]::GetCursorInfo([ref]$ci) -or $ci.flags -ne 1) { return }
    $ii = New-Object Native.Drive+ICONINFO
    if (-not [Native.Drive]::GetIconInfo($ci.hCursor, [ref]$ii)) { return }
    $dc = $graphics.GetHdc()
    [void][Native.Drive]::DrawIconEx($dc, $ci.x - $ii.xHotspot, $ci.y - $ii.yHotspot, $ci.hCursor, 0, 0, 0, [IntPtr]::Zero, 3)
    $graphics.ReleaseHdc($dc)
    if ($ii.hbmMask -ne [IntPtr]::Zero) { [void][Native.Drive]::DeleteObject($ii.hbmMask) }
    if ($ii.hbmColor -ne [IntPtr]::Zero) { [void][Native.Drive]::DeleteObject($ii.hbmColor) }
}

# Clicks and keys are real input: they go to whatever window is in front. They are sent only
# while the game is in front, so another program is never clicked.
function Test-GameInFront {
    $pid2 = [uint32]0
    [void][Native.Drive]::GetWindowThreadProcessId([Native.Drive]::GetForegroundWindow(), [ref]$pid2)
    return $pid2 -eq $proc.Id
}
[void][Native.Drive]::SetProcessDPIAware()

$outDir = "$PSScriptRoot\shots"
New-Item -ItemType Directory -Force $outDir | Out-Null
$game = Resolve-Path "$PSScriptRoot\..\.."
$proc = if ($Attach) { Get-Process "Lords2" | Select-Object -First 1 }
        else { Start-Process -FilePath "$game\Lords2.exe" -WorkingDirectory $game -PassThru }

# A launch from a background process may not get focus: ask for it, a few times.
if (-not $Attach) {
    $shell = New-Object -ComObject WScript.Shell
    for ($i = 0; $i -lt 8 -and -not (Test-GameInFront); $i++) { Start-Sleep -Milliseconds 500; [void]$shell.AppActivate($proc.Id) }
}

Add-Type -Namespace Native -Name Win -MemberDefinition @'
[StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
[StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
'@

function Update-View {
    # The picture is the largest 4:3 rectangle centred in the game's window (normally the whole
    # primary screen; a window with Fullscreen=0). $b is that area in screen pixels.
    $proc.Refresh()
    $r = New-Object Native.Win+RECT; $pt = New-Object Native.Win+POINT
    if ($proc.MainWindowHandle -ne [IntPtr]::Zero -and [Native.Win]::GetClientRect($proc.MainWindowHandle, [ref]$r) -and $r.r -gt 0) {
        [void][Native.Win]::ClientToScreen($proc.MainWindowHandle, [ref]$pt)
        $script:b = New-Object Drawing.Rectangle $pt.x, $pt.y, $r.r, $r.b
    } else { $script:b = [Windows.Forms.Screen]::PrimaryScreen.Bounds }
    $script:scale = [Math]::Min($b.Width / [double]$GameW, $b.Height / [double]$GameH)
    $script:ox = ($b.Width - $GameW * $scale) / 2
    $script:oy = ($b.Height - $GameH * $scale) / 2
}
Update-View

function Move-To([int]$gx, [int]$gy, [switch]$Wide, [switch]$Battle) {
    Update-View
    if ($Battle) {
        # The zoomed-out battle: the battlefield 1:1 on a picture twice the size, the panel doubled.
        $shown = [Math]::Max(640, [Math]::Floor(480 * $b.Width / $b.Height / 16) * 16)
        $s = $b.Height / 960.0
        $mapW = 2 * $shown - 324
        if ($gy -lt 24) { $lx = ($gx + $shown - 640) * 2; $ly = $gy * 2 }
        elseif ($gx -ge 478) { $lx = $mapW + ($gx - 478) * 2; $ly = 48 + ($gy - 24) * 2 } else { $lx = $gx + $mapW - 478; $ly = $gy + 24 }
        $sx = [int]($b.X + ($b.Width - 2 * $shown * $s) / 2 + ($lx + 0.5) * $s); $sy = [int]($b.Y + ($ly + 0.5) * $s)
        for ($i = 8; $i -ge 0; $i--) { [void][Native.Drive]::SetCursorPos($sx - $i * 5, $sy - $i * 3); Start-Sleep -Milliseconds 25 }
        return
    }
    if ($Wide) {
        # Wide mode shows the frame's spare columns too: as many as fit the monitor at 480 lines,
        # rounded down to 16 (lords2hd.c install_wide).
        $shown = [Math]::Max(640, [Math]::Floor(480 * $b.Width / $b.Height / 16) * 16)
        $s = $b.Height / 480.0
        $sx = [int]($b.X + ($b.Width - $shown * $s) / 2 + ($gx + $shown - 640 + 0.5) * $s); $sy = [int]($b.Y + ($gy + 0.5) * $s)
    } else {
        $sx = [int]($b.X + $ox + ($gx + 0.5) * $scale); $sy = [int]($b.Y + $oy + ($gy + 0.5) * $scale)
    }
    # Arrive in a few steps so the game sees ordinary mouse movement.
    for ($i = 8; $i -ge 0; $i--) { [void][Native.Drive]::SetCursorPos($sx - $i * 5, $sy - $i * 3); Start-Sleep -Milliseconds 25 }
}

function Click([int]$down, [int]$up) {
    Start-Sleep -Milliseconds 150
    [Native.Drive]::mouse_event($down, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 80
    [Native.Drive]::mouse_event($up, 0, 0, 0, [UIntPtr]::Zero)
}

foreach ($step in $Steps) {
    if ($proc.HasExited) { "game exited (code $($proc.ExitCode)) before '$step'"; break }
    $verb, $arg = $step -split ':', 2
    if ($verb -in 'click', 'rclick', 'key', 'move', 'wclick', 'wrclick', 'wmove', 'bmove', 'bclick', 'bdrag', 'wdrag' -and -not (Test-GameInFront)) { "the game is not in front: stopped before '$step'"; break }
    switch ($verb) {
        'wait'   { Start-Sleep -Milliseconds ([int]([double]$arg * 1000)) }
        'move'   { $x, $y = $arg -split '[, ]+'; Move-To $x $y }
        'click'  { $x, $y = $arg -split '[, ]+'; Move-To $x $y; Click 2 4 }
        'rclick' { $x, $y = $arg -split '[, ]+'; Move-To $x $y; Click 8 16 }
        'bmove'  { $x, $y = $arg -split '[, ]+'; Move-To $x $y -Battle }
        'bclick' { $x, $y = $arg -split '[, ]+'; Move-To $x $y -Battle; Click 2 4 }
        'bdrag'  {
            $v = $arg -split '[, ]+'
            Move-To $v[0] $v[1] -Battle; Start-Sleep -Milliseconds 200
            [Native.Drive]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 300
            Move-To $v[2] $v[3] -Battle; Start-Sleep -Milliseconds 500
            [Native.Drive]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
        }
        'wdrag'  {
            # wdrag:x1,y1,x2,y2 : a left-button drag on a widened screen
            $v = $arg -split '[, ]+'
            Move-To $v[0] $v[1] -Wide; Start-Sleep -Milliseconds 200
            [Native.Drive]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 300
            Move-To $v[2] $v[3] -Wide; Start-Sleep -Milliseconds 500
            [Native.Drive]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)
        }
        'wmove'  { $x, $y = $arg -split '[, ]+'; Move-To $x $y -Wide }
        'wclick' { $x, $y = $arg -split '[, ]+'; Move-To $x $y -Wide; Click 2 4 }
        'wrclick' { $x, $y = $arg -split '[, ]+'; Move-To $x $y -Wide; Click 8 16 }
        'key'    { [Windows.Forms.SendKeys]::SendWait($arg) }
        'shot'   {
            Update-View
            $full = New-Object Drawing.Bitmap($b.Width, $b.Height)
            $g = [Drawing.Graphics]::FromImage($full)
            $g.CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size)
            Add-Cursor $g
            # A screen is saved at half size, a window at full size.
            $div = if ($b.Width -ge 2560) { 2 } else { 1 }
            $half = New-Object Drawing.Bitmap($full, [int]($b.Width / $div), [int]($b.Height / $div))
            $half.Save("$outDir\$arg.png", [Drawing.Imaging.ImageFormat]::Png)
            $g.Dispose(); $full.Dispose(); $half.Dispose()
            "saved $arg.png ($($b.Width)x$($b.Height))"
        }
        'crop'   {
            $bmp = New-Object Drawing.Bitmap(1000, 700)
            $g = [Drawing.Graphics]::FromImage($bmp)
            $g.CopyFromScreen(($b.Width / 2 - 500), ($b.Height / 2 - 350), 0, 0, $bmp.Size)
            $bmp.Save("$outDir\$arg.png", [Drawing.Imaging.ImageFormat]::Png)
            $g.Dispose(); $bmp.Dispose()
            "saved $arg.png"
        }
        default { "unknown step '$step'" }
    }
}

if (-not $KeepRunning -and -not $proc.HasExited) {
    [void]$proc.CloseMainWindow()
    if (-not $proc.WaitForExit(3000)) { Stop-Process -Id $proc.Id -Force }
}
