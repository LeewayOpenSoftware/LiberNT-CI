#!/usr/bin/env python3
# PROJECT:     LiberNT media tools
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Convert a LiberNT version-3 visual style (INI text + BMP art) into the resources of a version-4 style
# COPYRIGHT:   Copyright 2026 LeewayOpenSoftware
#
# Input  : textfiles/NormalNormal.INI and the bitmaps/ directory of a v3 style, plus the property/part/state tables
#          of uxtheme (stylemap.c) and the public headers vsstyle.h / vssym32.h.
# Output : <out>/ cmap.bin vmap.bin bcmap.bin variant.bin rmap.bin img/<id>.png and <name>.rc to compile into the
#          resource-only module. Only LiberNT sources are read; no Windows file is an input.
import argparse, os, re, struct, sys, zlib

TYPES = dict(ENUM=200, STRING=201, INT=202, BOOL=203, COLOR=204, MARGINS=205, FILENAME=206, SIZE=207,
             POSITION=208, RECT=209, FONT=210, INTLIST=211)
FIRST_FONT_STRING = 496
IMAGE_BASE = 508


# ---- symbols from the public headers ---------------------------------------------------------------------------
def load_symbols(paths):
    sym = {'TMT_ENUM': 200, 'TMT_STOCKIMAGEFILE': 3007}
    for p in paths:
        txt = open(p, encoding='utf-8', errors='replace').read()
        txt = re.sub(r'/\*.*?\*/', '', txt, flags=re.S)
        txt = re.sub(r'//[^\n]*', '', txt)
        for m in re.finditer(r'^\s*#\s*define\s+(\w+)\s+(.+)$', txt, flags=re.M):
            name, val = m.group(1), m.group(2).strip()
            v = ev(val, sym)
            if v is not None:
                sym[name] = v
        for m in re.finditer(r'enum\s+\w*\s*\{(.*?)\}', txt, flags=re.S):
            cur = -1
            for item in m.group(1).split(','):
                item = item.strip()
                if not item:
                    continue
                if '=' in item:
                    n, v = [x.strip() for x in item.split('=', 1)]
                    val = ev(v, sym)
                    if val is None:
                        continue
                    cur = val
                else:
                    n = item
                    cur += 1
                if re.fullmatch(r'\w+', n):
                    sym[n] = cur
    return sym


def ev(expr, sym):
    expr = expr.strip()
    if not expr:
        return None
    e = re.sub(r'\b0x([0-9a-fA-F]+)[uUlL]*\b', lambda m: str(int(m.group(1), 16)), expr)
    e = re.sub(r'\b(\d+)[uUlL]+\b', r'\1', e)
    toks = re.findall(r'[A-Za-z_]\w*', e)
    for t in toks:
        if t not in sym:
            return None
        e = re.sub(r'\b%s\b' % t, str(sym[t]), e)
    if not re.fullmatch(r'[\d\s()+\-*/|&<>~]+', e):
        return None
    try:
        return int(eval(e, {'__builtins__': {}}))
    except Exception:
        return None


