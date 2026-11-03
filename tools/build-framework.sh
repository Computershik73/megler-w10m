#!/usr/bin/env bash
# Builds the Java framework an app with an Activity runs on: android.* written
# in Java on top of libcore, with its native side in megler.* classes that the
# core answers (core/src/drivers/mg_java*.c).
#
#   tools/build-framework.sh [--force]
#
# The sources are the framework's own (core/framework: src, gen-src, and stubs
# to compile against; core/framework/NOTICE says where it came from), with the
# overlay on top (core/framework-overlay): a file there replaces the file of the
# same path under src or gen-src, and a path that is in neither is added. Each
# overlay file says at its top why it differs.
#
# A second kind of overlay file adds to a class without replacing its file:
# X.java.members holds members -- methods, fields -- that are put into the
# original's X.java (or the overlay's own, if it has one) just before the
# closing brace of the file's last, outermost class. It is for the platform's
# public members that the original's class does not have and an app calls: a
# NoSuchMethodError where Android has a method. Copying a file of two thousand
# lines to add three would hide what was added; this way the overlay holds only
# that. The file begins with comment lines that say what is added and why.
#
# A third kind takes a file of the original's away: X.java.removed, whose
# lines are comments that say why, means the original's X.java (under src or
# gen-src) is not compiled at all. It is for a class of the original's that
# has no place beside what the overlay brings in its stead: the WebView's own
# glue classes, which the WebView's APK carries, and the original's helpers of
# a WebView over WKWebView, which the overlay's android.webkit does not use.
#
# javac, then d8, with what this PC has. The generated files of gen-src are
# kept as they are; nothing regenerates them here.
#
#   out: build/framework/megler-framework.dex   (tools/dvm-android-root.sh puts
#        it in the Android root as framework/megler-framework.dex)
#
# Nothing is rebuilt when the DEX file is newer than every source, unless
# --force is given. Nothing is downloaded.
#
#   MEGLER_JDK          a JDK's folder (C:\Program Files\Java\jdk1.8.0_202)
#   MEGLER_ANDROID_SDK  an Android SDK with build-tools 29.0.3 and
#                          platforms/android-29 (the user's Documents folder)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="$ROOT/core/framework"
JDK="${MEGLER_JDK:-/c/Program Files/Java/jdk1.8.0_202}"
SDK="${MEGLER_ANDROID_SDK:-$USERPROFILE/Documents}"
BUILD_TOOLS="${MEGLER_BUILD_TOOLS_VERSION:-29.0.3}"
PLATFORM="${MEGLER_ANDROID_PLATFORM:-android-29}"
D8="$SDK/build-tools/$BUILD_TOOLS/lib/d8.jar"
LIB="$SDK/platforms/$PLATFORM/android.jar"
OVERLAY="$ROOT/core/framework-overlay"
OUT="$ROOT/build/framework"
DEX="$OUT/megler-framework.dex"
FORCE=0
[ "${1:-}" = --force ] && FORCE=1
[ -x "$JDK/bin/javac" ] || [ -f "$JDK/bin/javac.exe" ] || { echo "no javac in $JDK (MEGLER_JDK)" >&2; exit 1; }
[ -f "$D8" ] && [ -f "$LIB" ] || { echo "no build-tools/$BUILD_TOOLS/lib/d8.jar or platforms/$PLATFORM/android.jar in $SDK (MEGLER_ANDROID_SDK)" >&2; exit 1; }

if [ "$FORCE" = 0 ] && [ -f "$DEX" ] && [ -z "$(find "$FW/src" "$FW/gen-src" "$FW/stubs" "$OVERLAY" "$0" "$ROOT/tools/framework-sources.py" -newer "$DEX" -type f -print -quit)" ]; then
  echo "build/framework/megler-framework.dex is newer than its sources ($(( $(stat -c%s "$DEX") / 1024 )) KB)"
  exit 0
fi

mkdir -p "$OUT"
rm -rf "$OUT/stubs" "$OUT/classes" "$OUT/dex"
mkdir -p "$OUT/stubs" "$OUT/classes" "$OUT/dex"
# javac is a Windows program: it is given the lists as files, with drive letters and forward slashes.
win() { sed -E 's|^/([a-zA-Z])/|\1:/|; s|.*|"&"|'; }
m() { cygpath -m "$1"; }

python "$ROOT/tools/framework-sources.py" "$(m "$FW")" "$(m "$OVERLAY")" "$(m "$OUT")"

"$JDK/bin/javac" -nowarn -source 8 -target 8 -encoding UTF-8 -d "$(m "$OUT/stubs")" "@$(m "$OUT/stubs.arg")" 2> "$OUT/javac-stubs.log" \
  || { grep -v '^Note:\|^warning:' "$OUT/javac-stubs.log" | head -40 >&2; echo "javac failed on the stubs" >&2; exit 1; }
"$JDK/bin/javac" -nowarn -source 8 -target 8 -encoding UTF-8 -Xmaxerrs 200 -cp "$(m "$OUT/stubs")" -d "$(m "$OUT/classes")" "@$(m "$OUT/sources.arg")" 2> "$OUT/javac.log" \
  || { grep -v '^Note:\|^warning:' "$OUT/javac.log" | head -60 >&2; echo "javac failed (build/framework/javac.log)" >&2; exit 1; }
(cd "$OUT/classes" && "$JDK/bin/jar" cf ../classes.jar .)
echo "framework: $(find "$OUT/classes" -name '*.class' | wc -l) classes compiled"
# The methods that are signatures alone, for the runtime to say when an app first calls one (tools/list-stubs.py).
python "$ROOT/tools/list-stubs.py" "$(m "$OUT/classes")" "$(m "$OUT/sources.txt")" "$(m "$FW")" "$(m "$OVERLAY")" "$(m "$OUT/megler-stubs.txt")" \
  || { echo "tools/list-stubs.py failed" >&2; exit 1; }
# --min-api 26 as the original: interfaces keep their default and static methods; lambdas become classes.
"$JDK/bin/java" -cp "$(m "$D8")" com.android.tools.r8.D8 --release --min-api 26 --lib "$(m "$LIB")" --output "$(m "$OUT/dex")" "$(m "$OUT/classes.jar")" 2> "$OUT/d8.log" \
  || { head -40 "$OUT/d8.log" >&2; echo "d8 failed (build/framework/d8.log)" >&2; exit 1; }
[ ! -f "$OUT/dex/classes2.dex" ] || { echo "d8 wrote more than one DEX file: the framework has outgrown one, and the runtime is given one" >&2; exit 1; }
mv "$OUT/dex/classes.dex" "$DEX"
rm -rf "$OUT/dex" "$OUT/classes" "$OUT/stubs" "$OUT/classes.jar"
echo "built build/framework/megler-framework.dex ($(( $(stat -c%s "$DEX") / 1024 )) KB)"
