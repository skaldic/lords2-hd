# Prints decompiled functions from src\ by address: .\tools\fn.ps1 00452306 004523e6 [-Lines 60]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Addresses, [int]$Lines = 60)
$src = Join-Path (Split-Path $PSScriptRoot -Parent) "src"
foreach ($a in $Addresses) {
    $a = $a.ToLower().PadLeft(8, '0')
    $hit = Select-String -Path "$src\*.c" -Pattern "^// $a  " | Select-Object -First 1
    if (-not $hit) { "===== $a not found"; continue }
    "===== $a ($($hit.Filename):$($hit.LineNumber))"
    $text = Get-Content $hit.Path
    $end = [Math]::Min($text.Count, $hit.LineNumber - 1 + $Lines)
    for ($i = $hit.LineNumber - 1; $i -lt $end; $i++) {
        if ($i -gt $hit.LineNumber - 1 -and $text[$i] -match '^// [0-9a-f]{8}  ') { break }
        $text[$i]
    }
}