# ---- uxtheme tables --------------------------------------------------------------------------------------------
def load_stylemap(path, sym):
    txt = open(path, encoding='utf-8', errors='replace').read()
    props = {}
    m = re.search(r'mapProperty\[\]\s*=\s*\{(.*?)\n\};', txt, flags=re.S)
    for e in re.finditer(r'\{L"(\w+)",\s*(\w+),\s*(\w+)\}', m.group(1)):
        t, i = sym.get(e.group(2)), sym.get(e.group(3))
        if t is None or i is None:
            raise SystemExit('unresolved property symbol %s/%s' % (e.group(2), e.group(3)))
        props[e.group(1).upper()] = (t, i)
    enums = []
    m = re.search(r'mapEnum\[\]\s*=\s*\{(.*?)\n\};', txt, flags=re.S)
    for e in re.finditer(r'\{(\w+),\s*(\w+),\s*L"(\w+)"\}', m.group(1)):
        a, b = sym.get(e.group(1)), sym.get(e.group(2))
        if a is None or b is None:
            continue
        enums.append((a, b, e.group(3).upper()))
    classes = {}
    for m in re.finditer(r'static const MSSTYLES_CLASS_MAP (\w+)\[\]\s*=\s*\{(.*?)\n\};', txt, flags=re.S):
        rows = []
        for e in re.finditer(r'\{(\w+),\s*(\w+|0),\s*L"(\w*)"\}', m.group(2)):
            p, s = sym.get(e.group(1), 0 if e.group(1) == '0' else None), sym.get(e.group(2), 0 if e.group(2) == '0' else None)
            if p is None or s is None or not e.group(3):
                continue
            rows.append((p, s, e.group(3).upper()))
        classes[m.group(1)] = rows
    cmap = {}
    m = re.search(r'mapClass\[\]\s*=\s*\{(.*?)\n\};', txt, flags=re.S)
    for e in re.finditer(r'\{(\w+),\s*L"(\w+)"\}', m.group(1)):
        cmap[e.group(2)] = classes[e.group(1)]
    return props, enums, cmap


def lookup_part_state(cmap, cls, part, state):
    rows = cmap.get(cls.upper())
    if rows is None:
        return (0, 0) if not part and not state else None
    pid = sid = 0
    if part:
        for p, s, n in rows:
            if s == 0 and n == part.upper():
                pid = p
                break
        if not pid:
            return None
    if state:
        for p, s, n in rows:
            if (p == pid or not part) and n == state.upper() and s != 0:
                sid = s
                break
        if not sid:
            return None
    return pid, sid


# ---- INI -------------------------------------------------------------------------------------------------------
def parse_ini(path):
    sections = []
    cur = None
    for line in open(path, encoding='utf-8-sig', errors='replace'):
        line = line.rstrip('\r\n')
        s = line.strip()
        if not s or s.startswith(';'):
            continue
        if s.startswith('[') and s.endswith(']'):
            cur = (s[1:-1], [])
            sections.append(cur)
        elif cur is not None and '=' in s:
            k, v = s.split('=', 1)
            cur[1].append((k.strip(), v.strip()))
    return sections


def split_section(name):
    app = ''
    rest = name
    if '::' in rest:
        app, rest = rest.split('::', 1)
    part = state = ''
    cls = rest
    if '.' in rest:
        cls, rest2 = rest.split('.', 1)
        if '(' in rest2:
            part, st = rest2.split('(', 1)
            state = st.rstrip(')')
        else:
            part = rest2
    elif '(' in rest:
        cls, st = rest.split('(', 1)
        state = st.rstrip(')')
    return app, cls, part, state


def ints(text):
    return [int(x) for x in re.findall(r'-?\d+', text)]


