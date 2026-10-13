#!/usr/bin/env bash
# Builds the test apps of the Java driver: ordinary Android apps with no
# native code, made the way an app is made -- aapt2 for the manifest and the
# resources, javac against the SDK's android.jar, d8 -- and not signed (the
# layer asks no signature of an APK).
#
#   tools/build-test-app.sh [--force]
#
#   core/tests/app/<name>/AndroidManifest.xml, res/, src/   ->   build/guest/java/<name>.apk
#
# The one picture among the resources (res/drawable/mark.png of `hello`) is
# written by this script the first time, so that the tests know every pixel of
# it: 16 x 16, 8-bit RGBA, the left half opaque orange (255, 128, 0), the
# right half opaque green (0, 160, 64), with a transparent one-pixel border.
#
# Nothing is rebuilt when an APK is newer than its sources, unless --force is
# given. Nothing is downloaded.
#
#   MEGLER_JDK          a JDK's folder (C:\Program Files\Java\jdk1.8.0_202)
#   MEGLER_ANDROID_SDK  an Android SDK with build-tools 29.0.3 (aapt2, d8)
#                          and platforms/android-29 (the user's Documents
#                          folder). The apps use nothing newer than API 28.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JDK="${MEGLER_JDK:-/c/Program Files/Java/jdk1.8.0_202}"
SDK="${MEGLER_ANDROID_SDK:-$USERPROFILE/Documents}"
BT="$SDK/build-tools/29.0.3"
JAR="$SDK/platforms/android-29/android.jar"
APPS="$ROOT/core/tests/app"
OUT="$ROOT/build/guest/java"
FORCE=0
[ "${1:-}" = --force ] && FORCE=1
[ -f "$BT/aapt2.exe" ] && [ -f "$BT/lib/d8.jar" ] && [ -f "$JAR" ] || { echo "no build-tools/29.0.3 (aapt2.exe, lib/d8.jar) or platforms/android-29/android.jar in $SDK (MEGLER_ANDROID_SDK)" >&2; exit 1; }
[ -f "$JDK/bin/javac.exe" ] || [ -x "$JDK/bin/javac" ] || { echo "no javac in $JDK (MEGLER_JDK)" >&2; exit 1; }
m() { cygpath -m "$1"; }
mkdir -p "$OUT"

MARK="$APPS/hello/res/drawable/mark.png"
if [ ! -f "$MARK" ]; then
  python - "$(m "$MARK")" <<'EOF'
import struct, sys, zlib
rows = b''
for y in range(16):
    rows += b'\0'
    for x in range(16):
        if x in (0, 15) or y in (0, 15):
            rows += bytes((0, 0, 0, 0))
        elif x < 8:
            rows += bytes((255, 128, 0, 255))
        else:
            rows += bytes((0, 160, 64, 255))
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)
png = bytes((137, 80, 78, 71, 13, 10, 26, 10)) + chunk(b'IHDR', struct.pack('>IIBBBBB', 16, 16, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b'')
open(sys.argv[1], 'wb').write(png)
EOF
fi

for app in "$APPS"/*/; do
  name="$(basename "$app")"
  apk="$OUT/$name.apk"
  if [ "$FORCE" = 0 ] && [ -f "$apk" ] && [ -z "$(find "$app" "$0" -newer "$apk" -type f -print -quit)" ]; then
    echo "build/guest/java/$name.apk is newer than its sources"
    continue
  fi
  W="$OUT/$name.tmp"
  rm -rf "$W" && mkdir -p "$W/gen" "$W/classes" "$W/dex"
  "$BT/aapt2.exe" compile --dir "$(m "$app/res")" -o "$(m "$W/res.zip")"
  # (aapt2 leaves PNGs as they are with --no-crunch-like care only when asked; a 16 x 16 one comes out a valid PNG either way.)
  "$BT/aapt2.exe" link -o "$(m "$W/base.apk")" -I "$(m "$JAR")" --manifest "$(m "$app/AndroidManifest.xml")" --java "$(m "$W/gen")" \
    --min-sdk-version 21 --target-sdk-version 28 "$(m "$W/res.zip")" 2>&1 | { grep -v "is compressed" || true; }
  # (android.jar has no java.lang.invoke.LambdaMetafactory, which javac wants to see before it will compile a lambda; the build tools carry it apart.)
  find "$app/src" "$W/gen" -name '*.java' | sed -E 's|^/([a-zA-Z])/|\1:/|; s|.*|"&"|' > "$W/sources.arg"
  "$JDK/bin/javac" -nowarn -source 8 -target 8 -encoding UTF-8 -bootclasspath "$(m "$JAR");$(m "$BT/core-lambda-stubs.jar")" -d "$(m "$W/classes")" "@$(m "$W/sources.arg")" 2> "$W/javac.log" \
    || { grep -v '^Note:\|^warning:' "$W/javac.log" | head -40 >&2; echo "javac failed for $name" >&2; exit 1; }
  (cd "$W/classes" && "$JDK/bin/jar" cf ../classes.jar .)
  "$JDK/bin/java" -cp "$(m "$BT/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api 26 --lib "$(m "$JAR")" --output "$(m "$W/dex")" "$(m "$W/classes.jar")" 2> "$W/d8.log" \
    || { head -40 "$W/d8.log" >&2; echo "d8 failed for $name" >&2; exit 1; }
  python - "$(m "$W/base.apk")" "$(m "$W/dex/classes.dex")" "$(m "$apk")" <<'EOF'
import shutil, sys, zipfile
base, dex, out = sys.argv[1:4]
shutil.copyfile(base, out)
with zipfile.ZipFile(out, 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(dex, 'classes.dex')
EOF
  rm -rf "$W"
  echo "built build/guest/java/$name.apk ($(stat -c%s "$apk") bytes: $(python -c "import zipfile,sys; print(' '.join(sorted(zipfile.ZipFile(sys.argv[1]).namelist())))" "$(m "$apk")"))"
done
