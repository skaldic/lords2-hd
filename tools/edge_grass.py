# Battle ground: the grass of tiles that are only part grass (forest edges, shores, rocks, stakes) from the HD plain-grass
# tiles (frames 0-15), shaded by the original tile's own grass colours (the forest's shadow stays). The upscaler reads
# each tile on its own and smoothed the grass of those tiles into a paler, flatter, blockier green than the plain grass
# beside them. Their other pixels (trees, water, rock, the shore line) keep their upscale. Run on a built .bin.
# usage: python -I edge_grass.py <bin in> <palette .256> <bin out> <preview.png> [frames a-b] [preview frames a,b,...]import struct, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFilter
src, palp, outp, prev = sys.argv[1:5]
d = bytearray(open(src, 'rb').read())
pr = open(palp, 'rb').read(); pal = np.array([[((pr[i*3+k] << 2) | (pr[i*3+k] >> 4)) for k in range(3)] for i in range(256)], np.float32)
n = struct.unpack_from('<i', d, 0)[0]; tab = struct.unpack_from('<%di' % n, d, 4)
def frame(i):
    o = tab[i]; w, h = struct.unpack_from('<HH', d, o)
    idx = np.frombuffer(bytes(d[o+4:o+4+w*h]), np.uint8).reshape(h, w)
    hd = np.frombuffer(bytes(d[o+4+w*h:o+4+w*h+16*w*h]), np.uint8).reshape(2*h, 2*w, 4)
    return o, w, h, idx, hd
def blur(a, s):   # separable gaussian, edges repeated (the tiles repeat, but their neighbours vary)
    if a.ndim == 3: return np.stack([blur(a[..., k], s) for k in range(a.shape[2])], -1)
    r = max(1, int(3 * s)); x = np.arange(-r, r + 1); k = np.exp(-x * x / (2 * s * s)); k /= k.sum()
    a = a.astype(np.float32)
    p = np.pad(a, ((0, 0), (r, r)), mode='edge'); a = sum(k[j] * p[:, j:j + a.shape[1]] for j in range(2 * r + 1))
    p = np.pad(a, ((r, r), (0, 0)), mode='edge'); a = sum(k[j] * p[j:j + a.shape[0], :] for j in range(2 * r + 1))
    return a.astype(np.float32)
grass = list(range(16))
gset = np.zeros(256, bool)
for g in grass: gset[frame(g)[3]] = True
first, last = (64, 123) if len(sys.argv) < 6 else map(int, sys.argv[5].split('-'))
changed = []; shots = []
for i in range(first, last + 1):
    if not tab[i]: continue
    o, w, h, idx, hd = frame(i)
    if w != 32 or h != 32 or (idx == 0).any(): continue
    m = gset[idx].astype(np.float32)
    share = m.mean()
    if share < 0.15 or share > 0.97: continue
    # tidy: lone grass pixels in the forest out, lone forest pixels in the grass in
    cnt = blur(m, 0.8)
    m = np.where(cnt > 0.6, 1.0, np.where(cnt < 0.3, 0.0, m))
    eq = [(frame(g)[3] == idx).sum() for g in grass]; g = grass[int(np.argmax(eq))]
    go, gw, gh, gidx, ghd = frame(g)
    orig = pal[idx]; gorig = pal[gidx]
    A = blur(orig * m[..., None], 2.0) / np.maximum(blur(m, 2.0)[..., None], 1e-3)   # the tile's grass colour round each pixel
    B = blur(gorig, 2.0)
    ratio = np.clip(A / np.maximum(B, 1.0), 0.4, 1.6)
    ratio2 = np.stack([np.asarray(Image.fromarray(ratio[..., k], 'F').resize((64, 64), Image.BILINEAR)) for k in range(3)], -1)
    m2 = np.asarray(Image.fromarray(m, 'F').resize((64, 64), Image.NEAREST), np.float32)
    alpha = np.clip(blur(m2, 1.0) * 1.4 - 0.2, 0, 1)[..., None]
    grassrgb = ghd[..., [2, 1, 0]].astype(np.float32) * ratio2
    cur = hd[..., [2, 1, 0]].astype(np.float32)
    new = np.clip(alpha * grassrgb + (1 - alpha) * cur, 0, 255).astype(np.uint8)
    out = hd.copy(); out[..., 2] = new[..., 0]; out[..., 1] = new[..., 1]; out[..., 0] = new[..., 2]
    d[o+4+w*h:o+4+w*h+16*w*h] = out.tobytes()
    changed.append(i); shots.append((i, cur.astype(np.uint8), new))
open(outp, 'wb').write(d)
print(len(changed), 'frames:', changed)
# preview: before / after, 3x
want = [int(v) for v in (sys.argv[6].split(',') if len(sys.argv) > 6 else '100,108,111,113,117,120,122'.split(','))]
sel = [s for s in shots if s[0] in want] or shots[:7]
W = 64 * 3
sheet = Image.new('RGB', (len(sel) * (W + 6), 2 * W + 30), (40, 0, 40)); dr = ImageDraw.Draw(sheet)
for k, (i, a, b) in enumerate(sel):
    sheet.paste(Image.fromarray(a).resize((W, W), Image.NEAREST), (k * (W + 6), 14))
    sheet.paste(Image.fromarray(b).resize((W, W), Image.NEAREST), (k * (W + 6), 14 + W + 6))
    dr.text((k * (W + 6) + 2, 1), str(i), fill=(255, 255, 255))
sheet.save(prev)
