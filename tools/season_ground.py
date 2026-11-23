# Seasonal Town Center art: makes the overlays and animations of a community season set join that
# season's own base picture. Ground (smooth grass, bare earth) is found once in the SUMMER art, with the
# shadows on it as shading; for every season that ground is taken from the season's vill\00.png and the
# objects (pit, trees, woodcutter, logs, mine, forge) keep the artist's pixels.
# Places at 2x in the 726x640 picture, measured from the game's 1x frames: quarry 40 and mine field 43
# at (24,24), woods 41 at (216,460), woodcutter 07-14 at (358,522) on 41, smithy 25-32 at (32,350),
# quarry workers 00-06 at (198,54) on 40, iron mine (villani1) at (200,24) on 43.
# Then import each season's folder with tools\import_sprites.ps1 -Suffix _<season> (spring, summer,
# autumn, winter) into <game>\lords2hd\sprites; the patch picks them by season (season_name in hd\lords2hd.c).
# Needs Python with numpy, scipy and Pillow.
# With -game <game folder> also the small animations on the picture itself (the tent, villani2 15-24 at
# 1x 302,293; the forge fire, 33-39 at 50,195): where their 1x pixel is the picture's own (the game's
# lords2hd\sprites\vill.bin and villani2.bin), the HD pixel is taken from the season's picture; the
# quarry workers' pit floor likewise comes from the fitted quarry overlay.
# usage: season_ground.py summer_hd season_hd out_hd [mask_preview.png] [-game <game folder>]
import sys, os, shutil, struct
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

args = sys.argv[1:]
game = None
if '-game' in args:
    i = args.index('-game'); game = args[i + 1]; del args[i:i + 2]
summer, src, dst = args[:3]
prev_p = args[3] if len(args) > 3 else None

def png(root, set_, f): return np.asarray(Image.open(os.path.join(root, set_, '%02d.png' % f)).convert('RGBA'), float)
def lum(a): return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114

def classify(a, path_rows=None, path_cols=None, smooth_only=True):
    """ground mask and shade from a summer piece (RGBA); path (bare earth) counts as ground above
    row path_rows / left of column path_cols"""
    rgb = a[..., :3]; r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    L = lum(rgb)
    mx = rgb.max(2); mn = rgb.min(2); sat = (mx - mn) / np.maximum(mx, 1)
    m1 = ndimage.gaussian_filter(L, 2); m2 = ndimage.gaussian_filter(L * L, 2)
    tex = np.sqrt(np.maximum(m2 - m1 * m1, 0))
    green = (g > r * 1.0) & (g > b * 1.15) & (sat > 0.15)
    grass = green & (tex < 16) if smooth_only else green   # grass is smooth; leaves are speckled (no trees in the animations)
    path = (r > g) & (g > b) & (r - b > 35) & (L > 70) & (L < 200) & (sat > 0.2) & (tex < 18)
    if path_rows is not None: path &= np.arange(a.shape[0])[:, None] < path_rows
    if path_cols is not None: path &= np.arange(a.shape[1])[None, :] < path_cols
    gr = grass | path
    gr = ndimage.binary_opening(gr, iterations=1)
    obj = ~gr & (a[..., 3] > 0)
    obj = ndimage.binary_closing(obj, iterations=2)
    obj = ndimage.binary_opening(obj, iterations=2)    # specks of 'object' in the grass go
    obj = ndimage.binary_fill_holes(obj)
    gr = ~obj
    # shading: the summer ground's brightness against its smooth surroundings (shadows on grass)
    gl = np.where(gr, L, 0); gw = gr.astype(float)
    ref = ndimage.gaussian_filter(gl, 8) / np.maximum(ndimage.gaussian_filter(gw, 8), 1e-3)
    ref = ndimage.gaussian_filter(ndimage.maximum_filter(ref, 21), 4)
    shade = np.clip(ndimage.gaussian_filter(L, 1.5) / np.maximum(ref, 1), 0.4, 1.0)
    return obj, shade

