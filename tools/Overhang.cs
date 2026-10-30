// Composes map tiles' overhang into pictures, from the layout dumped by the DLL.
using System; using System.Collections.Generic; using System.IO;
public static class Overhang {
    public const int W = 58, H = 66, Top = 36;
    static Dictionary<int, List<int[]>> layout = new Dictionary<int, List<int[]>>();
    public static void LoadLayout(string path) {
        foreach (var line in File.ReadLines(path)) {
            var p = line.Split(' ');
            int key = int.Parse(p[0]) * 100 + int.Parse(p[1]);
            List<int[]> l;
            if (!layout.TryGetValue(key, out l)) layout[key] = l = new List<int[]>();
            l.Add(new[] { int.Parse(p[2]), int.Parse(p[3]), int.Parse(p[4]) });
        }
    }
    static Pl8Frame New() {
        return new Pl8Frame { Width = W, Height = H, Pixels = new byte[W * H], Opaque = new bool[W * H] };
    }
    // Returns [context, overhang] per frame, both null when the frame has no overhang.
    public static Pl8Frame[] Compose(string pl8) {
        byte[] b = File.ReadAllBytes(pl8);
        int count = BitConverter.ToUInt16(b, 2);
        var o = new Pl8Frame[count * 2];
        for (int i = 0; i < count; i++) {
            int e = 8 + i * 16;
            int w = BitConverter.ToUInt16(b, e), h = BitConverter.ToUInt16(b, e + 2), off = BitConverter.ToInt32(b, e + 4);
            int type = b[e + 12], rows = b[e + 13];
            List<int[]> map;
            if (w != 58 || h != 30 || rows == 0 || !layout.TryGetValue(type * 100 + rows, out map)) continue;
            Pl8Frame ctx = New(), over = New();
            int p = off; // the diamond, as context
            for (int r = 0; r < 30; r++) {
                int k = r < 15 ? r : 29 - r, rw = 2 + 4 * k, x0 = (58 - rw) / 2;
                for (int x = 0; x < rw; x++, p++) { int q = (r + Top) * W + x0 + x; ctx.Pixels[q] = b[p]; ctx.Opaque[q] = true; }
            }
            foreach (var m in map) {
                int sp = off + 900 + m[0];
                if (sp >= b.Length || m[1] < 0 || m[1] >= W) continue;
                int y = m[2] + Top;
                if (y < 0 || y >= H || b[sp] == 0) continue;
                int q = y * W + m[1];
                ctx.Pixels[q] = over.Pixels[q] = b[sp];
                ctx.Opaque[q] = over.Opaque[q] = true;
            }
            o[2 * i] = ctx; o[2 * i + 1] = over;
        }
        return o;
    }
}
