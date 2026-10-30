// Reader for Lords of the Realm II picture files (.pl8) and palettes (.256).
//
// .pl8 layout (little-endian):
//   u16 compressed (0 = raw rows, 1 = row commands), u16 frame count, u16 ?, u8 0, u8 ?
//   per frame, 16 bytes: u16 width, u16 height, u32 data offset, u16 x, u16 y, u32 ?
// Raw frames are width*height palette indices. Compressed frames are rows of commands:
//   0x00 n  -> skip n transparent pixels
//   n       -> n literal pixels follow (n = 1..127)
// a row ends when it reaches the frame width.
// .256 is 256 RGB triples with 6-bit components (VGA DAC).
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public class Pl8Frame
{
    public int Width, Height, X, Y, Offset;
    public uint Extra;
    public byte[] Pixels;   // palette indices
    public bool[] Opaque;   // false where transparent
    public string Error;
}

public static class Pl8
{
    public static Color[] ReadPalette(string path)
    {
        byte[] b = File.ReadAllBytes(path);
        var pal = new Color[256];
        for (int i = 0; i < 256; i++)
        {
            int r = b[i * 3], g = b[i * 3 + 1], bl = b[i * 3 + 2];
            pal[i] = Color.FromArgb((r << 2) | (r >> 4), (g << 2) | (g >> 4), (bl << 2) | (bl >> 4));
        }
        return pal;
    }

    public static Pl8Frame[] Read(string path, out int compressed, out string header)
    {
        byte[] b = File.ReadAllBytes(path);
        compressed = b[0];
        int count = BitConverter.ToUInt16(b, 2);
        header = string.Format("{0} {1} {2} {3} {4}", compressed, b[1], count, BitConverter.ToUInt16(b, 4), b[7]);
        var frames = new Pl8Frame[count];
        // Bytes available to each frame: up to the next higher offset (or the end of the file).
        var offsets = new int[count];
        for (int i = 0; i < count; i++) offsets[i] = BitConverter.ToInt32(b, 8 + i * 16 + 4);
        var sorted = (int[])offsets.Clone();
        Array.Sort(sorted);
        for (int i = 0; i < count; i++)
        {
            int e = 8 + i * 16;
            var f = new Pl8Frame
            {
                Width = BitConverter.ToUInt16(b, e),
                Height = BitConverter.ToUInt16(b, e + 2),
                Offset = BitConverter.ToInt32(b, e + 4),
                X = BitConverter.ToInt16(b, e + 8),
                Y = BitConverter.ToInt16(b, e + 10),
                Extra = BitConverter.ToUInt32(b, e + 12),
            };
            int n = f.Width * f.Height;
            f.Pixels = new byte[n];
            f.Opaque = new bool[n];
            try
            {
                // The type is only a hint: raw and diamond frames turn up in files of every
                // type (the last frame of castle/town files is a diamond, font_c2 is raw
                // under type 1), so the bytes a frame has decide.
                int next = b.Length;
                foreach (int o in sorted) if (o > f.Offset) { next = o; break; }
                int avail = next - f.Offset;
                if (compressed == 1)
                {
                    try { Rows(b, f); }
                    catch { if (avail == n) Raw(b, f); else throw; }
                }
                else if (compressed == 0 || compressed == 2)
                {
                    // Map tiles are always diamonds, some with extra overhang data after them.
                    if (avail >= n && !(f.Width == 58 && f.Height == 30)) Raw(b, f); else Diamond(b, f);
                }
                else throw new Exception("compression type " + compressed);
            }
            catch (Exception ex)
            {
                f.Error = ex.Message;
            }
            frames[i] = f;
        }
        return frames;
    }

    static void Raw(byte[] b, Pl8Frame f)
    {
        int n = f.Width * f.Height;
        if (f.Offset + n > b.Length) throw new Exception("raw frame past end of file");
        Array.Copy(b, f.Offset, f.Pixels, 0, n);
        for (int i = 0; i < n; i++) f.Opaque[i] = true;
    }

