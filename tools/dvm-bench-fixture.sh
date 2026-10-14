#!/usr/bin/env bash
# Rebuilds the interpreter's benchmark: core/tests/dvm/bench/src/bench/Bench.java, compiled by javac and then d8, into
# core/tests/dvm/bench/bench.dex. The file is kept in the repository, so building and running the tests needs neither a JDK
# nor d8 (tools/build-dvm-fixtures.sh only copies it beside the other fixtures, and tools/build-appx.sh packs it for the
# phone's Self-test). Run this when Bench.java changes. What runs it is core/tests/hk_dvm_bench.c.
#
#   MEGLER_JDK          a JDK's folder (default: C:\Program Files\Java\jdk1.8.0_202)
#   MEGLER_ANDROID_SDK  an Android SDK with build-tools 29.0.3 and platforms/android-29 (default: the user's Documents)
#
# As tools/dvm-d8-fixture.sh does: d8 --release --min-api 26. Nothing is downloaded.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JDK="${MEGLER_JDK:-/c/Program Files/Java/jdk1.8.0_202}"
SDK="${MEGLER_ANDROID_SDK:-$USERPROFILE/Documents}"
D8="$SDK/build-tools/29.0.3/lib/d8.jar"
LIB="$SDK/platforms/android-29/android.jar"
[ -f "$JDK/bin/javac.exe" ] || [ -f "$JDK/bin/javac" ] || { echo "no javac in $JDK (set MEGLER_JDK)" >&2; exit 1; }
[ -f "$D8" ] && [ -f "$LIB" ] || { echo "no build-tools/29.0.3/lib/d8.jar or platforms/android-29/android.jar in $SDK (set MEGLER_ANDROID_SDK)" >&2; exit 1; }
w() { cygpath -w "$1"; }
export MSYS2_ARG_CONV_EXCL="*"
SRC="$ROOT/core/tests/dvm/bench"
OUT="$ROOT/build/dvm/bench"
rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex"
"$JDK/bin/javac" -nowarn -source 8 -target 8 -encoding UTF-8 -bootclasspath "$(w "$LIB")" -d "$(w "$OUT/classes")" $(for f in "$SRC"/src/bench/*.java; do printf "%s " "$(w "$f")"; done)
CLASSES=()
for c in "$OUT"/classes/bench/*.class; do CLASSES+=("$(w "$c")"); done
"$JDK/bin/java" -cp "$(w "$D8")" com.android.tools.r8.D8 --release --min-api 26 --lib "$(w "$LIB")" --output "$(w "$OUT/dex")" "${CLASSES[@]}"
cp "$OUT/dex/classes.dex" "$SRC/bench.dex"
echo "wrote core/tests/dvm/bench/bench.dex ($(wc -c < "$SRC/bench.dex") bytes, ${#CLASSES[@]} classes)"
