# Computer battle AI scenarios: seeded custom battles played hidden and muted in a COPY of the game (never your own),
# each with [Debug] BattleLog=1, and a one-line summary per battle from the log's AI lines.
#   tests\ai_battles.ps1 -Game "D:\L2HD-test\Lords of the Realm II" [-Dll <ddraw.dll>] [-Maps tb,sq] [-Odds strong,even,weak]
#                        [-Roles cpuatt,cpudef] [-Players default,stand,charge] [-Secs 180] [-Tag x] [-Ini Game.BattleAI=1]
# Odds: the custom battle screen's advantage arrows, 4 clicks towards the computer (strong) or towards you (weak).
# Roles: cpuatt = the computer is the attacker (the screen's default: you defend), cpudef = you attack, it defends.
# Players (what your side does): default = the map's own orders (stand on Three Bridges, march on Squigglewood and
# Two Lakes), stand = nothing, charge = Mop-Up as the battle starts (every unit of yours goes for the nearest enemy).
# Battles cut by the clock end at different seconds under load: compare runs second by second over the seconds
# both have, not by their last hash.
# Maps: field tb (Three Bridges), isth (Isthmus), lakes (Two Lakes), guer (Guerrilla War), sq (Squigglewood),
# inland (Inland Sea); sieges palisade, acre (Castle Acre), keep (Norman Keep).
# Per battle:
#   flips     how often the computer switched between its attack and defence rules
#   charges   group thinks that started a charge, and that ended one
#   contact   the first second with any unit in melee
#   isolated  seconds with a computer archer unit that has enemy melee within 8 cells and none of its own within 5
#   melee     seconds with a computer archer unit in melee
#   shots     the computer's shots (yours in brackets)
#   men       men left at the end on each side, melee + ranged (start in brackets)
#   hash      the battle's last hash (two runs of the same battle and settings must give the same)
# The copy's ddraw.dll and lords2hd.ini are put back afterwards. Several copies can run at once. About 3.5 minutes a battle.
param(
    [string]$Game,
    [string]$Dll = "$PSScriptRoot\..\hd\ddraw.dll",
    [string[]]$Maps = @('tb', 'sq', 'lakes'),
    [string[]]$Odds = @('strong', 'even', 'weak'),
    [string[]]$Roles = @('cpuatt'),
    [string[]]$Players = @('default'),
    [int]$Secs = 180,
    [string]$Tag = 'run',
    [string[]]$Ini = @(),  # extra Section.Key=Value settings for the run, e.g. Game.BattleAI=1
    [int]$AtSec = 0,       # summarise each battle at this battle second (0 = its end)
    [string[]]$Logs = @()  # only summarise these saved battle logs (out\ai\*.txt), playing nothing
)
$ErrorActionPreference = 'Stop'
$out = "$PSScriptRoot\out\ai"
if (-not $Logs.Count) {   # (summarise-only mode needs no game)
if (-not $Game) { throw "ai_battles: give -Game (a copy of the game), or -Logs to summarise saved battle logs" }
$Game = (Resolve-Path $Game).Path.TrimEnd('\')
$yours = (Resolve-Path "$PSScriptRoot\..\..").Path.TrimEnd('\')
if ($Game -ieq $yours) { throw "ai_battles: $Game is your own game folder; give a copy of the game" }
if (-not (Test-Path "$Game\Lords2.exe")) { throw "ai_battles: no Lords2.exe in $Game" }
New-Item -ItemType Directory -Force $out | Out-Null
$copy = Split-Path (Split-Path $Game -Parent) -Leaf          # e.g. L2HD-test-ai1: the copy's own backup and DLL snapshot
$keep = "$out\keep-$copy"
New-Item -ItemType Directory -Force $keep | Out-Null
$snap = "$keep\run.dll"
Copy-Item (Resolve-Path $Dll).Path $snap -Force                 # a rebuild during the run doesn't change it
Add-Type -Namespace L2T -Name AiIni -MemberDefinition '[DllImport("kernel32.dll", CharSet = CharSet.Ansi)] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'
$iniPath = "$Game\lords2hd.ini"
Copy-Item "$Game\ddraw.dll" "$keep\ddraw.dll" -Force -ErrorAction SilentlyContinue
Copy-Item $iniPath "$keep\lords2hd.ini" -Force -ErrorAction SilentlyContinue
}

# maps on the custom battle screen's list (x 533) and the map's own orders for you (given while the battle is paused)
$mapDefs = @{
    tb       = @{ pre = @('click:533,192'); paused = @() }                     # Three Bridges, you stand still
    isth     = @{ pre = @('click:533,207'); paused = @() }                     # Isthmus
    lakes    = @{ pre = @('click:533,223'); paused = @('march:40,40') }        # Two Lakes, you march to the middle
    guer     = @{ pre = @('click:533,241'); paused = @() }                     # Guerrilla War
    sq       = @{ pre = @('click:533,256'); paused = @('march:40,64') }        # Squigglewood, you march round the forest
    inland   = @{ pre = @('click:533,272'); paused = @() }                     # Inland Sea
    palisade = @{ pre = @('click:480,306'); paused = @() }                     # sieges: the Siege switch, then the castle
    acre     = @{ pre = @('click:480,306', 'wait:2', 'click:533,209'); paused = @() }
    keep     = @{ pre = @('click:480,306', 'wait:2', 'click:533,257'); paused = @() }
}
$oddDefs = @{
    strong = @('click:291,414', 'wait:1', 'click:291,414', 'wait:1', 'click:291,414', 'wait:1', 'click:291,414', 'wait:1')
    even   = @()
    weak   = @('click:156,414', 'wait:1', 'click:156,414', 'wait:1', 'click:156,414', 'wait:1', 'click:156,414', 'wait:1')
}
$roleDefs = @{ cpuatt = @(); cpudef = @('click:91,280', 'wait:1') }    # your Defender button turns you into the attacker
$boot = @('wait:7', 'click:320,240', 'wait:1', 'click:320,240', 'wait:1', 'click:320,240', 'wait:3', 'click:320,102')

function Copy-IntoGame($from, $to) {
    Get-Process Lords2 -ErrorAction SilentlyContinue | Where-Object { $_.Path -ieq "$Game\Lords2.exe" } | ForEach-Object { [void]$_.WaitForExit(10000) }
    for ($i = 0; ; $i++) {
        try { Copy-Item $from $to -Force -ErrorAction Stop; return }
        catch { if ($i -ge 40) { throw }; Start-Sleep -Milliseconds 250 }
    }
}
function Get-Num($line, $pat) { if ($line -match $pat) { [int]$Matches[1] } else { 0 } }

# The summary of one battle's log
function Get-AiSummary($file) {
    $L = @(Get-Content $file)
    if ($AtSec -gt 0) {   # cut after that second's lines (hash, sec, cpu, cover)
        $sl = @($L | Select-String ' sec side1 ')
        if ($sl.Count -gt $AtSec) { $L = $L[0..([math]::Min($L.Count - 1, $sl[$AtSec - 1].LineNumber + 1))] }
    }
    $cov = @($L | Select-String 'cover side \d \(computer\)')
    $first = $cov | Select-Object -First 1; $last = $cov | Select-Object -Last 1
    $start = ($L | Select-String 'ai groups at the start' | Select-Object -First 1).Line
    $secs = @($L | Select-String ' sec side1 ')
    $contact = '-'
    for ($k = 0; $k -lt $secs.Count; $k++) { if ($secs[$k].Line -match ' melee [1-9]') { $contact = $k + 1; break } }
    $hash = ($L | Select-String ' hash ([0-9a-f]{8})' | Select-Object -Last 1)
    [pscustomobject]@{
        contact  = $contact
        hash     = if ($hash) { $hash.Matches[0].Groups[1].Value } else { '-' }
        shots    = "{0} ({1})" -f ($cov | ForEach-Object { Get-Num $_.Line '\| shots (\d+)' } | Measure-Object -Sum).Sum,
                       ($cov | ForEach-Object { Get-Num $_.Line 'enemy shots (\d+)' } | Measure-Object -Sum).Sum
        start    = if ($start -match 'strength ([+-]\d+)') { $Matches[1] } else { '?' }
        flips    = @($L | Select-String 'ai strength ').Count
        charges  = @($L | Select-String 'did: charge(?! ends)').Count
        ends     = @($L | Select-String 'charge ends').Count
        isolated = @($cov | Where-Object { $_.Line -match 'isolated [1-9]' }).Count
        maxIso   = ($cov | ForEach-Object { Get-Num $_.Line 'isolated (\d+)' } | Measure-Object -Maximum).Maximum
        inMelee  = @($cov | Where-Object { $_.Line -match 'archers in melee [1-9]' }).Count
        cpu      = if ($last) { "{0}+{1} ({2}+{3})" -f (Get-Num $last.Line 'men melee (\d+)'), (Get-Num $last.Line ' ranged (\d+), enemy'),
                       (Get-Num $first.Line 'men melee (\d+)'), (Get-Num $first.Line ' ranged (\d+), enemy') } else { '?' }
        you      = if ($last) { "{0}+{1} ({2}+{3})" -f (Get-Num $last.Line 'enemy melee (\d+)'), (Get-Num $last.Line 'enemy melee \d+ ranged (\d+)'),
                       (Get-Num $first.Line 'enemy melee (\d+)'), (Get-Num $first.Line 'enemy melee \d+ ranged (\d+)') } else { '?' }
    }
}

function Format-AiLine($label, $s, $crash) {
    "{0} start {1,4} | flips {2,3} | charges {3,3} ends {4,3} | contact {5,3} s | isolated {6,3} s (max {7}) | archers in melee {8,3} s | shots {9} | computer men {10} | yours {11} | hash {12}{13}" -f
        $label, $s.start, $s.flips, $s.charges, $s.ends, $s.contact, $s.isolated, $s.maxIso, $s.inMelee, $s.shots, $s.cpu, $s.you, $s.hash,
        $(if ($crash) { ' | EXCEPTION in the log' } else { '' })
}

if ($Logs.Count) {
    foreach ($f in $Logs) {
        $p = if (Test-Path $f) { $f } else { "$out\$f" }
        $crash = (Test-Path ($p -replace '\.txt$', '.log')) -and (Select-String -Path ($p -replace '\.txt$', '.log') -Pattern 'exception|crash' -Quiet)
        Format-AiLine ("{0,-44}" -f (Split-Path $p -Leaf)) (Get-AiSummary $p) $crash
    }
    return
}

try {
    foreach ($m in $Maps) { foreach ($r in $Roles) { foreach ($p in $Players) { foreach ($o in $Odds) {
        Copy-IntoGame $snap "$Game\ddraw.dll"
        Copy-IntoGame "$keep\lords2hd.ini" $iniPath
        $set = @('Debug.Remote=1', 'Debug.RemoteVisible=0', 'Debug.BattleSeed=1', 'Debug.BattleLog=1', 'Debug.Timing=0', 'Debug.MpLogs=0') + $Ini
        foreach ($s in $set) { $k, $v = $s -split '=', 2; $sec, $key = $k -split '\.', 2; [void][L2T.AiIni]::WritePrivateProfileString($sec, $key, $v, $iniPath) }
        Remove-Item "$Game\lords2hd-battle.txt", "$Game\lords2hd.log" -ErrorAction SilentlyContinue
        $d = $mapDefs[$m]
        # Mop-Up while the battle is still paused, so it starts at the same step every run (you are side 1 in both roles)
        $orders = switch ($p) { 'default' { $d.paused } 'stand' { @() } 'charge' { @('mopup:1') } }
        $after = @()
        $steps = $boot + @('wait:4', 'click:320,174', 'wait:6', 'click:431,226', 'wait:10') + $d.pre + @('wait:2') + $roleDefs[$r] + $oddDefs[$o] +
            @('wait:2', 'click:605,446', 'wait:8') + $orders + @('wait:1', 'click:496,460') + $after + @("wait:$Secs")
        & "$PSScriptRoot\..\hd\bgdrive.ps1" -Game $Game -Start -Stop @steps | Out-Null
        $name = "$m-$r-$p-$o-$Tag"
        $file = "$out\$name.txt"
        Copy-Item "$Game\lords2hd-battle.txt" $file -Force -ErrorAction SilentlyContinue
        Copy-Item "$Game\lords2hd.log" "$out\$name.log" -Force -ErrorAction SilentlyContinue
        if (-not (Test-Path $file)) { "{0,-8} {1,-6} {2,-7} {3,-6} no battle log (did the battle start?)" -f $m, $r, $p, $o; continue }
        $crash = Select-String -Path "$out\$name.log" -Pattern 'exception|crash' -Quiet
        $s = Get-AiSummary $file
        Format-AiLine ("{0,-8} {1,-6} {2,-7} {3,-6}" -f $m, $r, $p, $o) $s $crash
    } } } }
}
finally {
    if (Test-Path "$keep\ddraw.dll") { Copy-IntoGame "$keep\ddraw.dll" "$Game\ddraw.dll" }
    if (Test-Path "$keep\lords2hd.ini") { Copy-IntoGame "$keep\lords2hd.ini" $iniPath }
}
