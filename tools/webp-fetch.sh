#!/bin/bash
# libwebp's source, for WebP pictures in a Java app's resources, into
# build/webp/ (src/dec, src/dsp, src/utils and the headers in src/webp: the
# decoder is all that is compiled).
#
#   tools/webp-fetch.sh            (from Git Bash or WSL; needs curl, tar, sha256sum)
#
#   https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-1.6.0.tar.gz
#   libwebp 1.6.0, 4296070 bytes
#   SHA-256 e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
#   (the WebM project's release folder gives no hashes; this is the one the
#   file had when it was first fetched for this project, on 2026-10-14)
#
# Windows 10 Mobile's imaging component has no WebP decoder, and apps of the
# last several years keep most of their pictures as WebP: without one, an
# app's first layout that names such a drawable ends the app ("cannot decode
# res/drawable/android17.webp", a real app's on the phone).
#
# libwebp is under a BSD licence (COPYING, PATENTS: kept beside the source).
# It is not kept in this repository: it is fetched once and its decoder is
# compiled into the core when it is there.
set -euo pipefail

VERSION=1.6.0
SIZE=4296070
SHA256=e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/webp"
TAR="$OUT/libwebp-$VERSION.tar.gz"
mkdir -p "$OUT"

if [ -s "$OUT/src/dec/vp8_dec.c" ] && grep -q "libwebp-$VERSION" "$OUT/FROM" 2> /dev/null; then
  echo "have build/webp/src ($(cat "$OUT/FROM"))"
  exit 0
fi

[ -s "$TAR" ] || curl -s -S -L -f -m 600 -o "$TAR" "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$VERSION.tar.gz"
[ "$(stat -c%s "$TAR")" = "$SIZE" ] || { echo "the file is $(stat -c%s "$TAR") bytes, not $SIZE" >&2; exit 1; }
got="$(sha256sum "$TAR" | cut -d' ' -f1)"
[ "$got" = "$SHA256" ] || { echo "the file's SHA-256 is $got, not $SHA256" >&2; exit 1; }
rm -rf "$OUT/src"
tar -xzf "$TAR" -C "$OUT" --strip-components=1 "libwebp-$VERSION/COPYING" "libwebp-$VERSION/PATENTS" "libwebp-$VERSION/src/dec" "libwebp-$VERSION/src/dsp" \
  "libwebp-$VERSION/src/utils" "libwebp-$VERSION/src/webp"
echo "libwebp-$VERSION.tar.gz, $SIZE bytes, SHA-256 $SHA256" > "$OUT/FROM"
rm -f "$TAR"
echo "build/webp: $(find "$OUT/src" -name '*.c' | wc -l) C files, $(find "$OUT/src" -name '*.h' | wc -l) headers; the decoder is src/dec with what it uses of src/dsp and src/utils"
