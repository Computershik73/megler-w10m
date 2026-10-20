#!/usr/bin/env bash
# Builds the Java framework an app with an Activity runs on: android.* written
# in Java on top of libcore, with its native side in meglet.* classes that the
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
#   out: build/framework/meglet-framework.dex   (tools/dvm-android-root.sh puts
#        it in the Android root as framework/meglet-framework.dex)
#
# Nothing is rebuilt when the DEX file is newer than every source, unless
# --force is given. Nothing is downloaded.
#
#   MEGLET_JDK          a JDK's folder (C:\Program Files\Java\jdk1.8.0_202)
#   MEGLET_ANDROID_SDK  an Android SDK with build-tools 29.0.3 and
#                          platforms/android-29 (the user's Documents folder)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FW="$ROOT/core/framework"
JDK="${MEGLET_JDK:-/c/Program Files/Java/jdk1.8.0_202}"
SDK="${MEGLET_ANDROID_SDK:-$USERPROFILE/Documents}"
D8="$SDK/build-tools/29.0.3/lib/d8.jar"
LIB="$SDK/platforms/android-29/android.jar"
OVERLAY="$ROOT/core/framework-overlay"
OUT="$ROOT/build/framework"
DEX="$OUT/meglet-framework.dex"
FORCE=0
[ "${1:-}" = --force ] && FORCE=1
[ -x "$JDK/bin/javac" ] || [ -f "$JDK/bin/javac.exe" ] || { echo "no javac in $JDK (MEGLET_JDK)" >&2; exit 1; }
[ -f "$D8" ] && [ -f "$LIB" ] || { echo "no build-tools/29.0.3/lib/d8.jar or platforms/android-29/android.jar in $SDK (MEGLET_ANDROID_SDK)" >&2; exit 1; }

if [ "$FORCE" = 0 ] && [ -f "$DEX" ] && [ -z "$(find "$FW/src" "$FW/gen-src" "$FW/stubs" "$OVERLAY" "$0" -newer "$DEX" -type f -print -quit)" ]; then
  echo "build/framework/meglet-framework.dex is newer than its sources ($(( $(stat -c%s "$DEX") / 1024 )) KB)"
  exit 0
fi

mkdir -p "$OUT"
rm -rf "$OUT/stubs" "$OUT/classes" "$OUT/dex"
mkdir -p "$OUT/stubs" "$OUT/classes" "$OUT/dex"
# javac is a Windows program: it is given the lists as files, with drive letters and forward slashes.
win() { sed -E 's|^/([a-zA-Z])/|\1:/|; s|.*|"&"|'; }
m() { cygpath -m "$1"; }

# The overlay's files, and the original's but for those the overlay replaces.
(cd "$OVERLAY" && find . -name '*.java' | sed 's|^\./||' | sort) > "$OUT/overlay.txt"
(cd "$OVERLAY" && find . -name '*.java.removed' | sed 's|^\./||; s|\.removed$||' | sort) > "$OUT/removed.txt"
: > "$OUT/sources.txt"
REPLACED=0
REMOVED=0
for tree in src gen-src; do
  while IFS= read -r rel; do
    if grep -q -x -F "$rel" "$OUT/overlay.txt"; then REPLACED=$((REPLACED + 1))
    elif grep -q -x -F "$rel" "$OUT/removed.txt"; then REMOVED=$((REMOVED + 1))
    else echo "$FW/$tree/$rel" >> "$OUT/sources.txt"; fi
  done < <(cd "$FW/$tree" && find . -name '*.java' | sed 's|^\./||' | sort)
done
while IFS= read -r rel; do
  [ -f "$FW/src/$rel" ] || [ -f "$FW/gen-src/$rel" ] || { echo "core/framework-overlay/$rel.removed takes away $rel, which the original does not have" >&2; exit 1; }
