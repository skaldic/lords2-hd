# Lists the units inside the running game's battle view with the wide-screen game x,y of their
# cell, and what the game reports for the mouse: .\tools\battleprobe.ps1
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class BP {
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(int a, bool i, int p);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr a, byte[] b, int s, out int r);
    public static byte[] R(int pid, long a, int s) { var h = OpenProcess(0x10, false, pid); var b = new byte[s]; int n; ReadProcessMemory(h, (IntPtr)a, b, s, out n); return b; }
    public static int I(int pid, long a) { return BitConverter.ToInt32(R(pid, a, 4), 0); }
}
'@
$id = (Get-Process Lords2).Id
$col = [BP]::I($id, 0x55ce70); $row = [BP]::I($id, 0x5678a0); $x0 = [BP]::I($id, 0x4e6558); $n = [BP]::I($id, 0x4e5d5c)
"scroll $col,$row first x $x0, $n columns; mouse $([BP]::I($id,0x4e65fc)),$([BP]::I($id,0x4e65f4)) cell $([BP]::I($id,0x553f88)),$([BP]::I($id,0x553f94)) hover unit $([BP]::I($id,0x553f90))"
$a = [BP]::R($id, 0x554480, 0x1b0 * 81)
for ($u = 1; $u -le 80; $u++) {
    $o = $u * 0x1b0
    if (-not $a[$o + 0x2c]) { continue }
    $c = [BitConverter]::ToInt16($a, $o + 0x20); $r = [BitConverter]::ToInt16($a, $o + 0x22)
    if ($c -ge $col -and $c -lt $col + $n -and $r -ge $row -and $r -lt $row + 14) {
        "unit $u side $($a[$o + 0x2c]) cell $c,$r at game x $(($c - $col) * 32 + $x0 + 16), y $(($r - $row) * 32 + 24 + 16) selected $($a[$o + 0xa])"
    }
}
