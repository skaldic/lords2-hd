# Seasonal battlefields, art draft: spring, autumn and winter versions of the battle tile sheets
# (t32_bat1 the open field; t32_stn1/stn2 the stone castle, t32_wod1/wod2 the wooden castle: the
# exe's sheet table at 0x4da550, loader FUN_004987b7). Only the 2x pixels change; the frame table and
# the 1x indices stay byte for byte, so the game draws and stamps the tiles exactly as before.
# The recolour is one pure colour -> colour function, the same for every sheet, with no per-tile
# statistics and no filters: two pixels of one colour come out the same anywhere, so tiles still join
# each other and the open field's grass matches the castles' grass.
# The patch already picks lords2hd\sprites\<sheet>_<season>.bin by the season (season_name in
# hd\lords2hd.c) when Seasons is on, so DO NOT write these into a game's lords2hd\sprites folder
# before the art is approved: they would show in campaign battles at once.
# usage: season_battle.py <lords2hd.pak or a folder of t32_*.bin> <out folder> [-preview]
#   writes <out>\<sheet>_<season>.bin for spring, autumn, winter; -preview also writes
#   <out>\preview\<sheet>.png: the sheet in summer | spring | autumn | winter, side by side.
# Needs Python with numpy (and Pillow for -preview).
import sys, os, struct
import numpy as np

SHEETS = ['t32_bat1', 't32_stn1', 't32_stn2', 't32_wod1', 't32_wod2']
SEASONS = ['spring', 'autumn', 'winter']

def read_sheets(src):
    """{sheet: bytes} from a pak (v2: magic, count, index offset, version; 56-byte entries) or a folder"""
    out = {}
    if os.path.isdir(src):
        for n in SHEETS:
            p = os.path.join(src, n + '.bin')
            if os.path.exists(p): out[n] = open(p, 'rb').read()
        return out
    with open(src, 'rb') as f:
        _, count, index, version = struct.unpack('<4I', f.read(16))
        if version != 2: raise SystemExit('pak version %d, not 2' % version)
        f.seek(index)
        ents = {}
        for _ in range(count):
            e = f.read(56)
            ents[e[:48].split(b'\0')[0].decode()] = struct.unpack('<2I', e[48:])
        for n in SHEETS:
            if 'sprites/%s.bin' % n in ents:
                off, size = ents['sprites/%s.bin' % n]
                f.seek(off); out[n] = f.read(size)
    return out

def frames(b):
    """(frame, w, h, offset of the 2x pixels) of every frame in a sprite-format sheet"""
    count = struct.unpack_from('<I', b)[0]
    for i, o in enumerate(struct.unpack_from('<%dI' % count, b, 4)):
        if o:
            w, h = struct.unpack_from('<HH', b, o)
            yield i, w, h, o + 4 + w * h

def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)

