#!/usr/bin/env python3
# Rasterise the Aureline ribbon SVG masters to multi-size .ico strips (G10).
# Build-time only: uses the pinned resvg CLI (see tools/README.md) to render PNG
# frames at 16/24/32, then assembles PNG-framed .ico (Vista+). Produces a colour
# .ico and a .mono.ico per slug. Commit the generated .ico and this script; do NOT
# commit resvg or the intermediate PNGs.
#
# Usage: RESVG=/path/to/resvg.exe python gen-aureline-ico.py [MASTERS_DIR] [OUT_DIR]
#   MASTERS_DIR default: ../../../../shell/appframework/icons (hive shared)
#   OUT_DIR     default: ../icons   (sdk/lib/leewayui/icons/<app>/)
import os, sys, re, struct, subprocess, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
RESVG = os.environ.get("RESVG", "resvg")
MASTERS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    HERE, "..", "..", "..", "..", "shell", "appframework", "icons")
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "..", "icons")
APPS = ["common", "paint", "viewer", "wordpad"]
SIZES = [16, 24, 32]

def slugs(app):
    d = os.path.join(MASTERS, app); s = set()
    if os.path.isdir(d):
        for f in os.listdir(d):
            m = re.match(r'aur\.' + app + r'\.([a-z0-9.-]+)@(?:16|32|mono)\.svg$', f)
            if m: s.add(m.group(1))
    return sorted(s)

def src_svg(app, slug, size, mono):
    base = os.path.join(MASTERS, app)
    if mono:
        return os.path.join(base, f"aur.{app}.{slug}@mono.svg")
    s16 = os.path.join(base, f"aur.{app}.{slug}@16.svg")
    s32 = os.path.join(base, f"aur.{app}.{slug}@32.svg")
    if size <= 16 and os.path.exists(s16): return s16
    return s32 if os.path.exists(s32) else s16

def raster(svg, size, tmp):
    png = os.path.join(tmp, f"{size}.png")
    subprocess.run([RESVG, "--width", str(size), "--height", str(size), svg, png],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return open(png, "rb").read()

def make_ico(frames, out_ico):   # frames: [(size, png_bytes)]
    n = len(frames); entries = b""; payloads = b""; off = 6 + 16 * n
    for size, data in sorted(frames):
        w = size if size < 256 else 0
        entries += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(data), off)
        payloads += data; off += len(data)
    os.makedirs(os.path.dirname(out_ico), exist_ok=True)
    with open(out_ico, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, n)); f.write(entries); f.write(payloads)

def main():
    total = 0
    with tempfile.TemporaryDirectory() as tmp:
        for app in APPS:
            for slug in slugs(app):
                for mono in (False, True):
                    svg_ok = True; frames = []
                    for size in SIZES:
                        svg = src_svg(app, slug, size, mono)
                        if not os.path.exists(svg): svg_ok = False; break
                        frames.append((size, raster(svg, size, tmp)))
                    if not svg_ok or not frames: continue
                    name = f"aur.{app}.{slug}" + (".mono.ico" if mono else ".ico")
                    make_ico(frames, os.path.join(OUT, app, name))
                    total += 1
    print(f"wrote {total} .ico into {os.path.abspath(OUT)}")

if __name__ == "__main__":
    main()