# ---- images ----------------------------------------------------------------------------------------------------
def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    hs = struct.unpack_from('<I', d, 14)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    planes, bpp, comp = struct.unpack_from('<HHI', d, 26)
    ncol = struct.unpack_from('<I', d, 46)[0]
    top_down = h < 0
    h = abs(h)
    masks = None
    if comp == 3:
        if hs >= 52:
            masks = struct.unpack_from('<4I', d, 54) if hs >= 56 else struct.unpack_from('<3I', d, 54) + (0,)
        else:
            masks = struct.unpack_from('<3I', d, 54) + (0,)
    elif comp != 0:
        raise ValueError('%s: compression %d' % (path, comp))
    pal = []
    if bpp <= 8:
        n = ncol or (1 << bpp)
        for i in range(n):
            b, g, r, _ = struct.unpack_from('<4B', d, 14 + hs + 4 * i)
            pal.append((r, g, b))
    stride = ((w * bpp + 31) // 32) * 4
    rows = []
    for y in range(h):
        sy = y if top_down else h - 1 - y
        row = d[off + sy * stride: off + (sy + 1) * stride]
        px = []
        for x in range(w):
            if bpp == 24:
                b, g, r = row[3 * x], row[3 * x + 1], row[3 * x + 2]
                px.append((r, g, b, 255))
            elif bpp == 32:
                v = struct.unpack_from('<I', row, 4 * x)[0]
                if masks:
                    def ch(m):
                        if not m:
                            return 255
                        sh = (m & -m).bit_length() - 1
                        return ((v & m) >> sh) * 255 // (m >> sh)
                    px.append((ch(masks[0]), ch(masks[1]), ch(masks[2]), ch(masks[3]) if masks[3] else 255))
                else:
                    px.append((row[4 * x + 2], row[4 * x + 1], row[4 * x], row[4 * x + 3]))
            elif bpp == 8:
                r, g, b = pal[row[x]]
                px.append((r, g, b, 255))
            elif bpp == 4:
                byte = row[x // 2]
                idx = (byte >> 4) if x % 2 == 0 else (byte & 15)
                r, g, b = pal[idx]
                px.append((r, g, b, 255))
            else:
                raise ValueError('%s: bpp %d' % (path, bpp))
        rows.append(px)
    return w, h, rows


def write_png(path, w, h, rows):
    has_alpha = any(p[3] != 255 for r in rows for p in r)
    raw = bytearray()
    for r in rows:
        raw.append(0)
        for p in r:
            raw += bytes(p[:4] if has_alpha else p[:3])
    def chunk(t, data):
        c = struct.pack('>I', len(data)) + t + data
        return c + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6 if has_alpha else 2, 0, 0, 0)) + \
        chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)


# ---- v4 blobs --------------------------------------------------------------------------------------------------
def u16z(s):
    return s.encode('utf-16-le') + b'\0\0'


def pad(b, n):
    return b + b'\0' * ((-len(b)) % n)


def build_cmap(names):
    return b''.join(pad(u16z(n), 8) for n in names)


def build_vmap(variant, size, color):
    out = b''
    for s in (variant, size, color):
        out += pad(struct.pack('<I', len(s) + 1) + u16z(s), 4)
    return out


def record(name_id, type_id, cls, part, state, ref, data, logical=None):
    cb = len(data) if logical is None else logical
    h = struct.pack('<8I', name_id, type_id, cls, part, state, ref, 0, cb)
    return pad(h + (data if ref == 0 else b''), 8)