def hsv(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx = rgb.max(-1); mn = rgb.min(-1); c = mx - mn
    h = np.zeros_like(mx)
    m = c > 1e-6
    rr = m & (mx == r); gg = m & (mx == g) & ~rr; bb = m & ~rr & ~gg
    h[rr] = ((g - b)[rr] / c[rr]) % 6
    h[gg] = (b - r)[gg] / c[gg] + 2
    h[bb] = (r - g)[bb] / c[bb] + 4
    return h * 60, np.where(mx > 0, c / np.maximum(mx, 1e-6), 0), mx

def from_hsv(h, s, v):
    h = (h % 360) / 60; c = v * s; x = c * (1 - np.abs(h % 2 - 1)); z = np.zeros_like(h)
    i = np.floor(h).astype(int) % 6
    r = np.choose(i, [c, x, z, z, x, c]); g = np.choose(i, [x, c, c, x, z, z]); b = np.choose(i, [z, z, x, c, c, x])
    return np.stack([r, g, b], -1) + (v - c)[..., None]

def luma(rgb): return rgb[..., 0] * 0.299 + rgb[..., 1] * 0.587 + rgb[..., 2] * 0.114

def weights(rgb):
    """soft masks from the colour alone, measured on t32_bat1: open grass is bright yellow-green (hue 50-94,
    v ~0.26), the woods' leaves dark blue-green (hue 112-148, v ~0.08), water teal (hue 153-171) and never
    darker than v ~0.19 - so dark blue-green is leaves, bright blue-green is water"""
    h, s, v = hsv(rgb)
    lit = smooth(0.006, 0.02, v)                                   # near-black has no hue to trust
    water = smooth(146, 153, h) * (1 - smooth(196, 210, h)) * smooth(0.13, 0.18, v) * smooth(0.40, 0.55, s)
    # from hue 44-52: the grass's yellow blade highlights are hue 50-58; thatch, roofs and stakes 24-46
    green = smooth(44, 52, h) * (1 - smooth(178, 192, h)) * smooth(0.12, 0.28, s) * lit * (1 - water)
    # 0 grass ... 1 woods: blue-green AND dark (the castles' grass is as blue in the walls' shadow, but lighter)
    leaf = smooth(98, 114, h) * (1 - smooth(0.11, 0.20, v))
    grey = 1 - smooth(0.10, 0.22, s)
    # thatch and light timber (roofs, stakes, bridge planks): bright orange-tan
    straw = smooth(18, 26, h) * (1 - smooth(50, 58, h)) * smooth(0.35, 0.55, s) * smooth(0.45, 0.70, v) * (1 - green)
    return h, s, v, green, water, grey, leaf, straw

def mix(a, b, w): return a * (1 - w[..., None]) + b * w[..., None]

def season_rgb(rgb, season):
    """rgb: float 0..1, (..., 3) -> the season's colours"""
    h, s, v, green, water, grey, leaf, straw = weights(rgb)
    L = luma(rgb)
    out = rgb.copy()
    if season == 'spring':
        # young green: lighter and more yellow on the grass, fresher and lighter in the woods
        nh = h + (84 - h) * 0.45 * (1 - leaf) + (104 - h) * 0.35 * leaf
        nv = np.clip(v * (1.14 + 0.16 * leaf) + 0.01, 0, 1)
        g2 = from_hsv(nh, np.clip(s * (0.97 + 0.10 * leaf), 0, 1), nv)
        out = mix(out, g2, green)
        out = mix(out, from_hsv(h - 4, s, np.clip(v * 1.05, 0, 1)), water)
    elif season == 'autumn':
        # grass: straw and olive; woods: orange to red, yellow where the sun is on them
        gh = 50 - (h - 80) * 0.45
        wh = 32 - (h - 112) * 0.55 + 16 * smooth(0.10, 0.30, v)
        nh = np.clip(gh * (1 - leaf) + wh * leaf, 6, 62)
        ns = np.clip(s * (0.70 * (1 - leaf)) + 0.88 * leaf, 0, 1)
        nv = np.clip(v * (1.04 * (1 - leaf) + 2.1 * leaf) + 0.025 * leaf, 0, 1)
        out = mix(out, from_hsv(nh, ns, nv), green)
        out = mix(out, from_hsv(h - 6, s * 0.72, v * 0.88), water)  # colder, duller water
    elif season == 'winter':
        # snow on the grass, its brightness following the grass's (its shading stays); the woods stay dark
        # and blue-grey under the trees, snow on the sunlit tops of the leaves
        cold = np.array([0.70, 0.77, 0.90]); white = np.array([0.95, 0.97, 1.0])
        # gain 1.1: more made the grass tiles' small differences in brightness show as bands in the snow
        k = np.clip(0.70 + 1.1 * (L - 0.12), 0.45, 1.0) * (1 - 0.12 * leaf)
        t = smooth(0.55, 0.95, k)[..., None]
        snow = k[..., None] * (cold * (1 - t) + white * t)
        shade = np.array([0.30, 0.36, 0.45]) * (0.55 + 2.2 * v)[..., None] + rgb * 0.25
        out = mix(out, mix(shade, snow, smooth(0.07, 0.22, v) * (1 - 0.25 * leaf)), green)
        out = mix(out, from_hsv(h + 8, s * 0.55, v * 0.80), water)  # dark grey winter water (not frozen)
        # stone, rock, wood and thatch: colder, with snow on the brightest (sunlit, upward) faces
        rest = (1 - green - water)[..., None]
        cool = rgb * np.array([0.94, 0.97, 1.03])
        dust = np.maximum(smooth(0.50, 0.80, L) * (0.35 + 0.35 * grey), 0.70 * straw)[..., None]
        out = out * (1 - rest) + (cool * (1 - dust) + white * dust) * rest
    return np.clip(out, 0, 1)

def recolour_sheet(b, season):
    out = bytearray(b)
    for _, w, h, p in frames(b):
        px = np.frombuffer(b, np.uint8, w * h * 16, p).reshape(2 * h, 2 * w, 4)
        rgb = px[..., 2::-1].astype(np.float32) / 255
        new = season_rgb(rgb, season)
        o = px.copy()
        o[..., 2::-1] = np.round(new * 255).astype(np.uint8)
        out[p:p + w * h * 16] = o.tobytes()
    return bytes(out)

def check_same_frames(a, b):
    """the frame table and the 1x indices must stay byte for byte"""
    assert len(a) == len(b)
    count = struct.unpack_from('<I', a)[0]
    assert a[:4 + 4 * count] == b[:4 + 4 * count]
    for _, w, h, p in frames(a):
        assert a[p - w * h - 4:p] == b[p - w * h - 4:p]

def sheet_png(b):
    from PIL import Image
    tiles = []
    for _, w, h, p in frames(b):
        px = np.frombuffer(b, np.uint8, w * h * 16, p).reshape(2 * h, 2 * w, 4)
        idx = np.frombuffer(b, np.uint8, w * h, p - w * h).reshape(h, w)
        rgb = px[..., 2::-1].copy()
        rgb[~np.kron(idx != 0, np.ones((2, 2), bool))] = 40
        tiles.append(rgb)
    cols = 16; cw = max(t.shape[1] for t in tiles); ch = max(t.shape[0] for t in tiles)
    sheet = np.full(((len(tiles) + cols - 1) // cols * ch, cols * cw, 3), 40, np.uint8)
    for k, t in enumerate(tiles):
        y, x = k // cols * ch, k % cols * cw
        sheet[y:y + t.shape[0], x:x + t.shape[1]] = t
    return Image.fromarray(sheet)

def main():
    args = sys.argv[1:]
    preview = '-preview' in args
    args = [a for a in args if a != '-preview']
    src, dst = args[:2]
    os.makedirs(dst, exist_ok=True)
    sheets = read_sheets(src)
    missing = [n for n in SHEETS if n not in sheets]
    if missing: print('not found: ' + ', '.join(missing))
    for n, b in sheets.items():
        made = {'summer': b}
        for season in SEASONS:
            out = recolour_sheet(b, season)
            check_same_frames(b, out)
            with open(os.path.join(dst, '%s_%s.bin' % (n, season)), 'wb') as f: f.write(out)
            made[season] = out
        print('%s: %d bytes x %d seasons' % (n, len(b), len(SEASONS)))
        if preview:
            from PIL import Image
            os.makedirs(os.path.join(dst, 'preview'), exist_ok=True)
            ims = [sheet_png(made[s]) for s in ('summer', 'spring', 'autumn', 'winter')]
            W = sum(i.width for i in ims) + 8 * 3
            pv = Image.new('RGB', (W, ims[0].height), (20, 20, 20)); x = 0
            for i in ims: pv.paste(i, (x, 0)); x += i.width + 8
            pv.save(os.path.join(dst, 'preview', n + '.png'))

if __name__ == '__main__':
    main()
