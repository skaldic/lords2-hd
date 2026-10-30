# Builds 2x county terrain: every 58x30 diamond of the ground sets, upscaled inside a lattice of
# itself so the joins are seamless, then half blended with a plain enlargement to keep the grain.
#   <game>\lords2hd\tiles\<set>.bin = frames x 116x60 BGRX, one slot per frame (others left black)
param([string[]]$Sets = @('base1a','base1b','base1c','base1d','roads1a','roads1b','roads1c','roads1d','mtns1a','mtns1b','mtns1c','mtns1d'), [double]$Plain = 0.25, [switch]$Recut)
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
$esr = Join-Path $tools "realesrgan\realesrgan-ncnn-vulkan.exe"
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$work = Join-Path $root "assets\tiles"; $in = Join-Path $work "in"; $up = Join-Path $work "x4"
$out = Join-Path $game "lords2hd\tiles"
if (-not $Recut) { Remove-Item $in, $up -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $in, $up, $out | Out-Null

$lattice = Join-Path $work "Lattice.cs"
Set-Content $lattice -Encoding ascii -Value @'
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class Lattice {
    public const int W = 178, H = 90; // room for the middle tile at (60, 30) and its neighbours
    // Paints f in the map's lattice (60 across, 30 down, every other row 30 in), back to front.
    public static void Save(Pl8Frame f, Color[] pal, string path) {
        var px = new int[W * H];
        for (int row = -2; row <= 7; row++) {
            int ox = (row & 1) != 0 ? 30 : 0, oy = row * 15;
            for (int col = -1; col <= 3; col++) {
                int X = col * 60 + ox, Y = oy;
                for (int y = 0; y < f.Height; y++) for (int x = 0; x < f.Width; x++) {
                    int i = y * f.Width + x;
                    if (!f.Opaque[i]) continue;
                    int a = X + x, b = Y + y;
                    if (a < 0 || b < 0 || a >= W || b >= H) continue;
                    px[b * W + a] = pal[f.Pixels[i]].ToArgb();
                    if (x == f.Width - 1 || !f.Opaque[i + 1]) { // the 2 column gap between diamonds
                        for (int k = 1; k <= 2 && a + k < W; k++) if (px[b * W + a + k] == 0) px[b * W + a + k] = px[b * W + a];
                    }
                }
            }
        }
        for (int i = 0; i < px.Length; i++) if (px[i] == 0) px[i] = i > 0 ? px[i - 1] : unchecked((int)0xFF000000);
        using (var bmp = new Bitmap(W, H, PixelFormat.Format32bppArgb)) {
            var d = bmp.LockBits(new Rectangle(0, 0, W, H), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            Marshal.Copy(px, 0, d.Scan0, px.Length);
            bmp.UnlockBits(d);
            bmp.Save(path, ImageFormat.Png);
        }
    }
    // Cuts the middle tile from the 4x lattice, reduces to 116x60, blends in the plain enlargement.
    public static byte[] Cut(string upscaled, Pl8Frame f, Color[] pal, double plain) {
        using (var src = new Bitmap(upscaled))
        using (var hd = new Bitmap(116, 60, PixelFormat.Format32bppRgb))
        using (var g = Graphics.FromImage(hd)) {
            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
            g.DrawImage(src, new Rectangle(0, 0, 116, 60), new Rectangle(60 * 4, 30 * 4, 58 * 4, 30 * 4), GraphicsUnit.Pixel);
            var d = hd.LockBits(new Rectangle(0, 0, 116, 60), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
            var o = new byte[116 * 60 * 4];
            Marshal.Copy(d.Scan0, o, 0, o.Length);
            hd.UnlockBits(d);
            for (int y = 0; y < 60; y++) for (int x = 0; x < 116; x++) {
                int i = (y / 2) * f.Width + x / 2;
                if (!f.Opaque[i]) continue;
                Color c = pal[f.Pixels[i]];
                int p = (y * 116 + x) * 4;
                // Towards the diamond's edge, more of the original: its tiles were drawn to join.
                int r1 = y / 2, k = r1 < 15 ? r1 : 29 - r1, rw = 2 + 4 * k, x0 = (58 - rw) / 2;
                int edge = Math.Min(Math.Min(x / 2 - x0, x0 + rw - 1 - x / 2) / 2, k);
                double w = Math.Max(plain, 1.0 - edge / 4.0);
                o[p] = (byte)(o[p] * (1 - w) + c.B * w);
                o[p + 1] = (byte)(o[p + 1] * (1 - w) + c.G * w);
                o[p + 2] = (byte)(o[p + 2] * (1 - w) + c.R * w);
            }
            return o;
        }
    }
}
'@
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs"), $lattice -ReferencedAssemblies System.Drawing

$pal = [Pl8]::ReadPalette((Join-Path $game "Base01.256"))
$all = @{}
foreach ($set in $Sets) {
    $c = 0; $h = ""
    $f = [Pl8]::Read((Join-Path $game "$set.pl8"), [ref]$c, [ref]$h)
    $all[$set] = $f
    for ($i = 0; $i -lt $f.Length; $i++) {
        if (-not $Recut -and $f[$i].Width -eq 58 -and $f[$i].Height -eq 30 -and -not $f[$i].Error) { [Lattice]::Save($f[$i], $pal, (Join-Path $in ("{0}_{1:D3}.png" -f $set, $i))) }
    }
}
"{0} tiles to upscale" -f (Get-ChildItem $in).Count
if (-not (Get-ChildItem $in)) { throw "no tiles" }
$ErrorActionPreference = 'Continue'
if (-not $Recut) { & $esr -i $in -o $up -n realesrgan-x4plus -s 4 -f png -m (Join-Path $tools "realesrgan\models") 2>$null | Out-Null }
if ((Get-ChildItem $up).Count -ne (Get-ChildItem $in).Count) { throw "the upscaler did not finish" }
foreach ($set in $Sets) {
    $f = $all[$set]
    $s = [IO.File]::Create((Join-Path $out "$set.bin"))
    $blank = New-Object byte[] (116 * 60 * 4)
    for ($i = 0; $i -lt $f.Length; $i++) {
        $u = Join-Path $up ("{0}_{1:D3}.png" -f $set, $i)
        $b = if (Test-Path $u) { [Lattice]::Cut($u, $f[$i], $pal, $Plain) } else { $blank }
        $s.Write($b, 0, $b.Length)
    }
    $s.Close()
    $set
}
