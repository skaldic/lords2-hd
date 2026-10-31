using System;
using System.IO;

// The Armoury as a wider room, from its own 2x painting (BGRX, 1280x960).
public static class Room
{
    const int M = 107, W = 640 + 2 * M, H = 480, CutL = 62, CutR = 578, FloorY = 366, BarY = 395;

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
        Directory.CreateDirectory(Path.GetDirectoryName(outBin));
        using (var f = new BinaryWriter(File.Create(outBin)))
        {
            f.Write(W); f.Write(CutL); f.Write(CutR); f.Write(FloorY); f.Write(BarY);
            f.Write(o);
        }
    }
}
