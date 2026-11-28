#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 Leeway Open Software
"""check-freeldr-size: fail when the PC FreeLoader image is too big to boot.
usage: python3 -I check-freeldr-size.py <build-dir> [<source-dir>]
FreeLoader on PC BIOS systems checks its own PE header at run time (lib/mm/meminit.c): a
SizeOfImage above MAX_FREELDR_PE_SIZE stops the boot before the menu with
"FreeLdr OptionalHeader is invalid", and nothing in the build notices. This script reads
the limit from include/arch/pc/x86common.h (MEMORY_MARGIN - FREELDR_PE_BASE - PAGE_SIZE)
and the SizeOfImage of every freeldr_pe*.exe and freeldr.sys under the build directory.
Exit status: 0 all fit, 1 one is too big, 2 nothing found or read error. No third-party
module is needed.
"""
import os
import re
import struct
import sys

PAGE_SIZE = 0x1000


def limit(source_dir):
    path = os.path.join(source_dir, "boot", "freeldr", "freeldr", "include", "arch", "pc", "x86common.h")
    values = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*#define\s+(MEMORY_MARGIN|FREELDR_PE_BASE)\s+HEX\(([0-9A-Fa-f]+)\)", line)
            if m:
                values[m.group(1)] = int(m.group(2), 16)
    if "MEMORY_MARGIN" not in values or "FREELDR_PE_BASE" not in values:
        raise ValueError("MEMORY_MARGIN / FREELDR_PE_BASE not found in " + path)
    return values["MEMORY_MARGIN"] - values["FREELDR_PE_BASE"] - PAGE_SIZE


def size_of_image(path):
    """SizeOfImage of the PE in the file; freeldr.sys carries a real-mode stub before it."""
    with open(path, "rb") as f:
        data = f.read()
    pos = 0
    while True:
        mz = data.find(b"MZ", pos)
        if mz < 0 or mz + 0x40 > len(data):
            return None
        lfanew = struct.unpack_from("<I", data, mz + 0x3C)[0]
        pe = mz + lfanew
        if 0 < lfanew < 0x1000 and data[pe:pe + 4] == b"PE\0\0":
            return struct.unpack_from("<I", data, pe + 24 + 56)[0]
        pos = mz + 2


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    build_dir = argv[1]
    source_dir = argv[2] if len(argv) > 2 else os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    try:
        maximum = limit(source_dir)
    except (OSError, ValueError) as e:
        print("check-freeldr-size: " + str(e))
        return 2
    found = []
    for root, _dirs, files in os.walk(os.path.join(build_dir, "boot", "freeldr")):
        for name in files:
            lower = name.lower()
            if (lower.startswith("freeldr_pe") and lower.endswith(".exe")) or lower == "freeldr.sys":
                found.append(os.path.join(root, name))
    if not found:
        print("check-freeldr-size: no freeldr_pe*.exe or freeldr.sys under " + build_dir)
        return 2
    bad = 0
    for path in sorted(found):
        size = size_of_image(path)
        if size is None:
            print("check-freeldr-size: no PE header in " + path)
            bad += 1
            continue
        verdict = "ok" if size <= maximum else "TOO BIG"
        print("%s: SizeOfImage 0x%X, limit 0x%X: %s" % (path, size, maximum, verdict))
        if size > maximum:
            bad += 1
    if bad:
        print("check-freeldr-size: the PC FreeLoader would stop at boot with 'FreeLdr OptionalHeader is invalid'")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
