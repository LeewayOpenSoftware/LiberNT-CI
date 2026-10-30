#!/bin/sh
#
# PROJECT:     LiberNT NTFS library tests
# LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
# PURPOSE:     Update records that use the NTFS 3.0 header, without a segment number
# COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
#
set -eu

driver=$1
mkntfs=$2
ntfscp=$3
ntfsinfo=$4
ntfsfix=$5

workdir=$(mktemp -d "${TMPDIR:-/tmp}/ntfslib-legacy.XXXXXX")
trap 'rm -rf "$workdir"' EXIT HUP INT TERM
image="$workdir/legacy.ntfs"
timestamp=126256467060000000

truncate -s 32M "$image"
"$mkntfs" -F -Q -q -L LEGACY_REC "$image"
printf 'first-legacy-record\n' >"$workdir/a.bin"
printf 'second-legacy-record\n' >"$workdir/b.bin"
"$ntfscp" -f -q "$image" "$workdir/a.bin" /a.bin
"$ntfscp" -f -q "$image" "$workdir/b.bin" /b.bin

python3 -I - "$image" a.bin b.bin <<'EOF'
import struct
import sys

image = sys.argv[1]
names = sys.argv[2:]
with open(image, 'r+b') as f:
    data = bytearray(f.read())
    boot = data[:512]
    sector = struct.unpack_from('<H', boot, 0x0B)[0]
    cluster = sector * boot[0x0D]
    raw = struct.unpack_from('<b', boot, 0x40)[0]
    size = 1 << -raw if raw < 0 else raw * cluster
    converted = []
    for off in range(0, len(data) - size, size):
        if data[off:off + 4] != b'FILE':
            continue
        r = bytearray(data[off:off + size])
        usa_ofs, usa_cnt = struct.unpack_from('<HH', r, 4)
        if usa_ofs != 0x30 or usa_cnt != size // sector + 1:
            continue
        usa = [struct.unpack_from('<H', r, usa_ofs + 2 * i)[0] for i in range(usa_cnt)]
        for i in range(1, usa_cnt):
            struct.pack_into('<H', r, i * sector - 2, usa[i])
        attr = struct.unpack_from('<H', r, 0x14)[0]
        used = struct.unpack_from('<I', r, 0x18)[0]
        name = None
        pos = attr
        while pos + 8 <= used:
            atype, alen = struct.unpack_from('<II', r, pos)
            if atype == 0xFFFFFFFF or alen == 0:
                break
            if atype == 0x30:
                value = pos + struct.unpack_from('<H', r, pos + 20)[0]
                name = r[value + 66:value + 66 + 2 * r[value + 64]].decode('utf-16le')
            pos += alen
        if name not in names or attr != 0x38:
            continue
        r[0x30:used - 8] = r[0x38:used]
        r[used - 8:used] = bytes(8)
        struct.pack_into('<H', r, 0x14, 0x30)
        struct.pack_into('<I', r, 0x18, used - 8)
        struct.pack_into('<H', r, 4, 0x2A)
        struct.pack_into('<H', r, 0x2A, usa[0])
        for i in range(1, usa_cnt):
            struct.pack_into('<H', r, 0x2A + 2 * i, struct.unpack_from('<H', r, i * sector - 2)[0])
            struct.pack_into('<H', r, i * sector - 2, usa[0])
        data[off:off + size] = r
        converted.append(name)
    if sorted(converted) != sorted(names):
        sys.exit('converted %s, expected %s' % (converted, names))
    f.seek(0)
    f.write(data)
EOF

record0() {
    python3 -I - "$image" <<'EOF'
import hashlib
import struct
import sys

with open(sys.argv[1], 'rb') as f:
    boot = f.read(512)
    sector = struct.unpack_from('<H', boot, 0x0B)[0]
    cluster = sector * boot[0x0D]
    f.seek(struct.unpack_from('<Q', boot, 0x30)[0] * cluster)
    print(hashlib.sha256(f.read(1024)).hexdigest())
EOF
}

before=$(record0)
"$driver" --basic "$image" /a.bin >/dev/null
"$driver" --set-basic "$image" /b.bin \
    "$timestamp" "$timestamp" "$timestamp" "$timestamp" 0x21

if [ "$(record0)" != "$before" ]
then
    echo "updating a 3.0 record rewrote MFT record 0" >&2
    exit 1
fi

actual=$("$driver" --basic "$image" /b.bin)
expected="$timestamp $timestamp $timestamp $timestamp 0x00000021"
if [ "$actual" != "$expected" ]
then
    echo "unexpected basic information: $actual" >&2
    exit 1
fi
"$driver" --basic "$image" /a.bin >/dev/null

LC_ALL=C "$ntfsinfo" -F /b.bin "$image" >"$workdir/ntfsinfo.txt"
if [ "$(grep -cF 'Sat Feb  3 04:05:06 2001 UTC' "$workdir/ntfsinfo.txt")" -lt 4 ]
then
    echo "ntfsinfo did not find the updated timestamps in the 3.0 record" >&2
    exit 1
fi
"$ntfsfix" -n "$image" >/dev/null