KNOWN_ABSENT = ('normal/ie_personalbarmenu.bmp', 'normal/normalgroupcollapse.bmp', 'normal/normalgroupexpand.bmp')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ini', required=True)
    ap.add_argument('--bitmaps', required=True)
    ap.add_argument('--stylemap', required=True)
    ap.add_argument('--headers', nargs='+', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--name', default='liberntv4')
    ap.add_argument('--display', default='LiberNT')
    ap.add_argument('--description', default='')
    ap.add_argument('--basic', action='store_true', help='switch every Liquid (composited material) class off')
    ap.add_argument('--reactos-version', action='store_true')
    a = ap.parse_args()

    sym = load_symbols(a.headers)
    props, enums, cmap_tab = load_stylemap(a.stylemap, sym)
    sections = parse_ini(a.ini)
    os.makedirs(os.path.join(a.out, 'img'), exist_ok=True)

    bmps = {f.upper(): f for f in os.listdir(a.bitmaps)}
    # win8.rc ids drop underscores that the file names keep (NORMAL_FLUENTSMALLCLOSEGLYPH12_BMP vs NORMAL_FLUENT_SMALLCLOSEGLYPH12.bmp)
    loose = {}
    for k in bmps:
        loose.setdefault(k.replace('_', ''), []).append(k)
    classes, order = {}, []
    stats = dict(unknown_prop=0, unknown_section=0, unresolved_image=0, bad_value=0, records=0)

    def cls_ordinal(name):
        if name not in classes:
            classes[name] = {}
            order.append(name)
        return classes[name]

    images, fonts = {}, {}
    missing = []
    next_img = [IMAGE_BASE]

    def image_id(fname):
        key = fname.upper().replace('\\', '_').replace('/', '_')
        if key.endswith('.BMP'):
            key = key[:-4]
        c = key + '.BMP'
        if c not in bmps:
            hits = loose.get(c.replace('_', ''), [])
            if len(hits) != 1:
                return None
            c = hits[0]
        if c not in images:
            images[c] = next_img[0]
            next_img[0] += 1
        return images[c]

    def font_id(text):
        if text not in fonts:
            fonts[text] = FIRST_FONT_STRING + len(fonts)
        return fonts[text]

    def enum_value(prop_id, text):
        t = text.strip().upper()
        for e, v, n in enums:
            if e == prop_id and n == t:
                return v
        return None

    def encode(ptype, pid, text, ps):
        """-> (type, reference, data, logical) or None"""
        if ptype == TYPES['ENUM']:
            v = enum_value(pid, text)
            if v is None:
                return None
            return ptype, 0, struct.pack('<i', v), None
        if ptype == TYPES['STRING']:
            return ptype, 0, u16z(text), None
        if ptype in (TYPES['INT'], TYPES['SIZE']):
            v = ints(text)
            if not v:
                return None
            return ptype, 0, struct.pack('<i', v[0]), None
        if ptype == TYPES['BOOL']:
            t = text.strip().lower()
            return ptype, 0, struct.pack('<i', 1 if t in ('true', '1', 'yes') else 0), None
        if ptype == TYPES['COLOR']:
            v = ints(text)
            if len(v) < 3:
                return None
            return ptype, 0, struct.pack('<i', (v[2] & 255) << 16 | (v[1] & 255) << 8 | (v[0] & 255)), None
        if ptype in (TYPES['MARGINS'], TYPES['RECT']):
            v = ints(text)
            if len(v) != 4:
                return None
            return ptype, 0, struct.pack('<4i', *v), None
        if ptype == TYPES['POSITION']:
            v = ints(text)
            if len(v) != 2:
                return None
            return ptype, 0, struct.pack('<2i', *v), None
        if ptype == TYPES['INTLIST']:
            v = ints(text)
            return ptype, 0, struct.pack('<I', len(v)) + struct.pack('<%di' % len(v), *v), None
        if ptype == TYPES['FILENAME']:
            i = image_id(text)
            if i is None:
                stats['unresolved_image'] += 1
                missing.append(text)
                return None
            return ptype, i, b'', 0
        if ptype == TYPES['FONT']:
            return ptype, font_id(text), b'', 92
        return None

    for name, items in sections:
        low = name.lower()
        if low == 'sysmetrics':
            app, cname, part, state, pid, sid = '', 'sysmetrics', '', '', 0, 0
        elif low == 'globals':
            app, cname, part, state, pid, sid = '', 'globals', '', '', 0, 0
        else:
            app, cname, part, state = split_section(name)
            ps = lookup_part_state(cmap_tab, cname, part, state)
            if ps is None or not cname:
                stats['unknown_section'] += 1
                continue
            pid, sid = ps
        full = (app + '::' if app else '') + cname
        c = cls_ordinal(full)
        bucket = c.setdefault((pid, sid), [])
        for k, v in items:
            pr = props.get(k.upper())
            if pr is None:
                stats['unknown_prop'] += 1
                continue
            if a.basic and cname.lower() == 'liquid' and k.upper() == 'COMPOSITED':
                v = 'false'
            enc = encode(pr[0], pr[1], v, None)
            if enc is None:
                stats['bad_value'] += 1
                print('skip [%s] %s = %s' % (name, k, v), file=sys.stderr)
                continue
            bucket.append((pr[1], enc))

    descriptors = ['documentation', 'sizevariant.NormalSize', 'colorvariant.NormalColor']
    # globals and sysmetrics first among the classes, then the rest in file order
    first = [n for n in ('globals', 'sysmetrics') if n in classes]
    rest = [n for n in order if n not in first]
    real = first + rest
    names = descriptors + real
    ordinal = {n: len(descriptors) + i for i, n in enumerate(real)}

    variant = b''
    for n in real:
        for (pid, sid) in sorted(classes[n]):
            for nid, (t, ref, data, logical) in classes[n][(pid, sid)]:
                variant += record(nid, t, ordinal[n], pid, sid, ref, data, logical)
                stats['records'] += 1

    # BCMAP: an App::Class styles on Class when that class exists; the engine appends globals itself
    shift = len(descriptors)
    bc = []
    for n in real:
        base = 0xFFFFFFFF
        if '::' in n:
            b = n.split('::', 1)[1]
            if b in ordinal:
                base = ordinal[b] - shift
        bc.append(base)
    bcmap = struct.pack('<I', len(bc)) + struct.pack('<%dI' % len(bc), *bc)

    disp_id = 600
    rmap = record(1, TYPES['STRING'], 0, 0, 0, disp_id, b'', 2 * (len(a.display) + 1))
    if 'documentation' in names:
        pass

    # images
    for c, i in images.items():
        w, h, rows = read_bmp(os.path.join(a.bitmaps, bmps[c]))
        write_png(os.path.join(a.out, 'img', '%d.png' % i), w, h, rows)

    open(os.path.join(a.out, 'cmap.bin'), 'wb').write(build_cmap(names))
    open(os.path.join(a.out, 'vmap.bin'), 'wb').write(build_vmap('Normal', 'NormalSize', 'NormalColor'))
    open(os.path.join(a.out, 'bcmap.bin'), 'wb').write(bcmap)
    open(os.path.join(a.out, 'variant.bin'), 'wb').write(variant)
    open(os.path.join(a.out, 'rmap.bin'), 'wb').write(rmap)

    rc = []
    if a.reactos_version:
        rc += ['#define REACTOS_VERSION_DLL', '#define REACTOS_STR_FILE_DESCRIPTION "%s\\0"' % (a.description or a.display),
               '#define REACTOS_STR_INTERNAL_NAME "%s\\0"' % a.name,
               '#define REACTOS_STR_ORIGINAL_FILENAME "%s.msstyles\\0"' % a.name, '#include <reactos/version.rc>', '']
    rc += ['#include <windows.h>', '', 'LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL', '',
          '1 PACKTHEM_VERSION {0x04,}', 'CMAP CMAP "cmap.bin"', 'VMAP VMAP "vmap.bin"', 'BCMAP BCMAP "bcmap.bin"',
          'RMAP RMAP "rmap.bin"', 'NORMAL VARIANT "variant.bin"', '1 MINCOLORDEPTH {0x000F,}', '']
    for c, i in sorted(images.items(), key=lambda kv: kv[1]):
        rc.append('%d IMAGE "img/%d.png"' % (i, i))
    rc += ['', 'STRINGTABLE', '{']
    ents = {disp_id: a.display}
    for t, i in fonts.items():
        ents[i] = t
    for i in sorted(ents):
        rc.append('    %d, "%s"' % (i, ents[i].replace('"', '""')))
    rc.append('}')
    open(os.path.join(a.out, a.name + '.rc'), 'w').write('\r\n'.join(rc) + '\r\n')

    print('classes=%d records=%d images=%d fonts=%d variant=%d bytes' %
          (len(real), stats['records'], len(images), len(fonts), len(variant)))
    print({k: v for k, v in stats.items() if k != 'records'})
    # Explorer bar images map to the 'dummy' bitmap in win8.rc and are known to be absent; anything else is a lost image
    lost = [m for m in missing if 'explorerbar' not in m.lower() and m.lower().replace(chr(92), '/') not in KNOWN_ABSENT]
    if lost:
        raise SystemExit('unresolved images: %s' % ', '.join(sorted(set(lost))))


if __name__ == '__main__':
    main()
