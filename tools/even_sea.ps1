# Evens out the open-sea tile variants so the joins between them don't show: each variant's broad
# shading (a wide blur inside the diamond) is replaced by the average broad shading of all of
# them; the fine wave texture stays. Run after match_tile_tones.ps1.
#   .\tools\even_sea.ps1 [-Sets base1a,...] [-Frames 22..37] [-Out <folder>] [-Preview <png>]
# Writes in place (after a copy to tiles\before_sea\) unless -Out is given.
param(
    [string[]]$Sets = @('base1a', 'base1b', 'base1c', 'base1d'),
    [int[]]$Frames = (22..37),
    [string]$Out = "",
    [string]$Preview = "",
    [int]$Radius = 10
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$tiles = Join-Path (Split-Path $root -Parent) "lords2hd\tiles"
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging;
public static class Sea {
    const int W = 116, H = 60;
    static bool In(int x, int y) { int k = y < H / 2 ? y / 2 : (H - 1 - y) / 2; int w = 2 * (2 + 4 * k), x0 = (W - w) / 2; return x >= x0 && x < x0 + w; }
    // box blur of one channel inside the diamond (normalised by the pixels inside)
    static double[] Blur(byte[] b, int f, int c, int r) {
        var o = new double[W * H]; int off = f * W * H * 4;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            if (!In(x, y)) continue; double s = 0; int n = 0;
            for (int dy = -r / 2; dy <= r / 2; dy++) for (int dx = -r; dx <= r; dx++) { // tiles are 2:1, so the blur is too
                int X = x + dx, Y = y + dy; if (X < 0 || Y < 0 || X >= W || Y >= H || !In(X, Y)) continue;
                s += b[off + (Y * W + X) * 4 + c]; n++; }
            o[y * W + x] = s / n; }
        return o;
    }
    public static void Even(byte[] b, int[] frames, int r) {
        int n = frames.Length; var blur = new double[n][][];
        for (int i = 0; i < n; i++) { blur[i] = new double[3][]; for (int c = 0; c < 3; c++) blur[i][c] = Blur(b, frames[i], c, r); }
        for (int c = 0; c < 3; c++) {
            var mean = new double[W * H];
            for (int i = 0; i < n; i++) for (int p = 0; p < W * H; p++) mean[p] += blur[i][c][p] / n;
            for (int i = 0; i < n; i++) { int off = frames[i] * W * H * 4;
                for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { if (!In(x, y)) continue; int p = y * W + x;
                    int v = (int)Math.Round(b[off + p * 4 + c] - blur[i][c][p] + mean[p]); b[off + p * 4 + c] = (byte)(v < 0 ? 0 : v > 255 ? 255 : v); } }
        }
    }
    public static Bitmap Mosaic(byte[] b, int[] frames, int cols, int rows, int seed) {
        var bm = new Bitmap(cols * W, rows * H / 2 + H / 2, PixelFormat.Format32bppRgb); var rnd = new Random(seed);
        var d = bm.LockBits(new Rectangle(0, 0, bm.Width, bm.Height), ImageLockMode.WriteOnly, PixelFormat.Format32bppRgb);
        int[] px = new int[bm.Width * bm.Height];
        for (int row = 0; row < rows; row++) for (int col = -1; col < cols; col++) {
            int f = frames[rnd.Next(frames.Length)], ox = col * W + (row % 2) * W / 2, oy = row * H / 2;
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { if (!In(x, y)) continue; int X = ox + x, Y = oy + y;
                if (X < 0 || Y < 0 || X >= bm.Width || Y >= bm.Height) continue; int i = (f * W * H + y * W + x) * 4;
                px[Y * bm.Width + X] = b[i] | b[i + 1] << 8 | b[i + 2] << 16; } }
        System.Runtime.InteropServices.Marshal.Copy(px, 0, d.Scan0, px.Length); bm.UnlockBits(d); return bm;
    }
}
'@
$shots = @()
foreach ($set in $Sets) {
    $src = Join-Path $tiles "$set.bin"
    $b = [IO.File]::ReadAllBytes($src)
    $before = [byte[]]$b.Clone()
    [Sea]::Even($b, $Frames, $Radius)
    if ($Preview) { $shots += ,@([Sea]::Mosaic($before, $Frames, 8, 10, 3), [Sea]::Mosaic($b, $Frames, 8, 10, 3)) }
    $dst = if ($Out) { New-Item -ItemType Directory -Force $Out | Out-Null; Join-Path $Out "$set.bin" } else {
        $bk = Join-Path $tiles "before_sea"; New-Item -ItemType Directory -Force $bk | Out-Null
        if (-not (Test-Path (Join-Path $bk "$set.bin"))) { Copy-Item $src $bk }; $src }
    [IO.File]::WriteAllBytes($dst, $b)
    "${set}: $($Frames.Count) sea frames evened -> $dst"
}
if ($Preview) {
    $w = $shots[0][0].Width; $h = $shots[0][0].Height
    $o = New-Object Drawing.Bitmap (2 * $w + 8), ($shots.Count * ($h + 8))
    $g = [Drawing.Graphics]::FromImage($o); $g.Clear([Drawing.Color]::Black)
    for ($i = 0; $i -lt $shots.Count; $i++) { $g.DrawImage($shots[$i][0], 0, $i * ($h + 8)); $g.DrawImage($shots[$i][1], $w + 8, $i * ($h + 8)) }
    $g.Dispose(); $o.Save($Preview); $o.Dispose()
    "preview: $Preview (left as now, right evened; one row per season)"
}
