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
    // Shore tiles: only their water pixels (colours the solid water tiles use, with water around them
    // so a lone pixel of that colour on land stays), the land keeps its upscale.
    public static void ApplyShore(byte[] o, int at, Pl8Frame f, System.Drawing.Color[] pal, bool[] waterColour) {
        int W = f.Width, H = f.Height, start = at + 4 + W * H;
        // The water: teal pixels, and every water-tile colour joined to them (the dark ripples).
        bool[] any = AnyWater(pal, waterColour), water = new bool[W * H];
        var todo = new System.Collections.Generic.Stack<int>();
        for (int i = 0; i < W * H; i++) if (!Bleed.Key(f, i) && waterColour[f.Pixels[i]]) { water[i] = true; todo.Push(i); }
        while (todo.Count > 0) {
            int i = todo.Pop(), x0 = i % W, y0 = i / W;
            foreach (int j in new[] { x0 > 0 ? i - 1 : -1, x0 < W - 1 ? i + 1 : -1, y0 > 0 ? i - W : -1, y0 < H - 1 ? i + W : -1 })
                if (j >= 0 && !water[j] && !Bleed.Key(f, j) && any[f.Pixels[j]]) { water[j] = true; todo.Push(j); }
        }
        Func<int, int, bool> wet = (a, c) => a < 0 || c < 0 || a >= W || c >= H || water[c * W + a];
        for (int y = 0; y < 2 * H; y++) for (int x = 0; x < 2 * W; x++) {
            int cx = x / 2, cy = y / 2;
            if (!water[cy * W + cx]) continue;
            int around = (wet(cx - 1, cy) ? 1 : 0) + (wet(cx + 1, cy) ? 1 : 0) + (wet(cx, cy - 1) ? 1 : 0) + (wet(cx, cy + 1) ? 1 : 0);
            if (around < 3) continue;
            var c = pal[Epx(f, x, y)];
            int p = start + (y * 2 * W + x) * 4;
            o[p] = c.B; o[p + 1] = c.G; o[p + 2] = c.R;
        }
    }
    // Every colour the solid water tiles use (set by WaterColours).
    static bool[] s_any;
    static bool[] AnyWater(System.Drawing.Color[] pal, bool[] teal) { return s_any ?? teal; }
    public static bool[] WaterColours(Pl8Frame[] f, System.Drawing.Color[] pal) {
        s_any = new bool[256];
        foreach (var fr in f) if (fr.Error == null && IsWater(fr, pal)) foreach (var v in fr.Pixels) s_any[v] = true;
        // The teal colours the water tiles use: grass shares some dark ones, which are left out.
        long[] wet = new long[256];
        foreach (var fr in f) if (fr.Error == null && IsWater(fr, pal)) foreach (var v in fr.Pixels) wet[v]++;
        var w = new bool[256];
        for (int v = 0; v < 256; v++) {
            var c = pal[v];
            w[v] = wet[v] > 0 && c.B > c.R + 25 && c.G > c.R + 25 && c.B + c.G > 120;
        }
        return w;
    }
    static int Count(Pl8Frame f, bool[] wc) { int n = 0; for (int i = 0; i < f.Pixels.Length; i++) if (!Bleed.Key(f, i) && wc[f.Pixels[i]]) n++; return n; }
    // A tile with a real stretch of water in it (a quarter or more).
    public static bool IsShore(Pl8Frame f, bool[] wc) { return Count(f, wc) * 4 >= f.Pixels.Length; }

    // The same on a built sprite file, in place: returns how many tiles changed.
    public static int Patch(string bin, Pl8Frame[] f, System.Drawing.Color[] pal) {
        byte[] d = File.ReadAllBytes(bin);
        bool[] wc = WaterColours(f, pal);
        int n = 0;
        for (int i = 0; i < f.Length; i++) {
            int off = BitConverter.ToInt32(d, 4 + 4 * i);
            if (off == 0 || f[i].Error != null) continue;
            if (IsWater(f[i], pal)) { Apply(d, off, f[i], pal); n++; }
            else if (IsShore(f[i], wc)) { ApplyShore(d, off, f[i], pal, wc); n++; }
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

// Portraits: the upscaler sharpens faces but shifts their light and colour (waxy skin, brighter eyes).
// Likeness.Patch puts the original's own tones back under the new detail - hd + (blur(orig) - blur(hd)) -
// and mixes a share of the doubled original back in as grain. Works on a built sprite file in place.
public static class Likeness {
    static double[] Blur(double[] a, int W, int H, int r) {
        var t = new double[a.Length]; var b = new double[a.Length];
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 3; c++) {
            double s = 0; int n = 0;
            for (int k = -r; k <= r; k++) { int xx = x + k; if (xx < 0 || xx >= W) continue; s += a[(y * W + xx) * 3 + c]; n++; }
            t[(y * W + x) * 3 + c] = s / n;
        }
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int c = 0; c < 3; c++) {
            double s = 0; int n = 0;
            for (int k = -r; k <= r; k++) { int yy = y + k; if (yy < 0 || yy >= H) continue; s += t[(yy * W + x) * 3 + c]; n++; }
            b[(y * W + x) * 3 + c] = s / n;
        }
        return b;
    }
    public static int Patch(string bin, System.Drawing.Color[] pal, double grain) {
        byte[] d = File.ReadAllBytes(bin); int n = BitConverter.ToInt32(d, 0), done = 0;
        for (int i = 0; i < n; i++) {
            int off = BitConverter.ToInt32(d, 4 + 4 * i); if (off == 0) continue;
            int w = BitConverter.ToUInt16(d, off), h = BitConverter.ToUInt16(d, off + 2), idx = off + 4, hd = idx + w * h, W = 2 * w, H = 2 * h;
            var o2 = new double[W * H * 3]; var h2 = new double[W * H * 3];
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                int p = (y * W + x) * 3, q = hd + (y * W + x) * 4; var c = pal[d[idx + (y / 2) * w + x / 2]];
                o2[p] = c.R; o2[p + 1] = c.G; o2[p + 2] = c.B; h2[p] = d[q + 2]; h2[p + 1] = d[q + 1]; h2[p + 2] = d[q];
            }
            var bo = Blur(o2, W, H, 4); var bh = Blur(h2, W, H, 4);
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
                if (d[idx + (y / 2) * w + x / 2] == 0) continue;
                int p = (y * W + x) * 3, q = hd + (y * W + x) * 4;
                for (int c = 0; c < 3; c++) {
                    double v = (h2[p + c] + bo[p + c] - bh[p + c]) * (1 - grain) + o2[p + c] * grain;
                    d[q + 2 - c] = (byte)Math.Max(0, Math.Min(255, v + 0.5));
                }
            }
            done++;
        }
        File.WriteAllBytes(bin, d);
        return done;
    }
}
