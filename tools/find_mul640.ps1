# Finds "reg * 640" compiled as LEA reg,[reg+reg*4] ; SHL reg,7 in Lords2.exe: the pitch hidden
# from an immediate search. Prints the address and register of each.
$exe = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) "Lords2.exe"
Add-Type -TypeDefinition @'
using System; using System.Collections.Generic;
public static class Mul640 {
    static readonly string[] Regs = { "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI" };
    public static List<string> Find(byte[] b) {
        var r = new List<string>();
        for (int i = 0x400; i + 6 <= 0x400 + 0xCEC00; i++) {
            if (b[i] != 0x8D) continue;
            int modrm = b[i + 1], sib = b[i + 2];
            if ((modrm & 0xC7) != 0x04) continue;            // LEA reg,[SIB]
            int reg = (modrm >> 3) & 7;
            if ((sib >> 6) != 2 || ((sib >> 3) & 7) != reg || (sib & 7) != reg || reg == 5) continue; // [reg+reg*4]
            if (b[i + 3] != 0xC1 || b[i + 4] != (0xE0 | reg) || b[i + 5] != 7) continue; // SHL reg,7
            r.Add(string.Format("{0:x8} {1}", 0x401000 + i - 0x400, Regs[reg]));
        }
        return r;
    }
}
'@
[Mul640]::Find([IO.File]::ReadAllBytes($exe))