    // Isometric tile: only the diamond is stored, row by row, each row centred.
    // Rows widen by a fixed step from 2 pixels to the full width, then narrow again
    // (58x30 tiles: 2, 6, ... 58, 58, ... 6, 2 = 900 bytes).
    static void Diamond(byte[] b, Pl8Frame f)
    {
        int half = f.Height / 2;
        if (half < 2 || f.Height % 2 != 0 || (f.Width - 2) % (half - 1) != 0)
            throw new Exception(string.Format("not a diamond: {0}x{1}", f.Width, f.Height));
        int step = (f.Width - 2) / (half - 1);
        int p = f.Offset;
        for (int y = 0; y < f.Height; y++)
        {
            int k = y < half ? y : f.Height - 1 - y;
            int w = 2 + step * k;
            int x0 = (f.Width - w) / 2;
            if (p + w > b.Length) throw new Exception("diamond past end of file at y=" + y);
            for (int x = 0; x < w; x++)
            {
                f.Pixels[y * f.Width + x0 + x] = b[p++];
                f.Opaque[y * f.Width + x0 + x] = true;
            }
        }
    }

    static void Rows(byte[] b, Pl8Frame f)
    {
        int p = f.Offset;
        for (int y = 0; y < f.Height; y++)
        {
            int x = 0;
            while (x < f.Width)
            {
                int c = b[p++];
                if (c == 0)
                {
                    x += b[p++];
                }
                else if (c < 0x80)
                {
                    for (int k = 0; k < c; k++, x++)
                    {
                        if (x >= f.Width) throw new Exception("literal past row end at y=" + y);
                        f.Pixels[y * f.Width + x] = b[p++];
                        f.Opaque[y * f.Width + x] = true;
                    }
                }
                else
                {
                    throw new Exception(string.Format("unknown command {0:X2} at {1:X}", c, p - 1));
                }
            }
            if (x != f.Width) throw new Exception("row overran width at y=" + y);
        }
    }

    // Average colour step between neighbouring opaque pixels: the right palette gives the
    // smoothest picture, a wrong one gives noise. Used to find a picture's palette.
    public static double Roughness(Pl8Frame f, Color[] pal)
    {
        long sum = 0, n = 0;
        for (int y = 0; y < f.Height; y++)
            for (int x = 1; x < f.Width; x++)
            {
                int i = y * f.Width + x;
                if (!f.Opaque[i] || !f.Opaque[i - 1]) continue;
                Color a = pal[f.Pixels[i]], c = pal[f.Pixels[i - 1]];
                sum += Math.Abs(a.R - c.R) + Math.Abs(a.G - c.G) + Math.Abs(a.B - c.B);
                n++;
            }
        return n == 0 ? 0 : (double)sum / n;
    }

    // Saves a frame as 32-bit PNG; transparent pixels get alpha 0 (or the key colour if given).
    public static void SavePng(Pl8Frame f, Color[] pal, string path, bool keyMagenta)
    {
        if (f.Width == 0 || f.Height == 0) return;
        using (var bmp = new Bitmap(f.Width, f.Height, PixelFormat.Format32bppArgb))
        {
            var data = bmp.LockBits(new Rectangle(0, 0, f.Width, f.Height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            var row = new int[f.Width];
            for (int y = 0; y < f.Height; y++)
            {
                for (int x = 0; x < f.Width; x++)
                {
                    int i = y * f.Width + x;
                    if (f.Opaque[i]) row[x] = pal[f.Pixels[i]].ToArgb();
                    else row[x] = keyMagenta ? unchecked((int)0xFFFF00FF) : 0;
                }
                Marshal.Copy(row, 0, data.Scan0 + y * data.Stride, f.Width);
            }
            bmp.UnlockBits(data);
            bmp.Save(path, ImageFormat.Png);
        }
    }
}
