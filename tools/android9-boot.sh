#!/bin/bash
# Android 9's boot class path as plain DEX files, in build/android9/boot.
#
#   tools/android9-boot.sh [jar...]        (from Git Bash; default: every jar the image has)
#
# From what tools/android9-unpack.sh took out of the image:
#
#   1. tools/vdex-dex.py turns each jar's compact DEX (in boot*.vdex) back into
#      DEX: build/android9/raw/<jar>.dex. Those files are right but untidy (the
#      old data section whole, with the rebuilt code after it).
#   2. d8 reads each and writes it again as it writes any DEX file: sorted,
#      nothing unused, a full map. --no-desugaring and --debug: the bytecode is
#      not to be changed and the line numbers are to stay.
#   3. tools/dexasm.py check on the result, and the SDK's dexdump, which is the
#      old libdex's own verifier: two readers that share no code with d8.
#
# libcore is the first six: core-oj, core-libart, conscrypt, okhttp,
# bouncycastle, apache-xml (the order Android 9's BOOTCLASSPATH has them in).
# The rest is the framework and what it leans on.
#
# Needs a JDK and an Android SDK's build-tools (for d8.jar and dexdump):
#   MEGLER_JAVA        java                      (C:\Program Files\Java\jdk1.8.0_202\bin\java.exe)
#   MEGLER_BUILD_TOOLS the build-tools folder    (C:\Users\User\Documents\build-tools\29.0.3)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/android9/system/framework"
RAW="$ROOT/build/android9/raw"
OUT="$ROOT/build/android9/boot"
JAVA="${MEGLER_JAVA:-/c/Program Files/Java/jdk1.8.0_202/bin/java.exe}"
BT="${MEGLER_BUILD_TOOLS:-/c/Users/User/Documents/build-tools/29.0.3}"
[ -d "$SRC" ] || { echo "no $SRC: run tools/android9-unpack.sh first" >&2; exit 1; }
[ -x "$JAVA" ] || { echo "no java at $JAVA (set MEGLER_JAVA)" >&2; exit 1; }
[ -f "$BT/lib/d8.jar" ] || { echo "no d8.jar in $BT/lib (set MEGLER_BUILD_TOOLS)" >&2; exit 1; }
w() { cygpath -w "$1"; }

rm -rf "$RAW" && mkdir -p "$RAW" "$OUT"
python "$ROOT/tools/vdex-dex.py" "$SRC" "$RAW" "$@"

failed=0
for raw in "$RAW"/*.dex; do
  name="$(basename "$raw" .dex)"
  tmp="$RAW/$name.d8"
  rm -rf "$tmp" && mkdir -p "$tmp"
  "$JAVA" -Xmx3g -cp "$(w "$BT/lib/d8.jar")" com.android.tools.r8.D8 --min-api 28 --no-desugaring --debug --output "$(w "$tmp")" "$(w "$raw")"
  if [ "$(ls "$tmp" | wc -l)" != 1 ]; then echo "$name: d8 wrote $(ls "$tmp" | tr '\n' ' ') -- one file was expected" >&2; failed=1; continue; fi
  mv "$tmp/classes.dex" "$OUT/$name.dex"
  rmdir "$tmp"
  # The same classes, members, code and instructions before and after d8 (the two lines differ only in what d8 does not keep: the file's name, and its SHA-1).
  before="$(python "$ROOT/tools/dexasm.py" check "$raw" | sed -e 's/^[^:]*: //' -e 's/ (its signature.*//')"
  after="$(python "$ROOT/tools/dexasm.py" check "$OUT/$name.dex" | sed -e 's/^[^:]*: //' -e 's/ (its signature.*//')"
  if [ "$before" != "$after" ]; then echo "$name: d8 changed what is in the file:"; echo "  before: $before"; echo "  after:  $after"; failed=1; fi
  if "$BT/dexdump.exe" -f "$(w "$OUT/$name.dex")" 2>&1 | grep -qE "^ERROR|E/libdex|structural verification"; then echo "$name: the SDK's dexdump refuses the file" >&2; failed=1; fi
  echo "$(printf '%-44s' "$name.dex") $(stat -c%s "$OUT/$name.dex") bytes; $after"
done
rm -rf "$RAW"
[ "$failed" = 0 ] || { echo "NOT all files are right" >&2; exit 1; }
echo "boot class path in $OUT: $(du -sh "$OUT" | cut -f1)"
