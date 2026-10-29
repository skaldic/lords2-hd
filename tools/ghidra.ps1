# Runs Ghidra headless on the game exe.
#   ghidra.ps1 import                      -> new project + auto-analysis + functions.csv
#   ghidra.ps1 script <Name.java> [args]   -> run one of ghidra_scripts on the analysed program
# Ghidra and a JDK are found under the tools folder (see tools_dir.ps1).
param([Parameter(Position = 0)][string]$Mode = "import", [Parameter(ValueFromRemainingArguments = $true)][string[]]$Rest)
$root = Split-Path $PSScriptRoot -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
$ghidra = Get-ChildItem $tools -Directory -Filter "ghidra_*" | Select-Object -First 1
$jdk = Get-ChildItem $tools -Directory -Filter "jdk*" | Select-Object -First 1
if (-not $ghidra) { throw "Ghidra not found in $tools" }
if ($jdk) { $env:JAVA_HOME = $jdk.FullName }
$exe = Join-Path (Split-Path $root -Parent) "Lords2.exe"
$proj = Join-Path $root "ghidra_proj"
New-Item -ItemType Directory -Force $proj | Out-Null
$headless = Join-Path $ghidra.FullName "support\analyzeHeadless.bat"
$scripts = Join-Path $root "ghidra_scripts"
$log = Join-Path $root "ghidra.log"

if ($Mode -eq "import") {
    $a = @($proj, "lords2", "-import", $exe, "-scriptPath", $scripts,
        "-postScript", "ExportFunctions.java", (Join-Path $root "functions.csv"), "-log", $log)
} else {
    $a = @($proj, "lords2", "-process", "Lords2.exe", "-noanalysis", "-scriptPath", $scripts,
        "-postScript") + $Rest + @("-log", $log)
}
# The batch file pauses on errors; feed it an empty stdin so it never waits.
$empty = Join-Path $root "ghidra_stdin.txt"
Set-Content $empty "" -Encoding ascii
$p = Start-Process -FilePath $headless -ArgumentList ($a | ForEach-Object { '"' + $_ + '"' }) `
    -NoNewWindow -Wait -PassThru -RedirectStandardInput $empty `
    -RedirectStandardOutput (Join-Path $root "ghidra_out.txt") -RedirectStandardError (Join-Path $root "ghidra_err.txt")
Get-Content (Join-Path $root "ghidra_out.txt") | Select-String "ERROR|SCRIPT|Exported|Decompiled|Named" | Select-Object -Last 20
"exit $($p.ExitCode)"
