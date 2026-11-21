# The multiplayer layer's own self-test (multiplayer\dplayx\test\selftest.c), built and run natively with the patch's
# compiler (llvm-mingw) instead of Docker and Wine: dplayx.dll driven the way Lords2.exe drives it, host and joiners on
# 127.0.0.1 (no firewall prompt), 34 cases, about 3 minutes. Exit code 0 when all pass.
#   tests\mp_selftest.ps1
$ErrorActionPreference = 'Stop'
$c = & "$PSScriptRoot\..\tools\find_cc.ps1"
$cc = $c.cc; $target = $c.args
$src = "$PSScriptRoot\..\multiplayer\dplayx"
$out = "$PSScriptRoot\out\mp"
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $src
try {
    & $cc @target -O2 -Wall -fstack-protector-strong -shared -o "$out\dplayx.dll" dplayx.c dplayx.def -lws2_32 -static-libgcc -lssp "-Wl,--kill-at,--dynamicbase,--nxcompat"
    if ($LASTEXITCODE -ne 0) { throw "mp self-test: building dplayx.dll failed" }
    & $cc @target -O2 -Wall -Wno-format-overflow -o "$out\selftest.exe" test\selftest.c -lws2_32 -static-libgcc
    if ($LASTEXITCODE -ne 0) { throw "mp self-test: building selftest.exe failed" }
}
finally { Pop-Location }
# The protocol version the host expects of a joiner of the same build: DLL_VERSION * 1000 (no game exe here)
$ver = [regex]::Match((Get-Content "$src\dplayx.c" -Raw), '#define DLL_VERSION (\d+)').Groups[1].Value
$p = Start-Process -FilePath "$out\selftest.exe" -ArgumentList ([int]$ver * 1000) -WorkingDirectory $out -NoNewWindow -PassThru `
    -RedirectStandardOutput "$out\selftest.txt" -RedirectStandardError "$out\selftest-err.txt"
if (-not $p.WaitForExit(600000)) { $p.Kill(); "mp self-test: timed out after 10 minutes"; exit 1 }
$lines = Get-Content "$out\selftest.txt"
$lines | Where-Object { $_ -match '^FAIL' }
$sum = $lines | Where-Object { $_ -match 'ALL PASS|failed\)' } | Select-Object -Last 1
"mp self-test (v$ver): $(@($lines | Where-Object { $_ -match '^PASS' }).Count) passed; $sum (log: tests\out\mp\selftest.txt, dplayx.log)"
if ($sum -match 'ALL PASS') { exit 0 } else { exit 1 }
