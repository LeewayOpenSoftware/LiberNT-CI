#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build segmdl2.ttf (Segoe MDL2 Assets) and SegoeIcons.ttf (Segoe Fluent Icons).

Glyphs come from Fluent UI System Icons (MIT), placed at the Windows icon-font
code points listed in iconmap.csv. The caption buttons keep the hand-drawn
glyphs of ../segmdl2.ttx.

usage: mkiconfont.py FLUENT_FONTS_DIR OUTDIR

FLUENT_FONTS_DIR is fonts/ of the pinned fluentui-system-icons commit.
"""
import csv, hashlib, json, os, sys
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.scaleUpem import scale_upem
from fontTools.ttLib.tables._g_l_y_f import Glyph
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen

HERE = os.path.dirname(os.path.abspath(__file__))
UPEM = 2048
SIZES = (20, 16, 24, 28, 32, 12, 48, 10)
# Pinned input: microsoft/fluentui-system-icons fonts/ at this commit.
FLUENT_COMMIT = '6c2c9075cf8952ef48369b2475233b6e9761efed'
FLUENT_SHA256 = {
    'FluentSystemIcons-Regular.ttf': 'dcbc53cef37733e7b872c9909aec4d09dab7583879ccebc6c8574236280f1efb',
    'FluentSystemIcons-Regular.json': '0b2a831d13a0601e68298ee806971299e18837bb78815170f3464ed476be6ec6',
    'FluentSystemIcons-Filled.ttf': '4bca8bf545d00b7a12c226748d78fd483a77fa314c45dbaed1fedd56cb275eaf',
    'FluentSystemIcons-Filled.json': '8dd85fdb51650da93404583c12e0823cae4633bb252b8b2c7d6ea839840af101',
}
CAPTION = {0xE8BB: 'close', 0xE921: 'minimize', 0xE922: 'maximize', 0xE923: 'restore',
           0xEF2C: 'closeHC', 0xEF2D: 'minimizeHC', 0xEF2E: 'maximizeHC', 0xEF2F: 'restoreHC'}
FONTS = {
    'm': ('segmdl2.ttf', 'Segoe MDL2 Assets', 'SegoeMDL2Assets'),
    'f': ('SegoeIcons.ttf', 'Segoe Fluent Icons', 'SegoeFluentIcons'),
}


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def load_map():
    rows = []
    with open(os.path.join(HERE, 'iconmap.csv'), newline='', encoding='utf-8') as f:
        for r in csv.DictReader(f):
            if r['codepoint'].startswith('#'):
                continue
            rows.append(r)
    return rows


def caption_font():
    src = TTFont()
    src.importXML(os.path.join(HERE, '..', 'segmdl2.ttx'))
    scale_upem(src, UPEM)
    return src


class Fluent:
    def __init__(self, ttf, js):
        self.font = TTFont(ttf)
        self.gs = self.font.getGlyphSet()
        self.scale = UPEM / self.font['head'].unitsPerEm
        cmap = self.font.getBestCmap()
        with open(js, encoding='utf-8') as f:
            self.byname = {k: cmap[v] for k, v in json.load(f).items() if v in cmap}

    def find(self, base, style):
        for s in SIZES:
            n = 'ic_fluent_%s_%d_%s' % (base, s, style)
            if n in self.byname:
                return self.byname[n]
        return None

    def draw(self, name):
        pen = TTGlyphPen(None)
        self.gs[name].draw(TransformPen(pen, (self.scale, 0, 0, self.scale, 0, 0)))
        return pen.glyph()


def build(key, rows, fluent, cap, outdir):
    fname, family, psname = FONTS[key]
    font = caption_font()
    order = ['.notdef', 'space']
    glyf = font['glyf']
    hmtx = font['hmtx']
    glyphs = {'.notdef': glyf['.notdef'], 'space': glyf['space']}
    metrics = {'.notdef': (UPEM, 0), 'space': (UPEM // 4, 0)}
    cmap = {0x20: 'space'}
    used = {}
    for r in rows:
        if key not in r['fonts']:
            continue
        cp = int(r['codepoint'], 16)
        if cp in CAPTION:
            n = CAPTION[cp]
            if n not in glyphs:
                glyphs[n] = cap['glyf'][n]
                metrics[n] = (UPEM, cap['hmtx'][n][1])
                order.append(n)
            cmap[cp] = n
            continue
        if not r['source']:
            continue
        src = fluent[r['style']].find(r['source'], r['style'])
        if src is None:
            raise SystemExit('missing glyph %s %s for U+%04X' % (r['source'], r['style'], cp))
        if src not in used:
            g = fluent[r['style']].draw(src)
            n = 'uni%04X' % cp
            used[src] = n
            glyphs[n] = g
            g.recalcBounds(None)
            metrics[n] = (UPEM, getattr(g, 'xMin', 0))
            order.append(n)
        cmap[cp] = used[src]

    font.setGlyphOrder(order)
    glyf.glyphs = glyphs
    glyf.glyphOrder = order
    hmtx.metrics = metrics
    for t in list(font.keys()):
        if t in ('GSUB', 'GPOS', 'kern', 'DSIG', 'hdmx', 'LTSH', 'VDMX'):
            del font[t]
    cm = font['cmap']
    cm.tables = [st for st in cm.tables if st.format == 4]
    for st in cm.tables:
        st.cmap = dict(cmap)

    head = font['head']
    head.unitsPerEm = UPEM
    head.fontRevision = 1.0
    head.modified = head.created
    font.recalcTimestamp = False
    hhea = font['hhea']
    hhea.ascent, hhea.descent, hhea.lineGap = UPEM, 0, 0
    os2 = font['OS/2']
    os2.sTypoAscender, os2.sTypoDescender, os2.sTypoLineGap = UPEM, 0, 0
    os2.usWinAscent, os2.usWinDescent = UPEM, 0
    os2.fsType = 0
    os2.usFirstCharIndex = min(cmap)
    os2.usLastCharIndex = max(cmap)
    os2.xAvgCharWidth = UPEM
    post = font['post']
    post.formatType = 3.0
    post.extraNames = []
    post.mapping = {}

    name = font['name']
    name.names = []
    version = 'Version 1.0; Fluent UI System Icons %s' % FLUENT_COMMIT[:10]
    for nid, text in (
        (0, 'Glyphs: Copyright (c) 2020 Microsoft Corporation (Fluent UI System Icons, MIT). '
            'Caption buttons: Copyright (C) 2026 Ahmed ARIF (GPL-3.0-or-later).'),
        (1, family), (2, 'Regular'), (3, 'LiberNT %s 1.0' % family), (4, family),
        (5, version), (6, psname), (11, 'https://github.com/microsoft/fluentui-system-icons'),
        (13, 'GPL-3.0-or-later; the Fluent UI System Icons glyphs are also available under MIT.'),
        (14, 'https://www.gnu.org/licenses/gpl-3.0.html'),
    ):
        name.setName(text, nid, 3, 1, 0x409)
        name.setName(text, nid, 1, 0, 0)

    font['maxp'].recalc(font)
    out = os.path.join(outdir, fname)
    font.save(out)
    return out, len(cmap) - 1, len(order) - 2


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    for k, h in FLUENT_SHA256.items():
        if sha256(os.path.join(argv[1], k)) != h:
            raise SystemExit('%s: sha256 differs from the pinned file' % k)
    fluent = {s: Fluent(os.path.join(argv[1], 'FluentSystemIcons-%s.ttf' % s.title()),
                        os.path.join(argv[1], 'FluentSystemIcons-%s.json' % s.title()))
              for s in ('regular', 'filled')}
    rows = load_map()
    cap = caption_font()
    for key in ('m', 'f'):
        out, n_cp, n_gl = build(key, rows, fluent, cap, argv[2])
        print('%s: %d code points, %d glyphs' % (out, n_cp, n_gl))


if __name__ == '__main__':
    main(sys.argv)
