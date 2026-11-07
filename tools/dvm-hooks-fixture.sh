#!/usr/bin/env bash
# Rebuilds the tests' fixture of the runtime's hooks (core/src/dvm/mg_dvm_hook.c, Xposed's XposedBridge): the Java under
# core/tests/dvm/hooks/src, compiled by javac and then d8, into core/tests/dvm/hooks/hooks.dex. The file is kept in the
# repository, as bench.dex is (tools/dvm-bench-fixture.sh), so building and running the tests needs neither a JDK nor d8:
# tools/build-dvm-fixtures.sh copies it beside the other fixtures. Run this when the sources change. What runs it is
# core/tests/mg_dvm_tests_hooks.c.
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
SRC="$ROOT/core/tests/dvm/hooks"
OUT="$ROOT/build/dvm/hooks"
rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex"
SOURCES=()
while IFS= read -r f; do SOURCES+=("$(w "$f")"); done < <(find "$SRC/src" -name '*.java' | sort)
"$JDK/bin/javac" -nowarn -source 8 -target 8 -encoding UTF-8 -bootclasspath "$(w "$LIB")" -d "$(w "$OUT/classes")" "${SOURCES[@]}"
CLASSES=()
while IFS= read -r c; do CLASSES+=("$(w "$c")"); done < <(find "$OUT/classes" -name '*.class' | sort)
"$JDK/bin/java" -cp "$(w "$D8")" com.android.tools.r8.D8 --release --min-api 26 --lib "$(w "$LIB")" --output "$(w "$OUT/dex")" "${CLASSES[@]}"
cp "$OUT/dex/classes.dex" "$SRC/hooks.dex"
echo "wrote core/tests/dvm/hooks/hooks.dex ($(wc -c < "$SRC/hooks.dex") bytes, ${#CLASSES[@]} classes)"
