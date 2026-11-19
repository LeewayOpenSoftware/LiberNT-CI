#!/usr/bin/env python3
# PROJECT:     LiberNT media tools
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Keep the 1 px window and taskbar borders unbroken where an opaque caption button or the Start tile overlaps them
# COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
#
# The caption image carries the outer border (top row, right column, rounded corner). Caption buttons placed flush to that
# edge are opaque and would paint over it, and the Start tile does the same to the taskbar top line. Instead of drawing the
# border a second time, the pixels of the button that lie on the border (and on the transparent corner of the caption) are made
# transparent so the caption or taskbar border shows through. 32-bit strips get alpha 0; 24-bit strips get the colour key
# (255,0,255) and need Transparent = True in the style. Idempotent. usage: border_continuity.py <bitmaps dir> [--check]
import os
import struct
import sys

KEY = (255, 0, 255)


class Bmp:
    def __init__(self, path):
        self.path = path
        self.d = bytearray(open(path, 'rb').read())
        d = self.d
        self.off = struct.unpack_from('<I', d, 10)[0]
        hdr = struct.unpack_from('<I', d, 14)[0]
        self.w, h, _, self.bpp, comp = struct.unpack_from('<iiHHI', d, 18)
        self.bottom_up = h > 0
        self.h = abs(h)
        if self.bpp == 32:
            if comp == 3:
                masks = struct.unpack_from('<IIII', d, 54)
                if masks != (0xFF0000, 0xFF00, 0xFF, 0xFF000000):
                    raise SystemExit('%s: unexpected channel masks %s' % (path, masks))
            elif comp != 0:
                raise SystemExit('%s: unsupported compression %d' % (path, comp))
        elif self.bpp != 24 or comp != 0:
            raise SystemExit('%s: unsupported format %d bpp comp %d' % (path, self.bpp, comp))
        self.stride = ((self.w * self.bpp + 31) // 32) * 4

    def at(self, x, y):
        row = (self.h - 1 - y) if self.bottom_up else y
        return self.off + row * self.stride + x * (self.bpp // 8)

    def get(self, x, y):
        o = self.at(x, y)
        b, g, r = self.d[o], self.d[o + 1], self.d[o + 2]
        a = self.d[o + 3] if self.bpp == 32 else 255
        return r, g, b, a

    def clear(self, x, y):
        o = self.at(x, y)
        if self.bpp == 32:
            self.d[o:o + 4] = b'\0\0\0\0'
        else:
            self.d[o:o + 3] = bytes((KEY[2], KEY[1], KEY[0]))

    def save(self):
        open(self.path, 'wb').write(self.d)


def corner_mask(caption, state_h, size=9):
    """True where the caption's top-right size x size block is not interior fill: colour key or border colour."""
    border = caption.get(caption.w - 1, state_h // 2)[:3]
    mask = set()
    for y in range(size):
        for x in range(size):
            px = caption.get(caption.w - size + x, y)
            if px[:3] == KEY or px[:3] == border:
                mask.add((x, y))
    return mask


def cut_buttons(d, variant, check):
    changed = []
    for name, caption_name, sh, rightmost in (
            ('CLOSEBUTTON', 'FRAMECAPTION', 32, True), ('MINBUTTON', None, 32, False),
            ('MAXBUTTON', None, 32, False), ('RESTOREBUTTON', None, 32, False),
            ('HELPBUTTON', None, 32, False), ('SMALLCLOSEBUTTON', 'SMALLFRAMECAPTION', 24, True)):
        path = os.path.join(d, '%s_%s.bmp' % (variant, name))
        b = Bmp(path)
        mask = set()
        if rightmost:
            cap = Bmp(os.path.join(d, '%s_%s.bmp' % (variant, caption_name)))
            mask = corner_mask(cap, sh)
        n = 0
        for s in range(b.h // sh):
            for y in range(sh):
                for x in range(b.w):
                    hit = y == 0 or (rightmost and (x == b.w - 1 or (x >= b.w - 9 and y < 9 and (x - (b.w - 9), y) in mask)))
                    if not hit:
                        continue
                    px = b.get(x, s * sh + y)
                    if (b.bpp == 32 and px[3] != 0) or (b.bpp == 24 and px[:3] != KEY):
                        b.clear(x, s * sh + y)
                        n += 1
        if n:
            changed.append((os.path.basename(path), n))
            if not check:
                b.save()
    return changed


def cut_start(d, check):
    b = Bmp(os.path.join(d, 'NORMAL_STARTBUTTON.bmp'))
    sh = b.h // 3
    n = 0
    for s in range(3):
        for x in range(b.w):
            if b.get(x, s * sh)[:3] != KEY:
                b.clear(x, s * sh)
                n += 1
    if n and not check:
        b.save()
    return [('NORMAL_STARTBUTTON.bmp', n)] if n else []


def main():
    d = sys.argv[1]
    check = '--check' in sys.argv
    out = cut_start(d, check)
    for v in ('NORMAL', 'LIGHT'):
        out += cut_buttons(d, v, check)
    for n, c in out:
        print('%-34s %5d pixels' % (n, c))
    print('%d files %s' % (len(out), 'would change' if check else 'changed'))
    return 1 if (check and out) else 0


sys.exit(main())
