#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Ahmed ARIF <arif.ing@outlook.com>

set -euo pipefail
W="${1:?usage: build_runtime.sh <workdir>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CN="${WMRUNTIME_CONTAINER:-wmruntime}"
SRC=wine-mono-11.3.0-src.tar.xz
SHA=9f4f03285fbeed484980efbb1eba1c19069244aea401d2ff1585e3990782edf7
mkdir -p "$W/patches"
if [ ! -f "$W/$SRC" ] || [ "$(shasum -a 256 "$W/$SRC" | cut -d' ' -f1)" != "$SHA" ]; then
    curl -sSL -o "$W/$SRC" "https://dl.winehq.org/wine/wine-mono/11.3.0/$SRC"
fi
[ "$(shasum -a 256 "$W/$SRC" | cut -d' ' -f1)" = "$SHA" ] || { echo "$SRC: unexpected sha256" >&2; exit 1; }
rm -f "$W"/patches/*.patch
cp "$HERE"/patches/mono-runtime-*.patch "$W/patches/"
docker rm -f "$CN" >/dev/null 2>&1 || true
docker run -d --name "$CN" -v "$W:/w" debian:bookworm sleep infinity >/dev/null
docker exec "$CN" bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq && apt-get install -y -qq --no-install-recommends build-essential autoconf automake libtool cmake python3 wget xz-utils pkg-config gettext ca-certificates git >/dev/null
mkdir -p /build && tar -xJf /w/wine-mono-11.3.0-src.tar.xz -C /build
cd /build/wine-mono-11.3.0/mono
for p in /w/patches/*.patch; do patch -p1 < "$p"; done
cd /build/wine-mono-11.3.0
make -j"$(nproc)" libmono-2.0-x86.dll libmono-2.0-x86_64.dll libmono-2.0-arm64.dll >/w/build-runtime.log 2>&1
for a in x86 x86_64 arm64; do
    cp image/bin/libmono-2.0-$a.dll /w/libmono-2.0-$a.patched.dll
    if [ -f image/bin/libmono-2.0-$a.pdb ]; then cp image/bin/libmono-2.0-$a.pdb /w/libmono-2.0-$a.patched.pdb; fi
done'
docker rm -f "$CN" >/dev/null
ls -la "$W"/libmono-2.0-*.patched.dll
