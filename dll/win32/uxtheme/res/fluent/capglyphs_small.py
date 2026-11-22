#!/usr/bin/env python3
# PROJECT:     LiberNT uxtheme
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Add the small caption-glyph frames that Fluent UI System Icons has
#              no native size for: 10 px for all five glyphs, and 12 px for restore
#              and help. These frames are hand-pixeled on the grid (1 px solid
#              strokes) to match the Fluent glyph shapes at a size with no native
#              source, rather than resampled. Native frames are preserved
#              byte-for-byte; the step is idempotent.
# COPYRIGHT:   Copyright 2026 Leeway Open Software
import os, struct, sys
if os.environ.get("LIBERNT_REGEN_ART") != "1":
    sys.exit("refusing to run: set LIBERNT_REGEN_ART=1 to (re)write the derived small frames on purpose")

HERE = os.path.dirname(os.path.abspath(__file__))
SHADES = {"w": (255,255,255), "i": (160,160,160), "d": (105,105,105),
          "l": (0,0,0), "li": (146,146,146), "ld": (224,224,224)}

# Hand-pixeled masks. '#' = opaque, '.' = transparent. One grid per (glyph, size).
G = {}
G[("close",10)] = [
 "..........",".#.....#..","..#...#...","...#.#....","....#.....",
 "...#.#....","..#...#...",".#.....#..","..........",".........."]
G[("max",10)] = [
 "..........",".#######..",".#.....#..",".#.....#..",".#.....#..",
 ".#.....#..",".#.....#..",".#######..","..........",".........."]
G[("min",10)] = [
 "..........","..........","..........","..........",".#######..",
 "..........","..........","..........","..........",".........."]
G[("restore",10)] = [
 "..........","....####..","....#..#..","....#..#..",".#####.#..",
 ".#...#....",".#...#....",".#...#....",".#####....",".........."]
G[("help",10)] = [
 "..........","...###....","..#...#...","......#...",".....#....",
 "....#.....","....#.....","..........","....#.....",".........."]
G[("restore",12)] = [
 "............",".....#####..",".....#...#..",".....#...#..",".....#...#..",
 ".######..#..",".#....#.....",".#....#.....",".#....#.....",".#....#.....",
 ".######.....","............"]
G[("help",12)] = [
 "............","...####.....","..#....#....",".......#....","......#.....",
 ".....#......",".....#......",".....#......","............",".....#......",
 "............","............"]

NEW = {"close":[10], "max":[10], "min":[10], "restore":[12,10], "help":[12,10]}

def rows_from_grid(g, size):
    assert len(g) == size and all(len(r) == size for r in g), "grid not %dx%d" % (size, size)
    return [[255 if ch == "#" else 0 for ch in r] for r in g]

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

def main():
    for fam, targets in NEW.items():
        masks = {t: rows_from_grid(G[(fam,t)], t) for t in targets}
        for suf, color in SHADES.items():
            p = os.path.join(HERE, "cap_%s_%s.ico" % (fam, suf))
            # keep every native frame; drop and re-add only the sizes this tool owns
            frames = [(w, blob) for (w, blob) in parse_ico(p) if w not in targets]
            for t in targets:
                frames.append((t, dib(t, masks[t], color)))
            write_ico(p, frames)
    print("done")

if __name__ == "__main__":
    main()
