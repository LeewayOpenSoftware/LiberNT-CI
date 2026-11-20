#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build segmdl2.ttf (Segoe MDL2 Assets) and SegoeIcons.ttf (Segoe Fluent Icons).

Glyphs come from Fluent UI System Icons (MIT), placed at the Windows icon-font
code points listed in iconmap.csv.

usage: mkiconfont.py FLUENT_FONTS_DIR OUTDIR

FLUENT_FONTS_DIR is fonts/ of the pinned fluentui-system-icons commit.
"""
import csv, hashlib, json, os, sys
from fontTools.ttLib import TTFont
from fontTools.fontBuilder import FontBuilder
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
FONTS = {
    'm': ('segmdl2.ttf', 'Segoe MDL2 Assets', 'SegoeMDL2Assets'),
    'f': ('SegoeIcons.ttf', 'Segoe Fluent Icons', 'SegoeFluentIcons'),
}
# 2026-11-20 00:00:00 UTC in seconds since 1904-01-01, for a reproducible head table.
TIMESTAMP = 3846528000


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.sha256(f.read()).hexdigest()


def load_map():
    with open(os.path.join(HERE, 'iconmap.csv'), newline='', encoding='utf-8') as f:
        return list(csv.DictReader(l for l in f if not l.startswith('#')))


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


def notdef():
    pen = TTGlyphPen(None)
    for (a, b, c, d) in ((256, 0, 1792, 2048), (384, 128, 1664, 1920)):
        pts = [(a, b), (a, d), (c, d), (c, b)] if a == 256 else [(a, b), (c, b), (c, d), (a, d)]
        pen.moveTo(pts[0])
        for p in pts[1:]:
            pen.lineTo(p)
        pen.closePath()
    return pen.glyph()


def build(key, rows, fluent, outdir):
    fname, family, psname = FONTS[key]
    order = ['.notdef', 'space']
    glyphs = {'.notdef': notdef(), 'space': TTGlyphPen(None).glyph()}
    cmap = {0x20: 'space'}
    used = {}
    for r in rows:
        if key not in r['fonts'] or not r['source']:
            continue
        cp = int(r['codepoint'], 16)
        src = fluent[r['style']].find(r['source'], r['style'])
        if src is None:
            raise SystemExit('missing glyph %s %s for U+%04X' % (r['source'], r['style'], cp))
        if (src, r['style']) not in used:
            n = 'uni%04X' % cp
            used[(src, r['style'])] = n
            glyphs[n] = fluent[r['style']].draw(src)
            order.append(n)
        cmap[cp] = used[(src, r['style'])]

    fb = FontBuilder(UPEM, isTTF=True)
    fb.setupGlyphOrder(order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyphs)
    metrics = {}
    for n in order:
        g = glyphs[n]
        g.recalcBounds(fb.font['glyf'])
        metrics[n] = (UPEM // 4 if n == 'space' else UPEM, getattr(g, 'xMin', 0))
    fb.setupHorizontalMetrics(metrics)
    fb.setupHorizontalHeader(ascent=UPEM, descent=0, lineGap=0)
    version = 'Version 1.0; Fluent UI System Icons %s' % FLUENT_COMMIT[:10]
    fb.setupNameTable({
        'copyright': 'Glyphs: Copyright (c) 2020 Microsoft Corporation (Fluent UI System Icons).',
        'familyName': family, 'styleName': 'Regular',
        'uniqueFontIdentifier': 'LiberNT %s 1.0' % family,
        'fullName': '%s (Leeway replacement)' % family,
        'description': '%s (Leeway replacement)' % family,
        'version': version, 'psName': psname,
        'vendorURL': 'https://github.com/microsoft/fluentui-system-icons',
        'licenseDescription': 'MIT License',
        'licenseInfoURL': 'https://opensource.org/licenses/MIT',
    })
    fb.setupOS2(sTypoAscender=UPEM, sTypoDescender=0, sTypoLineGap=0,
                usWinAscent=UPEM, usWinDescent=0, fsType=0, achVendID='LBNT',
                xAvgCharWidth=UPEM, ulCodePageRange1=1)
    fb.setupPost(keepGlyphNames=False)
    fb.setupHead(unitsPerEm=UPEM, fontRevision=1.0, created=TIMESTAMP, modified=TIMESTAMP)
    font = fb.font
    font.recalcTimestamp = False
    os2 = font['OS/2']
    os2.recalcUnicodeRanges(font)
    os2.setUnicodeRanges(os2.getUnicodeRanges() | {60})
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
    for key in ('m', 'f'):
        out, n_cp, n_gl = build(key, rows, fluent, argv[2])
        print('%s: %d code points, %d glyphs' % (out, n_cp, n_gl))


if __name__ == '__main__':
    main(sys.argv)
