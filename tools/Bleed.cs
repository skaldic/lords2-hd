using System; using System.IO; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
public static class Bleed {
    public static bool Key(Pl8Frame f, int i) { return !f.Opaque[i] || f.Pixels[i] == 0; }
    public static void Save(Pl8Frame f, Color[] pal, string path) {
        int W = f.Width, H = f.Height, pad = 4, PW = W + pad * 2, PH = H + pad * 2;
        var px = new int[PW * PH]; var known = new bool[PW * PH];
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int i = y * W + x;
            if (Key(f, i)) continue;
            int p = (y + pad) * PW + x + pad;
            px[p] = pal[f.Pixels[i]].ToArgb(); known[p] = true;
        }
        for (int pass = 0; pass < 6; pass++) {
            var nk = (bool[])known.Clone();
            for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) {
                int p = y * PW + x;
                if (known[p]) continue;
                int r = 0, g = 0, b = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                    int a = x + dx, c = y + dy;
                    if (a < 0 || c < 0 || a >= PW || c >= PH || !known[c * PW + a]) continue;
                    Color k = Color.FromArgb(px[c * PW + a]); r += k.R; g += k.G; b += k.B; n++;
                }
                if (n > 0) { px[p] = Color.FromArgb(r / n, g / n, b / n).ToArgb(); nk[p] = true; }
            }
            known = nk;
        }
        for (int p = 0; p < px.Length; p++) if (!known[p]) px[p] = unchecked((int)0xFF404040);
        using (var bmp = new Bitmap(PW, PH, PixelFormat.Format32bppArgb)) {
            var d = bmp.LockBits(new Rectangle(0, 0, PW, PH), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            Marshal.Copy(px, 0, d.Scan0, px.Length); bmp.UnlockBits(d);
            bmp.Save(path, ImageFormat.Png);
        }
    }
    public static byte[] Frame(Pl8Frame f, string upscaled) {
        int W = f.Width, H = f.Height, pad = 4;
        using (var src = new Bitmap(upscaled))
        using (var hd = new Bitmap(W * 2, H * 2, PixelFormat.Format32bppRgb))
        using (var g = Graphics.FromImage(hd)) {
            g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
            g.DrawImage(src, new Rectangle(0, 0, W * 2, H * 2), new Rectangle(pad * 4, pad * 4, W * 4, H * 4), GraphicsUnit.Pixel);
            var d = hd.LockBits(new Rectangle(0, 0, W * 2, H * 2), ImageLockMode.ReadOnly, PixelFormat.Format32bppRgb);
            var o = new byte[4 + W * H + W * 2 * H * 2 * 4];
            BitConverter.GetBytes((ushort)W).CopyTo(o, 0); BitConverter.GetBytes((ushort)H).CopyTo(o, 2);
            for (int i = 0; i < W * H; i++) o[4 + i] = Key(f, i) ? (byte)0 : f.Pixels[i];
            Marshal.Copy(d.Scan0, o, 4 + W * H, W * 2 * H * 2 * 4);
            hd.UnlockBits(d);
            return o;
        }
    }
}
