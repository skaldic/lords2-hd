using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

// 2x versions of an animation that carries its own background (see Room.AppendKey): the background
// is taken out and filled with colours bled from the figures before upscaling, as for the sprites.
public static class KeyArt
{
    const int Pad = 4;

    static bool Background(byte[] f, byte[] bg, byte[] mark, int i) { return mark[i] != 0 && f[i] == bg[i]; }

    public static void SaveBled(byte[] f, byte[] bg, byte[] mark, int w, int h, Color[] pal, string path)
    {
        int pw = w + 2 * Pad, ph = h + 2 * Pad;
        var px = new int[pw * ph]; var known = new bool[pw * ph];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int i = y * w + x;
                if (Background(f, bg, mark, i)) continue;
                int p = (y + Pad) * pw + x + Pad;
                px[p] = pal[f[i]].ToArgb(); known[p] = true;
            }
        for (int pass = 0; pass < 6; pass++)
        {
            var nk = (bool[])known.Clone();
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++)
                {
                    int p = y * pw + x;
                    if (known[p]) continue;
                    int r = 0, g = 0, b = 0, n = 0;
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                        {
                            int a = x + dx, c = y + dy;
                            if (a < 0 || c < 0 || a >= pw || c >= ph || !known[c * pw + a]) continue;
                            Color k = Color.FromArgb(px[c * pw + a]); r += k.R; g += k.G; b += k.B; n++;
                        }
                    if (n > 0) { px[p] = Color.FromArgb(r / n, g / n, b / n).ToArgb(); nk[p] = true; }
                }
            known = nk;
        }
        for (int p = 0; p < px.Length; p++) if (!known[p]) px[p] = unchecked((int)0xFF404040);
        using (var bmp = new Bitmap(pw, ph, PixelFormat.Format32bppArgb))
        {
            var d = bmp.LockBits(new Rectangle(0, 0, pw, ph), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            Marshal.Copy(px, 0, d.Scan0, px.Length); bmp.UnlockBits(d);
            bmp.Save(path, ImageFormat.Png);
        }
    }

    // The upscaled (x4) picture back at 2x, as BGRX.
    public static byte[] Load(string upscaled, int w, int h)
    {
        using (var src = new Bitmap(upscaled))
        using (var hd = new Bitmap(w * 2, h * 2, PixelFormat.Format32bppRgb))
        using (var g = Graphics.FromImage(hd))
        {
            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
            g.DrawImage(src, new Rectangle(0, 0, w * 2, h * 2), new Rectangle(Pad * 4, Pad * 4, w * 4, h * 4), GraphicsUnit.Pixel);
            var d = hd.LockBits(new Rectangle(0, 0, w * 2, h * 2), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
            var o = new byte[w * 2 * h * 2 * 4];
            Marshal.Copy(d.Scan0, o, 0, o.Length);
            hd.UnlockBits(d);
            return o;
        }
    }
}