def blend(art, obj, shade, under):
    soft = ndimage.gaussian_filter(obj.astype(float), 1.0)
    rgb = art[..., :3] * soft[..., None] + under * shade[..., None] * (1 - soft[..., None])
    return np.dstack([rgb, art[..., 3]])

def save(a, set_, f):
    os.makedirs(os.path.join(dst, set_), exist_ok=True)
    Image.fromarray(np.clip(a, 0, 255).astype(np.uint8), 'RGBA').save(os.path.join(dst, set_, '%02d.png' % f))

for d in os.listdir(src):
    if os.path.isdir(os.path.join(src, d)): shutil.copytree(os.path.join(src, d), os.path.join(dst, d), dirs_exist_ok=True)

base = png(src, 'vill', 0)[..., :3]
fixed, previews = {}, []
for f, (x, y) in ((40, (24, 24)), (41, (216, 460)), (43, (24, 24))):
    s = png(summer, 'villani2', f); art = png(src, 'villani2', f)
    h, w = s.shape[:2]
    obj, shade = classify(s, path_rows=0 if f == 40 else None, path_cols=170 if f == 43 else None)
    if f == 41: obj[:, w - 44:] = False; shade[:, w - 44:] = 1.0      # the castle wall: the picture's own
    if f == 43: obj[:, 248:] = False; shade[:, 248:] = 1.0             # the field's edge and wall
    out = blend(art, obj, shade, base[y:y + h, x:x + w])
    save(out, 'villani2', f); fixed[f] = (x, y, out[..., :3])
    previews.append(('overlay %d' % f, art, obj, shade, out))

def under_of(over):
    u = base.copy()
    if over is not None:
        x, y, o = fixed[over]; u[y:y + o.shape[0], x:x + o.shape[1]] = o
    return u

def anims(set_, frames, over, xy, path_rows=0):
    u = under_of(over)
    for f in frames:
        s = png(summer, set_, f); art = png(src, set_, f)
        h, w = s.shape[:2]; x, y = xy
        obj, shade = classify(s, path_rows=path_rows, smooth_only=False)
        out = blend(art, obj, shade, u[y:y + h, x:x + w])
        save(out, set_, f)
        if f == frames[1]: previews.append(('%s %02d' % (set_, f), art, obj, shade, out))

anims('villani2', range(7, 15), 41, (216 + 142, 460 + 62))   # woodcutter
anims('villani2', range(25, 33), None, (32, 350), path_rows=42)  # smithy: path and grass above the forge
anims('villani2', range(0, 7), 40, (198, 54))                # quarry workers, in the pit (1x 99,27)
anims('villani1', range(0, 21), 43, (200, 24))               # iron mine (1x 100,12)

def frames1x(name):
    b = open(os.path.join(game, 'lords2hd', 'sprites', name + '.bin'), 'rb').read()
    n = struct.unpack_from('<I', b)[0]; offs = struct.unpack_from('<%dI' % n, b, 4)
    out = {}
    for i, o in enumerate(offs):
        if o:
            w, h = struct.unpack_from('<HH', b, o)
            out[i] = np.frombuffer(b, np.uint8, w * h, o + 4).reshape(h, w)
    return out

if game:
    pic1, ani1 = frames1x('vill')[0], frames1x('villani2')
    for frames, (x, y) in ((range(15, 25), (302, 293)), (range(33, 40), (50, 195))):
        for f in frames:
            p1 = ani1[f]; h, w = p1.shape
            same = np.kron(p1 == pic1[y:y + h, x:x + w], np.ones((2, 2))).astype(bool)
            art = png(dst, 'villani2', f)
            art[..., :3][same] = base[2 * y:2 * y + 2 * h, 2 * x:2 * x + 2 * w][same]
            save(art, 'villani2', f)
    # the quarry workers (00-06, 1x 99,27) stand on the pit floor of the quarry overlay (40, 1x 12,12),
    # which is neither grass nor path: there the HD pixel comes from the fitted overlay
    q1, (qx, qy, q2) = ani1[40], fixed[40]
    for f in range(0, 7):
        p1 = ani1[f]; h, w = p1.shape; x, y = 99 - 12, 27 - 12
        same = np.kron(p1 == q1[y:y + h, x:x + w], np.ones((2, 2))).astype(bool)
        art = png(dst, 'villani2', f)
        art[..., :3][same] = q2[2 * y:2 * y + 2 * h, 2 * x:2 * x + 2 * w][same]
        save(art, 'villani2', f)
        if f == 1: print('quarry worker %02d: %d of %d 1x pixels are the pit\'s' % (f, (p1 == q1[y:y + h, x:x + w]).sum(), p1.size))

