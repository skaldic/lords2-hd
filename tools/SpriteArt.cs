// HD sprite files (<game>\lords2hd\sprites\<name>.bin): u32 frames, u32 offset per frame (0 = none),
// at each offset u16 w, u16 h, w*h original indices (0 = transparent), 2w*2h BGRX.
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;

public static class SpriteArt
{
    public static byte[] FromPak(string pak, string name)
    {
        using (var fs = File.OpenRead(pak)) using (var br = new BinaryReader(fs)) {
            br.ReadUInt32(); uint n = br.ReadUInt32(); uint idx = br.ReadUInt32(); br.ReadUInt32();
            fs.Position = idx;
            for (int i = 0; i < n; i++) {
                string nm = System.Text.Encoding.ASCII.GetString(br.ReadBytes(48)).TrimEnd('\0');
                uint off = br.ReadUInt32(), size = br.ReadUInt32();
                if (string.Equals(nm, name, StringComparison.OrdinalIgnoreCase)) {
                    long keep = fs.Position; fs.Position = off; var b = br.ReadBytes((int)size); fs.Position = keep; return b;
                }
            }
        }
        throw new Exception(name + " not in " + pak);
    }

    public static int Frames(byte[] bin) { return BitConverter.ToInt32(bin, 0); }
    public static int Offset(byte[] bin, int i) { return BitConverter.ToInt32(bin, 4 + 4 * i); }

    // Frame i as a 32-bit PNG; alpha 0 only where the original index is 0 and transparent is true.
    public static void Export(byte[] bin, int i, string path, bool transparent)
    {
        int off = Offset(bin, i); if (off == 0) return;
        int w = BitConverter.ToUInt16(bin, off), h = BitConverter.ToUInt16(bin, off + 2);
        int ix = off + 4, px = ix + w * h, W = 2 * w, H = 2 * h;
        var b = new byte[W * H * 4];
        Buffer.BlockCopy(bin, px, b, 0, b.Length);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
            b[(y * W + x) * 4 + 3] = (byte)(transparent && bin[ix + (y >> 1) * w + (x >> 1)] == 0 ? 0 : 255);
        Save(b, W, H, path);
    }

    public static void Save(byte[] argb, int w, int h, string path)
    {
        using (var bm = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
            var d = bm.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            Marshal.Copy(argb, 0, d.Scan0, argb.Length); bm.UnlockBits(d);
            bm.Save(path, ImageFormat.Png);
        }
    }

    // Raw pixels, whatever DPI the file says.
    public static byte[] Load(string path, out int w, out int h)
    {
        using (var src = new Bitmap(path)) {
            w = src.Width; h = src.Height;
            var d = src.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var b = new byte[w * h * 4]; Marshal.Copy(d.Scan0, b, 0, b.Length); src.UnlockBits(d);
            return b;
        }
    }

    // Replaces frame i's HD pixels with the PNG, laid over the frame's current HD pixels by its alpha.
    // Returns an error text or null.
    public static string Import(byte[] bin, int i, string png)
    {
        int off = Offset(bin, i); if (off == 0) return "frame has no HD slot";
        int w = BitConverter.ToUInt16(bin, off), h = BitConverter.ToUInt16(bin, off + 2);
        int px = off + 4 + w * h, W = 2 * w, H = 2 * h, pw, ph;
        var b = Load(png, out pw, out ph);
        if (pw != W || ph != H) return string.Format("is {0}x{1}, must be {2}x{3}", pw, ph, W, H);
        for (int p = 0; p < W * H; p++) {
            int a = b[p * 4 + 3], o = px + p * 4;
            for (int c = 0; c < 3; c++) bin[o + c] = (byte)((b[p * 4 + c] * a + bin[o + c] * (255 - a) + 127) / 255);
            bin[o + 3] = 0;
        }
        return null;
    }
}
