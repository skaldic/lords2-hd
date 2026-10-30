# Builds 2x sprites. Transparent areas are filled with colours bled out from the sprite before
# upscaling, so edges stay clean; the shape comes back from the pixel-doubled original mask.
#   <game>\lords2hd\sprites\<file>.bin = u32 frames, then per frame u32 offset (0 = none), then per
#   frame at its offset: u16 w, u16 h, w*h bytes original indices (0 = transparent), 2w*2h BGRX
param([string[]]$Files = @('sprite1a','sprite1b','sprite2a','sprite2b','flags1a','flags2a'),
      [string]$Palette = "Base01.256", [string]$Model = 'realesrgan-x4plus-anime')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$game = Split-Path $root -Parent
$tools = & (Join-Path $PSScriptRoot "tools_dir.ps1")
$esr = Join-Path $tools "realesrgan\realesrgan-ncnn-vulkan.exe"
Add-Type -AssemblyName System.Drawing
$work = Join-Path $root "assets\sprites"; $in = Join-Path $work "in"; $up = Join-Path $work "x4"
$out = Join-Path $game "lords2hd\sprites"
Remove-Item $in, $up -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $in, $up, $out | Out-Null
$helper = Join-Path $work "Bleed.cs"
Set-Content $helper -Encoding ascii -Value @'
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
'@
Add-Type -Path (Join-Path $PSScriptRoot "Pl8.cs"), $helper -ReferencedAssemblies System.Drawing
$pal = [Pl8]::ReadPalette((Join-Path $game $Palette))
$all = @{}
foreach ($n in $Files) {
    $c = 0; $h = ""
    $f = [Pl8]::Read((Join-Path $game "$n.pl8"), [ref]$c, [ref]$h)
    $all[$n] = $f
    for ($i = 0; $i -lt $f.Length; $i++) {
        if (-not $f[$i].Error -and $f[$i].Width -ge 6 -and $f[$i].Height -ge 6) { [Bleed]::Save($f[$i], $pal, (Join-Path $in ("{0}_{1:D3}.png" -f $n, $i))) }
    }
}
"{0} sprites to upscale" -f (Get-ChildItem $in).Count
$ErrorActionPreference = 'Continue'
& $esr -i $in -o $up -n $Model -s 4 -f png -m (Join-Path $tools "realesrgan\models") 2>$null | Out-Null
$ErrorActionPreference = 'Stop'
if ((Get-ChildItem $up).Count -ne (Get-ChildItem $in).Count) { throw "the upscaler did not finish" }
foreach ($n in $Files) {
    $f = $all[$n]
    $s = [IO.File]::Create((Join-Path $out "$n.bin"))
    $s.Write([BitConverter]::GetBytes([int]$f.Length), 0, 4)
    $table = New-Object int[] $f.Length
    $s.Write((New-Object byte[] (4 * $f.Length)), 0, 4 * $f.Length)
    for ($i = 0; $i -lt $f.Length; $i++) {
        $u = Join-Path $up ("{0}_{1:D3}.png" -f $n, $i)
        if (Test-Path $u) { $table[$i] = [int]$s.Position; $b = [Bleed]::Frame($f[$i], $u); $s.Write($b, 0, $b.Length) }
    }
    $s.Position = 4
    foreach ($o in $table) { $s.Write([BitConverter]::GetBytes($o), 0, 4) }
    $s.Close()
    "{0}: {1:N1} MB" -f $n, ((Get-Item (Join-Path $out "$n.bin")).Length / 1MB)
}
