# Evens out the HD map tiles' colour against the originals. The upscaled tiles came out darker than
# the game's own (6 levels on average, up to 17) and unevenly so: on calm sea the darker variants of
# the water tile show as darker diamonds. Each HD frame gets, per channel, the offset that gives it
# the original frame's average colour over the tile's diamond; its texture is kept.
#   .\tools\match_tile_tones.ps1 [-Sets base1a,base1b,...] [-Out <folder>] [-Preview <png>]
# Reads <game>\lords2hd\tiles\<set>.bin and the originals assets\tiles\in\<set>_NNN.png (the 3x3
# context pictures build_tiles.ps1 upscales); writes <Out>\<set>.bin (default: in place, after a
# copy to tiles\before_tone\).
param(
    [string[]]$Sets = @('base1a', 'base1b', 'base1c', 'base1d'),
    [string]$Out = "",
    [string]$Preview = ""
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tiles = Join-Path $game "lords2hd\tiles"
$in = Join-Path $root "assets\tiles\in"
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.IO; using System.Drawing; using System.Drawing.Imaging;
public static class Tone {
    const int W2 = 116, H2 = 60;
    static bool Inside(int x, int y) { int k = y < H2 / 2 ? y / 2 : (H2 - 1 - y) / 2; int w = 2 * (2 + 4 * k), x0 = (W2 - w) / 2; return x >= x0 && x < x0 + w; }
    public static double[] HdMean(byte[] b, int f) {
        int off = f * W2 * H2 * 4; double[] m = new double[3]; int n = 0;
        for (int y = 0; y < H2; y++) for (int x = 0; x < W2; x++) { if (!Inside(x, y)) continue; int i = off + (y * W2 + x) * 4; m[0] += b[i]; m[1] += b[i + 1]; m[2] += b[i + 2]; n++; }
        for (int c = 0; c < 3; c++) m[c] /= n; return m; // B, G, R
    }
    public static double[] LoMean(string p) {
        using (var bm = new Bitmap(p)) {
            int cx = bm.Width / 2, cy = bm.Height / 2; double[] m = new double[3]; int n = 0;
            for (int y = 0; y < 30; y++) { int k = y < 15 ? y : 29 - y; int w = 2 + 4 * k;
                for (int x = -w / 2; x < w / 2; x++) { var c = bm.GetPixel(cx + x, cy - 15 + y); m[0] += c.B; m[1] += c.G; m[2] += c.R; n++; } }
            for (int c = 0; c < 3; c++) m[c] /= n; return m;
        }
    }
    public static void Shift(byte[] b, int f, double[] d) {
        int off = f * W2 * H2 * 4;
        for (int i = off; i < off + W2 * H2 * 4; i += 4) for (int c = 0; c < 3; c++) { int v = (int)Math.Round(b[i + c] + d[c]); b[i + c] = (byte)(v < 0 ? 0 : v > 255 ? 255 : v); }
    }
    // frames f0.. laid out as isometric sea, rows of n; for a before/after look
    public static Bitmap Mosaic(byte[] b, int[] frames, int cols, int rows, int seed) {
        var bm = new Bitmap(cols * W2, rows * H2 / 2 + H2 / 2, PixelFormat.Format32bppRgb);
        var r = new Random(seed);
        var d = bm.LockBits(new Rectangle(0, 0, bm.Width, bm.Height), ImageLockMode.WriteOnly, PixelFormat.Format32bppRgb);
        int[] px = new int[bm.Width * bm.Height];
        for (int row = 0; row < rows; row++) for (int col = -1; col < cols; col++) {
            int f = frames[r.Next(frames.Length)], ox = col * W2 + (row % 2) * W2 / 2, oy = row * H2 / 2;
            for (int y = 0; y < H2; y++) for (int x = 0; x < W2; x++) {
                if (!Inside(x, y)) continue; int X = ox + x, Y = oy + y; if (X < 0 || Y < 0 || X >= bm.Width || Y >= bm.Height) continue;
                int i = (f * W2 * H2 + y * W2 + x) * 4; px[Y * bm.Width + X] = b[i] | b[i + 1] << 8 | b[i + 2] << 16;
            }
        }
        System.Runtime.InteropServices.Marshal.Copy(px, 0, d.Scan0, px.Length); bm.UnlockBits(d); return bm;
    }
}
'@
foreach ($set in $Sets) {
    $src = Join-Path $tiles "$set.bin"
    $b = [IO.File]::ReadAllBytes($src)
    $before = if ($Preview) { [byte[]]$b.Clone() } else { $null }
    $frames = $b.Length / (116 * 60 * 4)
    $changed = 0; $sea = @()
    for ($f = 0; $f -lt $frames; $f++) {
        $png = Join-Path $in ("{0}_{1:000}.png" -f $set, $f)
        if (-not (Test-Path $png)) { continue }
        $lo = [Tone]::LoMean($png); $hd = [Tone]::HdMean($b, $f)
        $d = @(($lo[0] - $hd[0]), ($lo[1] - $hd[1]), ($lo[2] - $hd[2]))
        if ($lo[0] -gt $lo[2] + 40 -and $f -ge 22 -and $f -le 37) { $sea += $f }   # open sea (blue in this season)
        if (($d | ForEach-Object { [Math]::Abs($_) } | Measure-Object -Maximum).Maximum -lt 1.5) { continue }
        [Tone]::Shift($b, $f, [double[]]$d); $changed++
    }
    if ($Preview -and $sea.Count) {
        $a = [Tone]::Mosaic($before, $sea, 9, 14, 7); $z = [Tone]::Mosaic($b, $sea, 9, 14, 7)
        $o = New-Object Drawing.Bitmap ($a.Width * 2 + 8), $a.Height
        $g = [Drawing.Graphics]::FromImage($o); $g.Clear([Drawing.Color]::Black); $g.DrawImage($a, 0, 0); $g.DrawImage($z, $a.Width + 8, 0); $g.Dispose()
        $o.Save($Preview); $o.Dispose(); $a.Dispose(); $z.Dispose()
        "preview: $Preview (left as shipped, right matched; open-sea frames $($sea -join ','))"
    }
    $dst = if ($Out) { New-Item -ItemType Directory -Force $Out | Out-Null; Join-Path $Out "$set.bin" } else {
        $bk = Join-Path $tiles "before_tone"; New-Item -ItemType Directory -Force $bk | Out-Null
        if (-not (Test-Path (Join-Path $bk "$set.bin"))) { Copy-Item $src $bk }; $src }
    [IO.File]::WriteAllBytes($dst, $b)
    "{0}: {1} of {2} frames evened out -> {3}" -f $set, $changed, $frames, $dst
}
