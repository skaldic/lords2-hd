# Integration checks: seeded custom battles played hidden and muted in a COPY of the game, never your own.
#   tests\integration.ps1 -Game "D:\L2HD-test\Lords of the Realm II" [-RefDll <another ddraw.dll>] [-Battles tb,sq] [-Secs 90]
# Checks, per battle:
#   1. no crash: lords2hd.log has no exception
#   2. the same battle twice gives the same hash every second (the game and the patch's battle fixes are
#      deterministic: multiplayer games stay in step only if they are)
#   3. with -RefDll (e.g. the last release's ddraw.dll): the same hashes as that build, so the battles play
#      out unchanged; otherwise the first second that differs (expected when the battle code changed on purpose)
# The copy's ddraw.dll and lords2hd.ini are put back afterwards. Takes about 2 minutes a battle and DLL.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Dll = "$PSScriptRoot\..\hd\ddraw.dll",
    [string]$RefDll,
    [string[]]$Battles = @('tb', 'field', 'siege', 'sq'),
    [int]$Secs = 90
)
$ErrorActionPreference = 'Stop'
$Game = (Resolve-Path $Game).Path.TrimEnd('\')
$yours = (Resolve-Path "$PSScriptRoot\..\..").Path.TrimEnd('\')   # the game folder this repository sits in
if ($Game -ieq $yours) { throw "integration: $Game is your own game folder; give a copy of the game" }
if (-not (Test-Path "$Game\Lords2.exe")) { throw "integration: no Lords2.exe in $Game" }
$Dll = (Resolve-Path $Dll).Path
if ($RefDll) { $RefDll = (Resolve-Path $RefDll).Path }
$out = "$PSScriptRoot\out\integration"
New-Item -ItemType Directory -Force $out | Out-Null

Add-Type -Namespace L2T -Name Ini -MemberDefinition '[DllImport("kernel32.dll", CharSet = CharSet.Ansi)] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'
$ini = "$Game\lords2hd.ini"
$keep = "$out\keep"
New-Item -ItemType Directory -Force $keep | Out-Null
Copy-Item "$Game\ddraw.dll" "$keep\ddraw.dll" -Force -ErrorAction SilentlyContinue
Copy-Item $ini "$keep\lords2hd.ini" -Force -ErrorAction SilentlyContinue

# Custom battle maps (the list on the custom battle screen, x 533) and what is done in each. Orders go in while the
# battle is still paused at its start ('paused'), so they land on the same battle step every run: sent a few seconds
# into the battle, the same order landed one step apart from run to run and the battles went different ways.
$defs = @{
    tb    = @{ pre = @('click:533,192'); paused = @() }                   # Three Bridges
    field = @{ pre = @(); paused = @() }                                  # the default map
    siege = @{ pre = @('click:480,306'); paused = @() }                   # a siege
    sq    = @{ pre = @('click:533,256'); paused = @('march:40,64', 'wait:1') } # Squigglewood, a march round the forest
}
$boot = @('wait:7', 'click:320,240', 'wait:1', 'click:320,240', 'wait:1', 'click:320,240', 'wait:3', 'click:320,102')

# A closed game keeps its files a moment: wait for this copy's game to be gone, then copy (a few tries)
function Copy-IntoGame($from, $to) {
    Get-Process Lords2 -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq "$Game\Lords2.exe" } | ForEach-Object { [void]$_.WaitForExit(10000) }
    for ($i = 0; ; $i++) {
        try { Copy-Item $from $to -Force -ErrorAction Stop; return }
        catch { if ($i -ge 40) { throw }; Start-Sleep -Milliseconds 250 }
    }
}
function Invoke-Battle($name, $dll, $tag) {
    Copy-IntoGame $dll "$Game\ddraw.dll"
    foreach ($k in @(@('Debug', 'Remote', '1'), @('Debug', 'RemoteVisible', '0'), @('Debug', 'BattleSeed', '1'), @('Debug', 'BattleLog', '1'),
            @('Debug', 'Timing', '0'), @('Multiplayer', 'MpLogs', '0'))) { [void][L2T.Ini]::WritePrivateProfileString($k[0], $k[1], $k[2], $ini) }
    Remove-Item "$Game\lords2hd-battle.txt", "$Game\lords2hd.log" -ErrorAction SilentlyContinue
    $d = $defs[$name]
    $steps = $boot + @('wait:4', 'click:320,174', 'wait:6', 'click:431,226', 'wait:10') + $d.pre +
        @('wait:2', 'click:605,446', 'wait:8') + $d.paused + @('click:496,460', "wait:$Secs")
    & "$PSScriptRoot\..\hd\bgdrive.ps1" -Game $Game -Start -Stop @steps | Out-Null
    $file = "$out\$name-$tag.txt"
    Copy-Item "$Game\lords2hd-battle.txt" $file -Force -ErrorAction SilentlyContinue
    Copy-Item "$Game\lords2hd.log" "$out\$name-$tag.log" -Force -ErrorAction SilentlyContinue
    $hashes = @(if (Test-Path $file) { Select-String $file -Pattern ' hash ([0-9a-f]{8})' | ForEach-Object { $_.Matches[0].Groups[1].Value } })
    $crash = @(if (Test-Path "$Game\lords2hd.log") { Select-String "$Game\lords2hd.log" -Pattern 'exception' | ForEach-Object Line })
    [pscustomobject]@{ hashes = $hashes; crash = $crash }
}
# The seconds two runs both logged (the last half second may be cut when the game is closed): $null if they agree,
# else the first second that differs.
function Compare-Hashes($a, $b) {
    $n = [math]::Min($a.Count, $b.Count)
    for ($i = 0; $i -lt $n; $i++) { if ($a[$i] -ne $b[$i]) { return $i + 1 } }
    return $null
}

$fail = 0
try {
    foreach ($b in $Battles) {
        if (-not $defs.ContainsKey($b)) { "integration: no battle '$b' (have: $($defs.Keys -join ', '))"; $fail++; continue }
        $r1 = Invoke-Battle $b $Dll 'new1'
        $r2 = Invoke-Battle $b $Dll 'new2'
        $line = "{0,-6} {1,3} s logged" -f $b, $r1.hashes.Count
        $ok = $true
        if ($r1.crash.Count -or $r2.crash.Count) { $line += " | CRASH: " + (@($r1.crash) + @($r2.crash) | Select-Object -First 1); $ok = $false }
        if ($r1.hashes.Count -lt [int]($Secs * 0.7)) { $line += " | too few seconds logged (did the battle start?)"; $ok = $false }
        $d = Compare-Hashes $r1.hashes $r2.hashes
        if ($null -eq $d) { $line += " | twice: same" } else { $line += " | twice: DIFFERENT from second $d"; $ok = $false }
        if ($RefDll) {
            $r0 = Invoke-Battle $b $RefDll 'ref'
            $d0 = Compare-Hashes $r0.hashes $r1.hashes
            if ($r0.crash.Count) { $line += " | reference crashed" }
            $line += if ($null -eq $d0) { " | vs reference: same" } else { " | vs reference: differs from second $d0" }
        }
        if (-not $ok) { $fail++ }
        $line
    }
}
finally {
    if (Test-Path "$keep\ddraw.dll") { Copy-IntoGame "$keep\ddraw.dll" "$Game\ddraw.dll" }
    if (Test-Path "$keep\lords2hd.ini") { Copy-IntoGame "$keep\lords2hd.ini" $ini }
}
"integration: $($Battles.Count) battles, $fail failed (logs in tests\out\integration)"
exit $fail