done < "$OUT/removed.txt"
ORIGINALS=$(wc -l < "$OUT/sources.txt")
sed "s|^|$OVERLAY/|" "$OUT/overlay.txt" >> "$OUT/sources.txt"
# The classes that are given more members: each compiled from a copy with the members in it, in place of the file it was made from.
rm -rf "$OUT/members"
ADDED=0
while IFS= read -r rel; do
  java="${rel%.members}"
  base="$(grep -F "/$java" "$OUT/sources.txt" | grep -E "(/src|/gen-src|/framework-overlay)/$java\$" | head -1 || true)"
  [ -n "$base" ] || { echo "core/framework-overlay/$rel adds members to $java, which is neither the original's nor the overlay's" >&2; exit 1; }
  mkdir -p "$OUT/members/$(dirname "$java")"
  python - "$(m "$base")" "$(m "$OVERLAY/$rel")" "$(m "$OUT/members/$java")" <<'EOF'
import io, sys
base, members, out = sys.argv[1:4]
text = io.open(base, encoding='utf-8', newline='').read().replace('\r\n', '\n')
more = io.open(members, encoding='utf-8', newline='').read().replace('\r\n', '\n')
at = text.rstrip().rfind('}')
if at < 0:
    sys.exit('no closing brace in ' + base)
io.open(out, 'w', encoding='utf-8', newline='\n').write(text[:at] + '    // ---- added by the overlay (' + members.rsplit('/', 1)[-1] + ')\n' + more + '}\n')
EOF
  grep -v -x -F "$base" "$OUT/sources.txt" > "$OUT/sources.tmp"
  mv "$OUT/sources.tmp" "$OUT/sources.txt"
  echo "$OUT/members/$java" >> "$OUT/sources.txt"
  ADDED=$((ADDED + 1))
done < <(cd "$OVERLAY" && find . -name '*.java.members' | sed 's|^\./||' | sort)
win < "$OUT/sources.txt" > "$OUT/sources.arg"
find "$FW/stubs" -name '*.java' | sort | win > "$OUT/stubs.arg"
echo "framework: $ORIGINALS files of the original's, $(wc -l < "$OUT/overlay.txt") of the overlay ($REPLACED of them in place of an original), $ADDED classes given more members, $REMOVED of the original's taken away"

"$JDK/bin/javac" -nowarn -source 8 -target 8 -encoding UTF-8 -d "$(m "$OUT/stubs")" "@$(m "$OUT/stubs.arg")" 2> "$OUT/javac-stubs.log" \
  || { grep -v '^Note:\|^warning:' "$OUT/javac-stubs.log" | head -40 >&2; echo "javac failed on the stubs" >&2; exit 1; }
"$JDK/bin/javac" -nowarn -source 8 -target 8 -encoding UTF-8 -Xmaxerrs 200 -cp "$(m "$OUT/stubs")" -d "$(m "$OUT/classes")" "@$(m "$OUT/sources.arg")" 2> "$OUT/javac.log" \
  || { grep -v '^Note:\|^warning:' "$OUT/javac.log" | head -60 >&2; echo "javac failed (build/framework/javac.log)" >&2; exit 1; }
(cd "$OUT/classes" && "$JDK/bin/jar" cf ../classes.jar .)
echo "framework: $(find "$OUT/classes" -name '*.class' | wc -l) classes compiled"
# --min-api 26 as the original: interfaces keep their default and static methods; lambdas become classes.
"$JDK/bin/java" -cp "$(m "$D8")" com.android.tools.r8.D8 --release --min-api 26 --lib "$(m "$LIB")" --output "$(m "$OUT/dex")" "$(m "$OUT/classes.jar")" 2> "$OUT/d8.log" \
  || { head -40 "$OUT/d8.log" >&2; echo "d8 failed (build/framework/d8.log)" >&2; exit 1; }
[ ! -f "$OUT/dex/classes2.dex" ] || { echo "d8 wrote more than one DEX file: the framework has outgrown one, and the runtime is given one" >&2; exit 1; }
mv "$OUT/dex/classes.dex" "$DEX"
rm -rf "$OUT/dex" "$OUT/classes" "$OUT/stubs" "$OUT/classes.jar"
echo "built build/framework/meglet-framework.dex ($(( $(stat -c%s "$DEX") / 1024 )) KB)"