# The swinging quarry worker never had a head you could see (dark hair on dark rock, in the original and in
# every repaint): a head of dark hair with a little skin is painted between his shoulders, at its 1x place
# in each frame (centre x, y), and the art's stray pure-white specks go.
HEADS = {0: (9.0, 10.0), 1: (9.0, 9.5), 2: (9.5, 9.0), 3: (9.5, 9.5), 4: (9.0, 9.0), 5: (9.0, 9.5), 6: (8.5, 10.0)}
def paint_head(art, cx, cy):
    h, w = art.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w].astype(float)
    X, Y = 2 * cx + 1 - xx, 2 * cy + 1 - yy                  # (2x pixels)
    d = np.sqrt((X / 3.3) ** 2 + (Y / 3.6) ** 2)
    a = np.clip((1.25 - d) * 2.5, 0, 1)                       # soft edge
    sh = np.clip((1.6 - np.sqrt(((X + 1.2) / 3.6) ** 2 + ((Y + 1.5) / 3.9) ** 2)) * 1.5, 0, 1) * 0.45   # shadow down-right
    rgb = art[..., :3] * (1 - sh[..., None])
    hair = np.array([62, 40, 26]) + np.clip(0.6 - np.hypot(X - 1.2, Y - 1.4) / 3, 0, 1)[..., None] * np.array([70, 48, 30])
    skin = np.array([186, 132, 96])
    k = np.clip((-Y - 1.6) / 1.6, 0, 1)[..., None] * 0.8     # the nape of the neck, at the bottom
    head = hair * (1 - k) + skin * k
    art[..., :3] = rgb * (1 - a[..., None]) + head * a[..., None]
    return art

for f, (cx, cy) in HEADS.items():
    art = png(dst, 'villani2', f)
    white = (art[..., :3] >= 250).all(2)
    if white.any():
        den = ndimage.uniform_filter((~white).astype(float), 5)
        avg = np.dstack([ndimage.uniform_filter(np.where(white, 0, art[..., c]), 5) for c in range(3)]) / np.maximum(den, 1e-3)[..., None]
        art[..., :3][white] = avg[white]
    save(paint_head(art, cx, cy), 'villani2', f)

if prev_p:
    rows = []
    for lab, art, obj, shade, out in previews:
        h, w = art.shape[:2]; k = 1 if w > 200 else 3
        mk = art[..., :3] * (1 - 0.6 * obj[..., None]) + np.array([255, 0, 255]) * 0.6 * obj[..., None]
        sh = np.repeat((shade * 255)[..., None], 3, 2)
        rows.append((lab, [Image.fromarray(np.clip(t, 0, 255).astype(np.uint8)).resize((w * k, h * k), Image.NEAREST) for t in (art[..., :3], mk, sh, out[..., :3])]))
    W = max(sum(t.size[0] + 4 for t in ts) for _, ts in rows); H = sum(ts[0].size[1] + 14 for _, ts in rows)
    pv = Image.new('RGB', (W, H), 'black'); dr = ImageDraw.Draw(pv); y = 0
    for lab, ts in rows:
        dr.text((2, y), lab + ': artist | objects kept (magenta) | shading | fixed', fill='white'); x = 0
        for t in ts: pv.paste(t, (x, y + 12)); x += t.size[0] + 4
        y += ts[0].size[1] + 14
    pv.save(prev_p)
print('ok')
