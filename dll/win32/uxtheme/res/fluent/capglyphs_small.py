#!/usr/bin/env python3
# PROJECT:     LiberNT uxtheme
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Add the small caption-glyph frames Fluent has no native size for
#              (10 px for all five glyphs; 12 px for restore and help), pixel-fit
#              from the nearest native frame already in each cap_*.ico. Existing
#              frames are preserved byte-for-byte; run is idempotent.
# COPYRIGHT:   Copyright 2026 Leeway Open Software
import os, struct, sys
if os.environ.get("LIBERNT_REGEN_ART") != "1":
    sys.exit("refusing to run: set LIBERNT_REGEN_ART=1 to add the derived small frames on purpose")
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
SHADES = {"w": (255,255,255), "i": (160,160,160), "d": (105,105,105),
          "l": (0,0,0), "li": (146,146,146), "ld": (224,224,224)}
REF = {"close":12, "max":12, "min":12, "restore":16, "help":16}   # nearest native to derive from
NEW = {"close":[10], "max":[10], "min":[10], "restore":[12,10], "help":[12,10]}

def dib(size, rows, color):
    r,g,b = color
    px = bytearray()
    for row in reversed(rows):
        for a in row:
            px += bytes((b,g,r,a))
    mask_stride = ((size+31)//32)*4
    header = struct.pack("<IiiHHIIiiII", 40, size, size*2, 1, 32, 0,
                         len(px)+mask_stride*size, 0,0,0,0)
    return header + bytes(px) + bytes(mask_stride*size)

def parse_ico(path):
    b = open(path,"rb").read()
    _,_,cnt = struct.unpack("<HHH", b[:6]); frames=[]
    for i in range(cnt):
        o=6+i*16; w,h,nc,rv,pl,bp,sz,io = struct.unpack("<BBBBHHII", b[o:o+16])
        frames.append(((w or 256), b[io:io+sz]))
    return frames

def write_ico(path, frames):
    frames = sorted(frames, key=lambda f: f[0])
    out = struct.pack("<HHH",0,1,len(frames)); offset = 6+16*len(frames); blobs=[]
    for size,blob in frames:
        out += struct.pack("<BBBBHHII", size%256, size%256, 0,0,1,32, len(blob), offset)
        offset += len(blob); blobs.append(blob)
    open(path,"wb").write(out + b"".join(blobs))

def alpha_rows(path, size):
    im = Image.open(path); im.size=(size,size); a = im.convert("RGBA").split()[3]; px=a.load()
    return [[px[x,y] for x in range(size)] for y in range(size)]

def derive(ref_rows, ref_size, target):
    img = Image.new("L",(ref_size,ref_size)); img.putdata([v for row in ref_rows for v in row])
    small = img.resize((target,target), Image.LANCZOS)
    small = small.filter(ImageFilter.UnsharpMask(radius=0.7, percent=140, threshold=0))
    px = small.load()
    return [[px[x,y] for x in range(target)] for y in range(target)]

def main():
    for fam, targets in NEW.items():
        ref = alpha_rows(os.path.join(HERE,"cap_%s_w.ico"%fam), REF[fam])
        masks = {t: derive(ref, REF[fam], t) for t in targets}
        for suf, color in SHADES.items():
            p = os.path.join(HERE, "cap_%s_%s.ico"%(fam,suf))
            frames = parse_ico(p); have = {w for w,_ in frames}
            for t in targets:
                if t not in have:
                    frames.append((t, dib(t, masks[t], color)))
            write_ico(p, frames)
    print("done")

if __name__ == "__main__":
    main()
