# Finds raw references to code addresses in Lords2.exe, also where Ghidra made no function:
# E8 rel32 calls, E9 rel32 jumps and absolute 32-bit pointers.
#   .\tools\xrefs.ps1 004b5ee8 004b6034
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Targets)
$exe = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "Lords2.exe"
Add-Type -TypeDefinition @'
using System; using System.Collections.Generic;
public static class Xrefs {
    // .text: virtual address 0x401000, file offset 0x400, 0xCEC00 bytes.
    public static List<string> Find(byte[] b, uint[] targets) {
        var r = new List<string>();
        var set = new HashSet<uint>(targets);
        for (int i = 0x400; i + 5 <= 0x400 + 0xCEC00; i++) {
            if (b[i] != 0xE8 && b[i] != 0xE9) continue;
            uint va = (uint)(0x401000 + i - 0x400);
            uint t = (uint)(va + 5 + BitConverter.ToInt32(b, i + 1));
            if (set.Contains(t)) r.Add(string.Format("{0:x8} {1} {2:x8}", va, b[i] == 0xE8 ? "call" : "jmp ", t));
        }
        for (int i = 0; i + 4 <= b.Length; i++) {
            uint v = BitConverter.ToUInt32(b, i);
            if (set.Contains(v)) r.Add(string.Format("file {0:x6} pointer {1:x8}", i, v));
        }
        return r;
    }
}
'@
$t = [uint32[]]($Targets | ForEach-Object { [Convert]::ToUInt32($_, 16) })
[Xrefs]::Find([IO.File]::ReadAllBytes($exe), $t)
