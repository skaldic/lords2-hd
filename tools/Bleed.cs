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
        bool full = true;
        for (int i = 0; i < W * H && full; i++) full = !Key(f, i);
        if (full) { // a solid tile: wrap it around itself, so its edges upscale as if joined
            for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) {
                int p = y * PW + x;
                if (known[p]) continue;
                int sy = ((y - pad) % H + H) % H, sx = ((x - pad) % W + W) % W;
                px[p] = pal[f.Pixels[sy * W + sx]].ToArgb(); known[p] = true;
            }
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

// Water keeps its original look: the upscaler reads each tile differently, so tiles showed as squares.
// Solid water tiles get Scale2x (EPX) of the original instead, which only looks at a pixel's
// neighbours and so stays seamless.
public static class PixelWater {
    public static bool IsWater(Pl8Frame f, System.Drawing.Color[] pal) {
        long r = 0, g = 0, b = 0;
        for (int i = 0; i < f.Pixels.Length; i++) {
            if (Bleed.Key(f, i)) return false;
            var c = pal[f.Pixels[i]]; r += c.R; g += c.G; b += c.B;
        }
        int n = f.Pixels.Length;
        return n > 0 && b / n > r / n + 15 && g / n > r / n + 15;
    }
    static byte Epx(Pl8Frame f, int x, int y) {
        int W = f.Width, H = f.Height, cx = x / 2, cy = y / 2;
        Func<int, int, byte> P = (a, c) => f.Pixels[((c % H + H) % H) * W + ((a % W + W) % W)];
        byte e = P(cx, cy), up = P(cx, cy - 1), l = P(cx - 1, cy), r = P(cx + 1, cy), dn = P(cx, cy + 1);
        if (up == dn || l == r) return e;
        bool right = (x & 1) == 1, down = (y & 1) == 1;
        if (!right && !down) return l == up ? l : e;
        if (right && !down) return up == r ? r : e;
        if (!right && down) return l == dn ? l : e;
        return dn == r ? r : e;
    }
    // Overwrites the 2x pixels of a Bleed.Frame record (u16 w, u16 h, indices, 2w x 2h BGRX).
    public static void Apply(byte[] o, int at, Pl8Frame f, System.Drawing.Color[] pal) {
        int W = f.Width, H = f.Height, start = at + 4 + W * H;
        for (int y = 0; y < 2 * H; y++) for (int x = 0; x < 2 * W; x++) {
            var c = pal[Epx(f, x, y)];
            int p = start + (y * 2 * W + x) * 4;
            o[p] = c.B; o[p + 1] = c.G; o[p + 2] = c.R;
        }
    }
    // The same on a built sprite file, in place: returns how many tiles changed.
    public static int Patch(string bin, Pl8Frame[] f, System.Drawing.Color[] pal) {
        byte[] d = File.ReadAllBytes(bin);
        int n = 0;
        for (int i = 0; i < f.Length; i++) {
            int off = BitConverter.ToInt32(d, 4 + 4 * i);
            if (off == 0 || f[i].Error != null || !IsWater(f[i], pal)) continue;
            Apply(d, off, f[i], pal); n++;
        }
        File.WriteAllBytes(bin, d);
        return n;
    }
}

public static class BlendBack {
    // Mixes the doubled original into a Bleed.Frame record: o = u16 w, u16 h, indices, 2w x 2h BGRX.
    public static void Apply(byte[] o, Pl8Frame f, System.Drawing.Color[] pal, double plain) {
        int W = f.Width, H = f.Height, start = 4 + W * H;
        for (int y = 0; y < 2 * H; y++) for (int x = 0; x < 2 * W; x++) {
            int i = (y / 2) * W + x / 2;
            if (Bleed.Key(f, i)) continue;
            var c = pal[f.Pixels[i]];
            int p = start + (y * 2 * W + x) * 4;
            o[p] = (byte)(o[p] * (1 - plain) + c.B * plain);
            o[p + 1] = (byte)(o[p + 1] * (1 - plain) + c.G * plain);
            o[p + 2] = (byte)(o[p + 2] * (1 - plain) + c.R * plain);
        }
    }
}
