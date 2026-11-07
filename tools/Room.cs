using System;
using System.IO;

// Wider rooms: each painting's 2x version (BGRX, 1280x960) carried on to 854 columns, for the DLL's
// lords2hd\wide\<name>.bin. Recipes work in 1x coordinates (x1, y1) and read the 2x painting.
public static class Room
{
    const int M = 107, W = 640 + 2 * M, H = 480;

    static byte[] Load(string bin) { return File.ReadAllBytes(bin); }

    static void Get(byte[] pic, int sx, int sy, double k, byte[] o, int d)
    {
        sx = Math.Max(0, Math.Min(1279, sx)); sy = Math.Max(0, Math.Min(959, sy));
        int s = 768 + (sy * 1280 + sx) * 4;
        for (int c = 0; c < 3; c++) o[d + c] = (byte)Math.Min(255, file_k(pic[s + c], k));
    }
    static double file_k(byte v, double k) { return v * k; }

    static void Save(string outBin, byte[] o, int cutL, int cutR, int floorY, int barY)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(outBin));
        using (var f = new BinaryWriter(File.Create(outBin)))
        {
            f.Write(W); f.Write(cutL); f.Write(cutR); f.Write(floorY); f.Write(barY);
            f.Write(o);
        }
    }

    // Some animations carry their own background, cut from elsewhere in the texture. The frames'
    // background (each pixel's commonest value, kept where at least 4 frames share it) is appended to
    // the room file as a key: int count; per key int w, h, then w*h values and w*h marks (1 = background,
    // 2 = background in every frame, which the DLL uses to find where the animation is drawn); then
    // int frames, and per frame w*h values and its 2x version, 2w*2h BGRX.
    public static void AppendKey(string outBin, byte[][] frames, int w, int h, byte[][] hd)
    {
        byte[] bg, mark;
        KeyBackground(frames, w, h, out bg, out mark);
        byte[] old = File.ReadAllBytes(outBin);
        int baseLen = 20 + 2 * W * 2 * H * 4;
        using (var f = new BinaryWriter(File.Create(outBin)))
        {
            f.Write(old, 0, baseLen);
            int count = old.Length > baseLen ? BitConverter.ToInt32(old, baseLen) : 0;
            f.Write(count + 1);
            if (old.Length > baseLen + 4) f.Write(old, baseLen + 4, old.Length - baseLen - 4);
            f.Write(w); f.Write(h); f.Write(bg); f.Write(mark);
            f.Write(frames.Length);
            for (int k = 0; k < frames.Length; k++) { f.Write(frames[k]); f.Write(hd[k]); }
        }
    }

    public static void KeyBackground(byte[][] frames, int w, int h, out byte[] bg, out byte[] mark)
    {
        bg = new byte[w * h]; mark = new byte[w * h];
        for (int i = 0; i < w * h; i++)
        {
            int[] n = new int[256];
            foreach (var f in frames) n[f[i]]++;
            int best = 0;
            for (int v = 1; v < 256; v++) if (n[v] > n[best]) best = v;
            bg[i] = (byte)best;
            mark[i] = (byte)(n[best] == frames.Length ? 2 : n[best] >= 4 ? 1 : 0);
        }
        // Where a figure stands in most frames its colour wins: background only in colours the
        // always-background pixels use.
        bool[] bgColour = new bool[256];
        for (int i = 0; i < w * h; i++) if (mark[i] == 2) bgColour[bg[i]] = true;
        for (int i = 0; i < w * h; i++) if (mark[i] == 1 && !bgColour[bg[i]]) mark[i] = 0;
    }

    // A preview at 1x, as a 24-bit BMP.
    public static void Preview(string outBin, string bmp)
    {
        byte[] b = File.ReadAllBytes(outBin);
        int w2 = 2 * W, rowBytes = (W * 3 + 3) & ~3;
        using (var f = new BinaryWriter(File.Create(bmp)))
        {
            f.Write((ushort)0x4d42); f.Write(54 + rowBytes * H); f.Write(0); f.Write(54);
            f.Write(40); f.Write(W); f.Write(H); f.Write((short)1); f.Write((short)24);
            f.Write(0); f.Write(rowBytes * H); f.Write(0); f.Write(0); f.Write(0); f.Write(0);
            var row = new byte[rowBytes];
            for (int y = H - 1; y >= 0; y--)
            {
                for (int x = 0; x < W; x++)
                    for (int c = 0; c < 3; c++)
                        row[x * 3 + c] = b[20 + ((2 * y) * w2 + 2 * x) * 4 + c];
                f.Write(row);
            }
        }
    }

    static int PingPong(int i, int n) { i = ((i % (2 * n)) + 2 * n) % (2 * n); return i < n ? i : 2 * n - 1 - i; }

    // The middle 640 columns as painted; f(x2, y2, side, i2) fills the new columns, where side is
    // -1 or 1 and i2 counts 2x columns away from the painting's edge (0 next to it).
    delegate void Fill(byte[] o, int d, int x2, int y2, int side, int i2);
    static byte[] Centred(byte[] pic, Fill fill)
    {
        int w2 = 2 * W, h2 = 2 * H;
        byte[] o = new byte[w2 * h2 * 4];
        for (int y = 0; y < h2; y++)
            for (int x = 0; x < w2; x++)
            {
                int d = (y * w2 + x) * 4, sx = x - 2 * M;
                if (sx >= 0 && sx < 1280) Get(pic, sx, y, 1, o, d);
                else if (sx < 0) fill(o, d, x, y, -1, -sx - 1);
                else fill(o, d, x, y, 1, sx - 1280);
            }
        return o;
    }

    // A plain patch of the painting's own texture, tiled to and fro, a little darker at the joins.
    static void Texture(string bin, string outBin, int px, int py, int pw, int ph, double edgeDark)
    {
        byte[] pic = Load(bin);
        byte[] o = Centred(pic, (o2, d, x2, y2, side, i2) =>
        {
            double k = 1;
            if (i2 < 8) k = 1 - edgeDark * (8 - i2) / 8.0;
            Get(pic, 2 * px + PingPong(i2 + (side < 0 ? 37 : 0), 2 * pw), 2 * py + PingPong(y2, 2 * ph), k, o2, d);
        });
        Save(outBin, o, 0, 640, 480, 480);
    }

    // The same, with the patch taken from another painting of the same texture.
    static void TextureFrom(string bin, string patchBin, string outBin, int px, int py, int pw, int ph, double edgeDark)
    {
        byte[] pic = Load(bin), patch = Load(patchBin);
        byte[] o = Centred(pic, (o2, d, x2, y2, side, i2) =>
        {
            double k = i2 < 8 ? 1 - edgeDark * (8 - i2) / 8.0 : 1;
            Get(patch, 2 * px + PingPong(i2 + (side < 0 ? 37 : 0), 2 * pw), 2 * py + PingPong(y2, 2 * ph), k, o2, d);
        });
        Save(outBin, o, 0, 640, 480, 480);
    }

    // The edges mirrored, fading to black: for dark pictures.
    static void MirrorFade(string bin, string outBin)
    {
        byte[] pic = Load(bin);
        byte[] o = Centred(pic, (o2, d, x2, y2, side, i2) =>
        {
            double t = i2 / (2.0 * M);
            Get(pic, side < 0 ? i2 : 1279 - i2, y2, (1 - t) * (1 - t), o2, d);
        });
        Save(outBin, o, 0, 640, 480, 480);
    }

    // Stone courses: rows py .. py+ph repeated straight down (no mirroring), columns from px on.
    static void Courses(string bin, string outBin, int px, int py2, int ph2, double edgeDark)
    {
        byte[] pic = Load(bin);
        byte[] o = Centred(pic, (o2, d, x2, y2, side, i2) =>
        {
            double k = i2 < 8 ? 1 - edgeDark * (8 - i2) / 8.0 : 1;
            Get(pic, 2 * px + (side < 0 ? 2 * M - 1 - i2 : i2), py2 + (((y2 - py2) % ph2) + ph2) % ph2, k, o2, d);
        });
        Save(outBin, o, 0, 640, 480, 480);
    }

    // Sides painted by tools\paint_sides.ps1 (assets\wide\<name>-gen<seed>.png, 854x480), upscaled to
    // 1708x960 (sides2x): the HD painting stays in the middle, faded into the painted sides at the joins.
    public static void Painted(string bin, string sides2x, string outBin)
    {
        byte[] pic = Load(bin);
        byte[] side;
        using (var b = new System.Drawing.Bitmap(sides2x))
        using (var c = new System.Drawing.Bitmap(2 * W, 2 * H, System.Drawing.Imaging.PixelFormat.Format32bppRgb))
        {
            using (var g = System.Drawing.Graphics.FromImage(c))
            {
                g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                g.DrawImage(b, 0, 0, 2 * W, 2 * H);
            }
            var d = c.LockBits(new System.Drawing.Rectangle(0, 0, 2 * W, 2 * H), System.Drawing.Imaging.ImageLockMode.ReadOnly, c.PixelFormat);
            side = new byte[2 * W * 2 * H * 4];
            System.Runtime.InteropServices.Marshal.Copy(d.Scan0, side, 0, side.Length);
            c.UnlockBits(d);
        }
        const int fade = 24;
        byte[] o = Centred(pic, (o2, d, x2, y2, s, i2) => { for (int k = 0; k < 3; k++) o2[d + k] = side[d + k]; });
        for (int y = 0; y < 2 * H; y++)
            for (int k = 0; k < fade; k++)
            {
                double t = 1 - (k + 0.5) / fade; // share of the painted side
                foreach (int x in new[] { 2 * M + k, 2 * M + 1279 - k })
                {
                    int d = (y * 2 * W + x) * 4;
                    for (int c = 0; c < 3; c++) o[d + c] = (byte)(o[d + c] * (1 - t) + side[d + c] * t);
                }
            }
        Save(outBin, o, 0, 640, 480, 480);
    }

    // Plain dark sides, with a soft shadow where the picture ends: nothing copied.
    public static void DarkSides(string bin, string outBin)
    {
        byte[] pic = Load(bin);
        byte[] o = Centred(pic, (o2, d, x2, y2, side, i2) => { o2[d] = o2[d + 1] = o2[d + 2] = 0; });
        // the shadow falls on the picture's own edge columns
        int w2 = 2 * W;
        for (int y = 0; y < 2 * H; y++)
            for (int k = 0; k < 24; k++)
            {
                double t = 0.35 + 0.65 * k / 24.0;
                foreach (int x in new[] { 2 * M + k, 2 * M + 1279 - k })
                {
                    int d = (y * w2 + x) * 4;
                    for (int c = 0; c < 3; c++) o[d + c] = (byte)(o[d + c] * t);
                }
            }
        Save(outBin, o, 0, 640, 480, 480);
    }

    // The picture itself enlarged to fill the frame, blurred hard and dimmed, behind the picture.
    public static void BlurBackdrop(string bin, string outBin)
    {
        byte[] pic = Load(bin);
        int w2 = 2 * W, h2 = 2 * H, sw = 160, sh = 120;   // blurred at a small size, then enlarged
        double[] small = new double[sw * sh * 3];
        // scale the 1280x960 picture to fill 1708 wide (crop top and bottom), sampled down
        double scale = 1280.0 / w2, cropY = (960 - h2 * scale) / 2;
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++)
            {
                double[] acc = new double[3]; int n = 0;
                for (int dy = 0; dy < 8; dy++)
                    for (int dx = 0; dx < 8; dx++)
                    {
                        int sx = (int)(((x + dx / 8.0) / sw) * w2 * scale), sy = (int)(cropY + ((y + dy / 8.0) / sh) * h2 * scale);
                        sx = Math.Max(0, Math.Min(1279, sx)); sy = Math.Max(0, Math.Min(959, sy));
                        int s = 768 + (sy * 1280 + sx) * 4;
                        for (int c = 0; c < 3; c++) acc[c] += pic[s + c];
                        n++;
                    }
                for (int c = 0; c < 3; c++) small[(y * sw + x) * 3 + c] = acc[c] / n;
            }
        for (int pass = 0; pass < 6; pass++) // box blur, several times
        {
            double[] t = new double[small.Length];
            for (int y = 0; y < sh; y++)
                for (int x = 0; x < sw; x++)
                    for (int c = 0; c < 3; c++)
                    {
                        double a = 0; int n = 0;
                        for (int dy = -2; dy <= 2; dy++)
                            for (int dx = -2; dx <= 2; dx++)
                            {
                                int xx = Math.Max(0, Math.Min(sw - 1, x + dx)), yy = Math.Max(0, Math.Min(sh - 1, y + dy));
                                a += small[(yy * sw + xx) * 3 + c]; n++;
                            }
                        t[(y * sw + x) * 3 + c] = a / n;
                    }
            small = t;
        }
        byte[] o = Centred(pic, (o2, d, x2, y2, side, i2) =>
        {
            double fx = x2 * (double)sw / w2 - 0.5, fy = y2 * (double)sh / h2 - 0.5;
            int x0 = Math.Max(0, Math.Min(sw - 2, (int)fx)), y0 = Math.Max(0, Math.Min(sh - 2, (int)fy));
            double ax = Math.Max(0, Math.Min(1, fx - x0)), ay = Math.Max(0, Math.Min(1, fy - y0));
            double k = 0.55 * (0.75 + 0.25 * Math.Min(1, i2 / 60.0));  // a touch darker next to the picture
            for (int c = 0; c < 3; c++)
            {
                double v = (small[(y0 * sw + x0) * 3 + c] * (1 - ax) + small[(y0 * sw + x0 + 1) * 3 + c] * ax) * (1 - ay)
                         + (small[((y0 + 1) * sw + x0) * 3 + c] * (1 - ax) + small[((y0 + 1) * sw + x0 + 1) * 3 + c] * ax) * ay;
                o2[d + c] = (byte)(v * k);
            }
        });
        Save(outBin, o, 0, 640, 480, 480);
    }

    // The outer strips (left, right columns wide) reflected to and fro.
    static void EdgeBounce(string bin, string outBin, int left, int right)
    {
        byte[] pic = Load(bin);
        byte[] o = Centred(pic, (o2, d, x2, y2, side, i2) =>
            Get(pic, side < 0 ? PingPong(i2, 2 * left) : 1279 - PingPong(i2, 2 * right), y2, 1, o2, d));
        Save(outBin, o, 0, 640, 480, 480);
    }

    // A photo-like scene: the edges mirrored, blurred more and more and darkened, like a vignette.
    static void SoftEdge(string bin, string outBin)
    {
        byte[] pic = Load(bin);
        byte[] o = Centred(pic, (o2, d, x2, y2, side, i2) =>
        {
            double t = i2 / (2.0 * M);
            int r = 1 + (int)(t * 24);
            double[] acc = new double[3]; int n = 0;
            for (int dy = -r; dy <= r; dy += Math.Max(1, r / 4))
                for (int dx = -r; dx <= r; dx += Math.Max(1, r / 4))
                {
                    int sx = side < 0 ? i2 + dx : 1279 - i2 + dx, sy = y2 + dy;
                    sx = Math.Max(0, Math.Min(1279, sx)); sy = Math.Max(0, Math.Min(959, sy));
                    int s = 768 + (sy * 1280 + sx) * 4;
                    for (int c = 0; c < 3; c++) acc[c] += pic[s + c];
                    n++;
                }
            double k = 1 - 0.75 * t;
            for (int c = 0; c < 3; c++) o2[d + c] = (byte)(acc[c] / n * k);
        });
        Save(outBin, o, 0, 640, 480, 480);
    }

    // The greatest nobles: the frame stays at the edges and the sky and battlement carry on between,
    // copied whole merlon periods over (faded into a mirror of the sky at the joins). The bottom bar
    // moves whole with the middle, its frame carried on to the edges.
    static void Grtnoble(string bin, string outBin)
    {
        const int frameL = 24, frameR = 621, contentR = 600, floorY = 428, period = 131, skyY = 250;
        byte[] pic = Load(bin);
        int w2 = 2 * W, h2 = 2 * H;
        byte[] o = new byte[w2 * h2 * 4];
        for (int y = 0; y < h2; y++)
            for (int x = 0; x < w2; x++)
            {
                int d = (y * w2 + x) * 4, x1 = x / 2, y1 = y / 2, v = x - 2 * M; // v: the painting's 2x column
                if (y1 >= floorY)
                {
                    if (v < 0 || v >= 1280) Get(pic, 4 + PingPong(v < 0 ? -v : v - 1280, 72), y, 1, o, d);
                    else Get(pic, v, y, 1, o, d);
                }
                else if (x1 < frameL) Get(pic, x, y, 1, o, d);
                else if (x1 >= frameR + 2 * M) Get(pic, x - 4 * M, y, 1, o, d);
                else if (v >= 2 * frameL && v < 2 * contentR) Get(pic, v, y, 1, o, d);
                else
                {
                    bool left = v < 2 * frameL;
                    Get(pic, left ? v + 2 * period : v - 2 * period, y, 1, o, d);
                    int fromJoin = left ? 2 * frameL - 1 - v : v - 2 * contentR;
                    const int blend = 60;
                    if (fromJoin < blend && y1 < skyY)
                    {
                        int b = left ? 2 * frameL + fromJoin : 2 * contentR - 1 - fromJoin;
                        double t = (1 - fromJoin / (double)blend) * Math.Min(1, (skyY - y1) / 30.0); // fading out above the merlons
                        int s = 768 + (y * 1280 + b) * 4;
                        for (int c = 0; c < 3; c++) o[d + c] = (byte)(o[d + c] * (1 - t) + pic[s + c] * t);
                    }
                }
            }
        Save(outBin, o, frameL, frameR, floorY, 480);
    }
    public static void Build(string screens, string wide)
    {
        Func<string, string> i = n => Path.Combine(screens, n + ".bin");
        Func<string, string> w = n => Path.Combine(wide, n + ".bin");
        Armoury(i("armoury"), w("armoury"));
        foreach (var n in new[] { "start", "lords2", "gateway" }) MirrorFade(i(n), w(n));
        Courses(i("cas_back"), w("cas_back"), 170, 441, 94, 0.3);
        Texture(i("custom"), w("custom"), 170, 250, 450, 220, 0.3);
        TextureFrom(i("skirmish"), i("skircust"), w("skirmish"), 226, 180, 212, 190, 0.3);
        TextureFrom(i("skircust"), i("skircust"), w("skircust"), 226, 180, 212, 190, 0.3);
        Texture(i("score2"), w("score2"), 210, 25, 210, 115, 0.2);
        EdgeBounce(i("score1"), w("score1"), 70, 80);
        SoftEdge(i("merchant"), w("merchant"));
        Grtnoble(i("grtnoble"), w("grtnoble"));
    }

    const int CutL = 62, CutR = 578, FloorY = 366, BarY = 395;

    public static void Armoury(string paintingBin, string outBin)
    {
        byte[] file = File.ReadAllBytes(paintingBin);
        int w2 = 2 * W, h2 = 2 * H;
        byte[] o = new byte[w2 * h2 * 4];
        for (int y = 0; y < h2; y++)
            for (int x = 0; x < w2; x++)
            {
                double light = 1; int sx, sy = y;
                int x1 = x / 2, y1 = y / 2;
                if (y1 >= BarY)
                {
                    sx = x - 4 * M;                                     // whole, against the right edge
                    if (sx < 170) sx = 170 + ((sx - 170) % 900 + 900) % 900; // then all six blocks, in turn
                }
                else if (y1 >= FloorY) sx = 200 + x % 800;
                else if (x1 < CutL) sx = x;
                else if (x1 >= CutL + M && x1 < CutR + M) sx = x - 2 * M;
                else if (x1 >= CutR + 2 * M) sx = x - 4 * M;
                else
                {
                    int i = x1 < 400 ? x - 2 * CutL : x - 2 * (CutR + M);
                    if (y1 < 56) sx = 140 + i % 80;
                    else if (y1 < 86) sx = 430 + i % 90;
                    else
                    {
                        int k = i % 320; if (k >= 160) k = 319 - k;
                        sx = 380 + k;
                        sy = 540 + ((y - 540) % 164 + 164) % 164;
                        light = 0.55;
                        if (y1 < 130) light *= 0.3 + 0.7 * (y1 - 86) / 44.0;
                        if (y1 > 340) light *= 1 - 0.5 * (y1 - 340) / 26.0;
                        int edge = Math.Min(i, 2 * M - 1 - i);
                        if (edge < 24) light *= 0.55 + 0.45 * edge / 24.0;
                    }
                }
                int s = 768 + (sy * 1280 + sx) * 4, d = (y * w2 + x) * 4;
                for (int c = 0; c < 3; c++) o[d + c] = (byte)(file[s + c] * light);
            }
        Save(outBin, o, CutL, CutR, FloorY, BarY);
    }
}
