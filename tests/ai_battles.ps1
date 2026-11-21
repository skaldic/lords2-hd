# Computer battle AI scenarios: seeded custom battles played hidden and muted in a COPY of the game (never your own),
# each with [Debug] BattleLog=1, and a one-line summary per battle from the log's AI lines.
#   tests\ai_battles.ps1 -Game "D:\L2HD-test\Lords of the Realm II" [-Dll <ddraw.dll>] [-Maps tb,sq] [-Odds strong,even,weak] [-Secs 120] [-Tag x]
# Odds: the custom battle screen's advantage arrows, 4 clicks towards the computer (strong) or towards you (weak).
# Per battle:
#   flips     how often the computer switched between its attack and defence rules
#   charges   group thinks that started a charge, and that ended one
#   isolated  seconds with a computer archer unit that has enemy melee within 8 cells and none of its own within 5
#   melee     seconds with a computer archer unit in melee
#   men       men left at the end on each side, melee + ranged (start in brackets)
# The copy's ddraw.dll and lords2hd.ini are put back afterwards. About 2.5 minutes a battle.
param(
    [Parameter(Mandatory = $true)][string]$Game,
    [string]$Dll = "$PSScriptRoot\..\hd\ddraw.dll",
    [string[]]$Maps = @('tb', 'sq', 'lakes'),
    [string[]]$Odds = @('strong', 'even', 'weak'),
    [int]$Secs = 120,
    [string]$Tag = 'run',
    [string[]]$Ini = @()   # extra Section.Key=Value settings for the run, e.g. Game.BattleAI=1
)
$ErrorActionPreference = 'Stop'
$Game = (Resolve-Path $Game).Path.TrimEnd('\')
$yours = (Resolve-Path "$PSScriptRoot\..\..").Path.TrimEnd('\')
if ($Game -ieq $yours) { throw "ai_battles: $Game is your own game folder; give a copy of the game" }
if (-not (Test-Path "$Game\Lords2.exe")) { throw "ai_battles: no Lords2.exe in $Game" }
$Dll = (Resolve-Path $Dll).Path
$out = "$PSScriptRoot\out\ai"
New-Item -ItemType Directory -Force $out | Out-Null
Add-Type -Namespace L2T -Name AiIni -MemberDefinition '[DllImport("kernel32.dll", CharSet = CharSet.Ansi)] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);'
$iniPath = "$Game\lords2hd.ini"
$keep = "$out\keep"
New-Item -ItemType Directory -Force $keep | Out-Null
Copy-Item "$Game\ddraw.dll" "$keep\ddraw.dll" -Force -ErrorAction SilentlyContinue
Copy-Item $iniPath "$keep\lords2hd.ini" -Force -ErrorAction SilentlyContinue

# maps on the custom battle screen's list (x 533) and what you do in each: orders go in while the battle is paused
$mapDefs = @{
    tb    = @{ pre = @('click:533,192'); paused = @() }                     # Three Bridges, you stand still
    sq    = @{ pre = @('click:533,256'); paused = @('march:40,64') }        # Squigglewood, you march round the forest
    lakes = @{ pre = @('click:533,223'); paused = @('march:40,40') }        # Two Lakes, you march to the middle
    isth  = @{ pre = @('click:533,207'); paused = @() }                     # Isthmus, you stand still
}
$oddDefs = @{
    strong = @('click:291,414', 'wait:1', 'click:291,414', 'wait:1', 'click:291,414', 'wait:1', 'click:291,414', 'wait:1')
    even   = @()
    weak   = @('click:156,414', 'wait:1', 'click:156,414', 'wait:1', 'click:156,414', 'wait:1', 'click:156,414', 'wait:1')
}
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
    $L = Get-Content $file
    $cov = @($L | Select-String 'cover side \d \(computer\)')
    $first = $cov | Select-Object -First 1; $last = $cov | Select-Object -Last 1
    $start = ($L | Select-String 'ai groups at the start' | Select-Object -First 1).Line
    [pscustomobject]@{
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

try {
    foreach ($m in $Maps) {
        foreach ($o in $Odds) {
            Copy-IntoGame $Dll "$Game\ddraw.dll"
            $set = @('Debug.Remote=1', 'Debug.RemoteVisible=0', 'Debug.BattleSeed=1', 'Debug.BattleLog=1', 'Debug.Timing=0', 'Debug.MpLogs=0') + $Ini
            foreach ($s in $set) { $k, $v = $s -split '=', 2; $sec, $key = $k -split '\.', 2; [void][L2T.AiIni]::WritePrivateProfileString($sec, $key, $v, $iniPath) }
            Remove-Item "$Game\lords2hd-battle.txt", "$Game\lords2hd.log" -ErrorAction SilentlyContinue
            $d = $mapDefs[$m]
            $steps = $boot + @('wait:4', 'click:320,174', 'wait:6', 'click:431,226', 'wait:10') + $oddDefs[$o] + $d.pre +
                @('wait:2', 'click:605,446', 'wait:8') + $d.paused + @('wait:1', 'click:496,460', "wait:$Secs")
            & "$PSScriptRoot\..\hd\bgdrive.ps1" -Game $Game -Start -Stop @steps | Out-Null
            $file = "$out\$m-$o-$Tag.txt"
            Copy-Item "$Game\lords2hd-battle.txt" $file -Force -ErrorAction SilentlyContinue
            Copy-Item "$Game\lords2hd.log" "$out\$m-$o-$Tag.log" -Force -ErrorAction SilentlyContinue
            if (-not (Test-Path $file)) { "{0,-6} {1,-6} no battle log (did the battle start?)" -f $m, $o; continue }
            $s = Get-AiSummary $file
            "{0,-6} {1,-6} start {2,4} | flips {3,3} | charges {4,3} ends {5,3} | isolated {6,3} s (max {7}) | archers in melee {8,3} s | computer men {9} | yours {10}" -f
                $m, $o, $s.start, $s.flips, $s.charges, $s.ends, $s.isolated, $s.maxIso, $s.inMelee, $s.cpu, $s.you
        }
    }
}
finally {
    if (Test-Path "$keep\ddraw.dll") { Copy-IntoGame "$keep\ddraw.dll" "$Game\ddraw.dll" }
    if (Test-Path "$keep\lords2hd.ini") { Copy-IntoGame "$keep\lords2hd.ini" $iniPath }
}
