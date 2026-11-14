# Spike: non-AI upscalers on the game's own 1x frame, side by side with what the patch does now.
#   .\tools\scaler_spike.ps1 -In1x shots\sp_1x.png -InHD shots\sp_hd2.png -Out shots\scalers.png
# Panels (all at the same scale, a crop of the frame): nearest, current (bilinear 2x + smooth
# stretch), Catmull-Rom, FSR-style (Lanczos-2 + RCAS contrast-adaptive sharpening, an
# approximation of AMD FSR 1's EASU + RCAS), and the AI art.
param([string]$In1x, [string]$InHD, [string]$Out, [int]$X = 40, [int]$Y = 30, [int]$Size = 106, [int]$Scale = 6)
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Drawing; using System.Drawing.Imaging;
public static class Sc {
    public static float[][] Load(Bitmap b, out int w, out int h) {
        w = b.Width; h = b.Height; var c = new float[3][]; for (int k = 0; k < 3; k++) c[k] = new float[w * h];
        var d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
        int[] px = new int[w * h]; System.Runtime.InteropServices.Marshal.Copy(d.Scan0, px, 0, px.Length); b.UnlockBits(d);
        for (int i = 0; i < px.Length; i++) { c[0][i] = (px[i] >> 16) & 255; c[1][i] = (px[i] >> 8) & 255; c[2][i] = px[i] & 255; }
        return c;
    }
    public static Bitmap Save(float[][] c, int w, int h) {
        var b = new Bitmap(w, h, PixelFormat.Format32bppRgb); int[] px = new int[w * h];
        for (int i = 0; i < px.Length; i++) { int r = Cl(c[0][i]), g = Cl(c[1][i]), bl = Cl(c[2][i]); px[i] = (r << 16) | (g << 8) | bl; }
        var d = b.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppRgb);
        System.Runtime.InteropServices.Marshal.Copy(px, 0, d.Scan0, px.Length); b.UnlockBits(d); return b;
    }
    static int Cl(float v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f); }
    static float Px(float[] c, int w, int h, int x, int y) { x = x < 0 ? 0 : x >= w ? w - 1 : x; y = y < 0 ? 0 : y >= h ? h - 1 : y; return c[y * w + x]; }
    static double CR(double t) { t = Math.Abs(t); return t < 1 ? 1.5 * t * t * t - 2.5 * t * t + 1 : t < 2 ? -0.5 * t * t * t + 2.5 * t * t - 4 * t + 2 : 0; }
    static double L2(double t) { t = Math.Abs(t); if (t < 1e-6) return 1; if (t >= 2) return 0; double p = Math.PI * t; return 2 * Math.Sin(p) * Math.Sin(p / 2) / (p * p); }
    // separable resampler; kernel 0 = Catmull-Rom, 1 = Lanczos-2, 2 = bilinear, 3 = nearest. Source crop x0,y0,cw,ch; s = scale
    public static float[][] Resize(float[][] src, int w, int h, int x0, int y0, int cw, int ch, double s, int kernel) {
        int ow = (int)(cw * s), oh = (int)(ch * s); var o = new float[3][];
        for (int k = 0; k < 3; k++) {
            var tmp = new float[ow * (ch + 8)]; o[k] = new float[ow * oh];
            for (int yy = -4; yy < ch + 4; yy++) for (int x = 0; x < ow; x++) {
                double sx = (x + 0.5) / s - 0.5 + x0; tmp[(yy + 4) * ow + x] = Sample(src[k], w, h, sx, yy + y0, kernel, true); }
            for (int y = 0; y < oh; y++) for (int x = 0; x < ow; x++) {
                double sy = (y + 0.5) / s - 0.5; double v = 0, wsum = 0; int iy = (int)Math.Floor(sy);
                if (kernel == 3) { int ny = (int)Math.Round(sy); ny = Math.Max(-4, Math.Min(ch + 3, ny)); o[k][y * ow + x] = tmp[(ny + 4) * ow + x]; continue; }
                int r = kernel == 2 ? 1 : 2;
                for (int j = iy - r + 1; j <= iy + r; j++) { double t = sy - j; double wt = kernel == 0 ? CR(t) : kernel == 1 ? L2(t) : Math.Max(0, 1 - Math.Abs(t));
                    int jj = Math.Max(-4, Math.Min(ch + 3, j)); v += wt * tmp[(jj + 4) * ow + x]; wsum += wt; }
                o[k][y * ow + x] = (float)(v / wsum); }
        }
        return o;
    }
    static float Sample(float[] c, int w, int h, double sx, int y, int kernel, bool horiz) {
        if (kernel == 3) return Px(c, w, h, (int)Math.Round(sx), y);
        int ix = (int)Math.Floor(sx), r = kernel == 2 ? 1 : 2; double v = 0, ws = 0;
        for (int i = ix - r + 1; i <= ix + r; i++) { double t = sx - i; double wt = kernel == 0 ? CR(t) : kernel == 1 ? L2(t) : Math.Max(0, 1 - Math.Abs(t)); v += wt * Px(c, w, h, i, y); ws += wt; }
        return (float)(v / ws);
    }
    // AMD RCAS (robust contrast-adaptive sharpening), sharpness 0 = strongest
    public static void Rcas(float[][] c, int w, int h, double sharp) {
        double lim = 0.25 - 1.0 / 16; double k = Math.Pow(2, -sharp);
        var src = new float[3][]; for (int i = 0; i < 3; i++) src[i] = (float[])c[i].Clone();
        for (int y = 1; y < h - 1; y++) for (int x = 1; x < w - 1; x++) {
            double lobe = 0; double best = -lim; bool first = true;
            for (int ch = 0; ch < 3; ch++) {
                double n = src[ch][(y - 1) * w + x] / 255, s = src[ch][(y + 1) * w + x] / 255, e = src[ch][y * w + x + 1] / 255, ww = src[ch][y * w + x - 1] / 255;
                double mn = Math.Min(Math.Min(n, s), Math.Min(e, ww)), mx = Math.Max(Math.Max(n, s), Math.Max(e, ww));
                double hitMin = mn / (4 * mx + 1e-5), hitMax = (1 - mx) / (4 * mn - 4 - 1e-5);
                double l = Math.Max(-hitMin, hitMax); if (first || l < best) { best = l; first = false; } }
            lobe = Math.Max(-lim, Math.Min(best, 0)) * k;
            for (int ch = 0; ch < 3; ch++) {
                double n = src[ch][(y - 1) * w + x], s = src[ch][(y + 1) * w + x], e = src[ch][y * w + x + 1], ww = src[ch][y * w + x - 1], cc = src[ch][y * w + x];
                c[ch][y * w + x] = (float)((lobe * (n + s + e + ww) + cc) / (4 * lobe + 1)); }
        }
    }
}
'@
$b1 = [Drawing.Bitmap]::FromFile((Resolve-Path $In1x)); $w = 0; $h = 0; $src = [Sc]::Load($b1, [ref]$w, [ref]$h); $b1.Dispose()
$panels = @()
$panels += ,@('Nearest (square pixels)', [Sc]::Save([Sc]::Resize($src, $w, $h, $X, $Y, $Size, $Size, $Scale, 3), $Size * $Scale, $Size * $Scale))
# current: 9:3:3:1 doubling (bilinear at quarter offsets) then a smooth stretch for the rest
$d2 = [Sc]::Resize($src, $w, $h, $X - 4, $Y - 4, $Size + 8, $Size + 8, 2, 2)
$cur = [Sc]::Resize($d2, ($Size + 8) * 2, ($Size + 8) * 2, 8, 8, $Size * 2, $Size * 2, $Scale / 2, 2)
$panels += ,@('Now: bilinear (AI art off)', [Sc]::Save($cur, $Size * $Scale, $Size * $Scale))
$panels += ,@('Catmull-Rom', [Sc]::Save([Sc]::Resize($src, $w, $h, $X, $Y, $Size, $Size, $Scale, 0), $Size * $Scale, $Size * $Scale))
$fsr = [Sc]::Resize($src, $w, $h, $X, $Y, $Size, $Size, $Scale, 1); [Sc]::Rcas($fsr, $Size * $Scale, $Size * $Scale, 0.2)
$panels += ,@('FSR-style (Lanczos + RCAS)', [Sc]::Save($fsr, $Size * $Scale, $Size * $Scale))
if ($InHD) {
    $bh = [Drawing.Bitmap]::FromFile((Resolve-Path $InHD)); $hw = 0; $hh = 0; $hd = [Sc]::Load($bh, [ref]$hw, [ref]$hh); $bh.Dispose()
    $panels += ,@('AI art (the patch now)', [Sc]::Save([Sc]::Resize($hd, $hw, $hh, $X * 2, $Y * 2, $Size * 2, $Size * 2, $Scale / 2, 2), $Size * $Scale, $Size * $Scale))
}
$ps = $Size * $Scale; $cols = 3; $rows = [Math]::Ceiling($panels.Count / $cols)
$o = New-Object Drawing.Bitmap ($cols * ($ps + 8)), ($rows * ($ps + 40))
$g = [Drawing.Graphics]::FromImage($o); $g.Clear([Drawing.Color]::Black); $font = New-Object Drawing.Font 'Arial', 18
for ($i = 0; $i -lt $panels.Count; $i++) { $px = ($i % $cols) * ($ps + 8); $py = [Math]::Floor($i / $cols) * ($ps + 40)
    $g.DrawString($panels[$i][0], $font, [Drawing.Brushes]::White, $px + 6, $py + 6); $g.DrawImage($panels[$i][1], $px, $py + 36, $ps, $ps) }
$g.Dispose(); $o.Save($Out, [Drawing.Imaging.ImageFormat]::Png); $o.Dispose()
"saved $Out ($($panels.Count) panels at ${Scale}x)"
