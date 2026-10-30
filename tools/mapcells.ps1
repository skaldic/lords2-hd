# Reads the county map's cell grid (65 columns x 129 rows of 32-bit values at 0x0055cea0) from the
# running game and prints, per column, how many cells are "special" (>= 0x0fff0000) and how many of
# those draw one of the black terrain frames 0..5.
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class Mem {
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, int size, out int read);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    public static byte[] Read(int pid, long addr, int size) {
        IntPtr h = OpenProcess(0x10, false, pid);
        var b = new byte[size]; int n;
        ReadProcessMemory(h, (IntPtr)addr, b, size, out n);
        CloseHandle(h);
        return b;
    }
}
'@
$p = Get-Process Lords2 -ErrorAction Stop | Select-Object -First 1
$b = [Mem]::Read($p.Id, 0x0055cea0, 0x104 * 0x81)
$special = New-Object int[] 65; $black = New-Object int[] 65
for ($r = 0; $r -lt 0x81; $r++) {
    for ($c = 0; $c -lt 65; $c++) {
        $v = [BitConverter]::ToUInt32($b, $r * 0x104 + $c * 4)
        if ($v -ge 0x0fff0000) { $special[$c]++; if (($v - 0x0fff0000) -lt 6) { $black[$c]++ } }
    }
}
for ($c = 0; $c -lt 65; $c++) { "{0,2}: special {1,3}  black {2,3}" -f $c, $special[$c], $black[$c] }
