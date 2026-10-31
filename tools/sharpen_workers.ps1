# The workers on the map are the overhang of animated town tiles, a few pixels wide: the AI upscaler
# turns them into soft blobs that differ from frame to frame. Run after build_overhang.ps1: it
# redraws those frames in lords2hd\sprites\town1?.bin by the Scale2x rule from their own pixels.
param([string[]]$Sets = @('town1a','town1b','town1c','town1d'))
$ErrorActionPreference = 'Stop'
$game = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Add-Type -TypeDefinition @"
using System; using System.IO;
public static class Scale2x {
    // Frames FUN_004071a0 cycles through: 10-18, 20-28, 30-45.
    static bool Animated(int i) { return (i >= 10 && i <= 18) || (i >= 20 && i <= 28) || (i >= 30 && i <= 45); }
    public static int Patch(string bin, string palette) {
        byte[] d = File.ReadAllBytes(bin), pal = File.ReadAllBytes(palette);
        int n = BitConverter.ToInt32(d, 0), done = 0;
        for (int f = 0; f < n; f++) {
            int off = BitConverter.ToInt32(d, 4 + 4 * f);
            if (off == 0 || !Animated(f)) continue;
            int w = BitConverter.ToUInt16(d, off), h = BitConverter.ToUInt16(d, off + 2), idx = off + 4, hd = idx + w * h;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                byte E = d[idx + y * w + x];
                if (E == 0) continue;
                byte B = y > 0 ? d[idx + (y - 1) * w + x] : (byte)0, H = y < h - 1 ? d[idx + (y + 1) * w + x] : (byte)0;
                byte D = x > 0 ? d[idx + y * w + x - 1] : (byte)0, F = x < w - 1 ? d[idx + y * w + x + 1] : (byte)0;
                byte[] e = { E, E, E, E };
                if (B != H && D != F) {
                    if (D == B && D != 0) e[0] = D;
                    if (B == F && F != 0) e[1] = F;
                    if (D == H && D != 0) e[2] = D;
                    if (H == F && F != 0) e[3] = F;
                }
                for (int k = 0; k < 4; k++) {
                    int o = hd + ((2 * y + k / 2) * 2 * w + 2 * x + k % 2) * 4;
                    d[o] = (byte)(pal[e[k] * 3 + 2] * 255 / 63); d[o + 1] = (byte)(pal[e[k] * 3 + 1] * 255 / 63); d[o + 2] = (byte)(pal[e[k] * 3] * 255 / 63);
                }
            }
            done++;
        }
        File.WriteAllBytes(bin, d);
        return done;
    }
}
"@
foreach ($s in $Sets) { "{0}: {1} frames" -f $s, [Scale2x]::Patch((Join-Path $game "lords2hd\sprites\$s.bin"), (Join-Path $game "Base01.256")) }
