# HD video frames: every frame of a game video upscaled 2x ("AI 50/50": Real-ESRGAN x4plus blended half
# and half with a gentle de-dither + Lanczos, then the original's colour and brightness put back at
# large scale), stored as JPEGs in one file per video, <out>\<name>.bin, which the DLL draws over the
# playing video frame by frame (the .smk keeps playing for sound and timing).
#   python -I tools\build_videos.py <game folder> <out folder> <work folder> [names...]
# Needs PyAV, numpy, scipy, Pillow, and realesrgan-ncnn-vulkan (tools\tools_dir.ps1's folder\realesrgan).
#
# File: 8 DWORDs (magic "L2HV", version 1, width, height, frames, source width, source height, 0), then
# frames+1 DWORD offsets from the file start (frame i is offsets[i] .. offsets[i+1]), then the JPEGs.
import sys, os, glob, shutil, struct, subprocess, io
import numpy as np
import av
from PIL import Image
from scipy.ndimage import gaussian_filter

# The 500x312 event clips are already shown from their large disc version (h_SmackOpen) and gain
# little at 2x; LOM.SMK is the Lords of Magic advert. AXMEN.SMK is a copy of Axemen.smk.
SKIP = {"lom", "axmen", "axemen", "cart_brn", "cart_bsp", "cart_kgt", "hang", "jail", "pill_brn", "pill_cts",
        "pill_kgt", "win_game"}
QUALITY = 90

def frames_of(path):
    c = av.open(path)
    out = [f.to_ndarray(format="rgb24") for f in c.decode(video=0)]
    c.close()
    return out

def smk_head(path):
    with open(path, "rb") as f:
        h = struct.unpack("<6I", f.read(24))
    return h[1], h[2], h[3], h[5]  # width, height, frames, flags

def up(a, W, H, f):
    return np.asarray(Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).resize((W, H), f), np.float32)

def build(smk, outdir, work, esrgan, models):
    name = os.path.splitext(os.path.basename(smk))[0].lower()
    w, h, n, flags = smk_head(smk)
    if flags & 2:
        print(name, "interlaced, skipped"); return
    src = frames_of(smk)
    if len(src) != n:
        raise SystemExit("%s: decoded %d frames, header says %d" % (name, len(src), n))
    W, H = 2 * w, 2 * h
    tmp = os.path.join(work, name)
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(os.path.join(tmp, "src")); os.makedirs(os.path.join(tmp, "x4"))
    for i, f in enumerate(src):
        Image.fromarray(f).save(os.path.join(tmp, "src", "%05d.png" % i))
    r = subprocess.run([esrgan, "-i", os.path.join(tmp, "src"), "-o", os.path.join(tmp, "x4"), "-n", "realesrgan-x4plus",
                        "-s", "4", "-m", models, "-f", "png"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0:
        raise SystemExit("%s: realesrgan failed (%d)" % (name, r.returncode))
    src = [f.astype(np.float32) for f in src]
    blobs = []
    for i in range(n):
        a, b, c = src[max(i - 1, 0)], src[i], src[min(i + 1, n - 1)]
        # gentle de-dither: a 3-frame median where the picture stands still, a light blur, Lanczos, mild sharpening
        still = (np.abs(a - b).max(2, keepdims=True) < 24) & (np.abs(c - b).max(2, keepdims=True) < 24)
        f = np.where(still, np.median(np.stack([a, b, c]), 0), b)
        f = 0.6 * f + 0.4 * gaussian_filter(f, (0.7, 0.7, 0))
        det = up(f, W, H, Image.LANCZOS)
        det = det + 0.6 * (det - gaussian_filter(det, (1.2, 1.2, 0)))
        esr = np.asarray(Image.open(os.path.join(tmp, "x4", "%05d.png" % i)).convert("RGB").resize((W, H), Image.LANCZOS), np.float32)
        base = up(b, W, H, Image.BICUBIC)
        esr = esr + gaussian_filter(base, (3, 3, 0)) - gaussian_filter(esr, (3, 3, 0)) # the original's colour and levels
        o = np.clip(0.5 * esr + 0.5 * det, 0, 255).astype(np.uint8)
        bio = io.BytesIO()
        Image.fromarray(o).save(bio, "JPEG", quality=QUALITY, optimize=True)
        blobs.append(bio.getvalue())
    head = struct.pack("<8I", 0x5648324C, 1, W, H, n, w, h, 0)
    off = len(head) + 4 * (n + 1)
    offs = []
    for bl in blobs:
        offs.append(off); off += len(bl)
    offs.append(off)
    os.makedirs(outdir, exist_ok=True)
    tmpout = os.path.join(outdir, name + ".bin.part")
    with open(tmpout, "wb") as fo:
        fo.write(head); fo.write(struct.pack("<%dI" % (n + 1), *offs))
        for bl in blobs: fo.write(bl)
    os.replace(tmpout, os.path.join(outdir, name + ".bin"))
    shutil.rmtree(tmp, ignore_errors=True)
    print("%s: %dx%d -> %dx%d, %d frames, %.1f MB" % (name, w, h, W, H, n, off / 1048576), flush=True)

def main():
    game, outdir, work = sys.argv[1:4]
    only = {a.lower() for a in sys.argv[4:]}
    esrgan = os.environ.get("REALESRGAN", r"D:\Tools\realesrgan\realesrgan-ncnn-vulkan.exe")
    models = os.path.join(os.path.dirname(esrgan), "models")
    seen = set()
    for smk in sorted(glob.glob(os.path.join(game, "*.smk")) + glob.glob(os.path.join(game, "*.SMK"))):
        name = os.path.splitext(os.path.basename(smk))[0].lower()
        if name in seen or (only and name not in only) or (not only and name in SKIP): continue
        seen.add(name)
        if not only and os.path.exists(os.path.join(outdir, name + ".bin")):
            print(name, "done already"); continue
        build(smk, outdir, work, esrgan, models)

if __name__ == "__main__":
    main()
