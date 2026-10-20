#!/usr/bin/env bash
# Builds the test apps of the Java driver: ordinary Android apps, made the way
# an app is made -- aapt2 for the manifest, the resources and the assets,
# javac against the SDK's android.jar, d8, and the NDK for the one that has
# native code -- and not signed (the layer asks no signature of an APK).
#
#   tools/build-test-app.sh [--force]
#
#   core/tests/app/<name>/AndroidManifest.xml, res/, src/, assets/, jni/   ->   build/guest/java/<name>.apk
#
# An app with a file named `splits` is built as a base and split APKs, as for
# Google Play, and bundled: build/guest/java/<name>.apks and <name>.xapk
# (tools/build-split-app.py; the file says what the splits are).
#
# An app with a jni/ folder has native libraries, built by the NDK's clang
# against the NDK's own headers and libraries, as an app's are: armeabi-v7a,
# Thumb-2, softfp. Each jni/<x>.c becomes lib/armeabi-v7a/lib<x>.so; those
# named <x>_dep.c are built first, and every other library is linked against
# them (so that it needs them: DT_NEEDED). Where there is no NDK such an app is
# not built, which is said, and its APK is not there for the tests to run.
#
# The one picture among the resources (res/drawable/mark.png of `hello`) is
# written by this script the first time, so that the tests know every pixel of
# it: 16 x 16, 8-bit RGBA, the left half opaque orange (255, 128, 0), the
# right half opaque green (0, 160, 64), with a transparent one-pixel border.
#
# Nothing is rebuilt when an APK is newer than its sources, unless --force is
# given. Nothing is downloaded.
#
#   MEGLET_JDK          a JDK's folder (C:\Program Files\Java\jdk1.8.0_202)
#   MEGLET_ANDROID_SDK  an Android SDK with build-tools 29.0.3 (aapt2, d8)
#                          and platforms/android-29 (the user's Documents
#                          folder). The apps use nothing newer than API 28.
#   MEGLET_NDK          an Android NDK (android-ndk-r21e in the user's
#                          Documents folder), for the apps with jni/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JDK="${MEGLET_JDK:-/c/Program Files/Java/jdk1.8.0_202}"
SDK="${MEGLET_ANDROID_SDK:-$USERPROFILE/Documents}"
BT="$SDK/build-tools/29.0.3"
NDK="${MEGLET_NDK:-$USERPROFILE/Documents/android-ndk-r21e}"
NDKCC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
JAR="$SDK/platforms/android-29/android.jar"
APPS="$ROOT/core/tests/app"
OUT="$ROOT/build/guest/java"
FORCE=0
[ "${1:-}" = --force ] && FORCE=1
[ -f "$BT/aapt2.exe" ] && [ -f "$BT/lib/d8.jar" ] && [ -f "$JAR" ] || { echo "no build-tools/29.0.3 (aapt2.exe, lib/d8.jar) or platforms/android-29/android.jar in $SDK (MEGLET_ANDROID_SDK)" >&2; exit 1; }
[ -f "$JDK/bin/javac.exe" ] || [ -x "$JDK/bin/javac" ] || { echo "no javac in $JDK (MEGLET_JDK)" >&2; exit 1; }
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
  if [ -f "$app/splits" ]; then
    # An app in several APKs: a base and splits, bundled as an .apks and an .xapk (tools/build-split-app.py says how).
    if [ "$FORCE" = 0 ] && [ -f "$OUT/$name.xapk" ] && [ -z "$(find "$app" "$0" "$ROOT/tools/build-split-app.py" -newer "$OUT/$name.xapk" -type f -print -quit)" ]; then
      echo "build/guest/java/$name.apks and .xapk are newer than their sources"
      continue
    fi
    CC=-
    [ -f "$NDKCC" ] && CC="$(m "$NDKCC")"
    if [ -d "$app/jni" ] && [ "$CC" = - ]; then
      echo "(no NDK at $NDK (MEGLET_NDK): the test app $name, which has native code, is not built)"
      rm -f "$OUT/$name.apks" "$OUT/$name.xapk" "$OUT/$name-base.apk"
      continue
    fi
    python "$(m "$ROOT/tools/build-split-app.py")" "$(m "$app")" "$(m "$OUT")" "$(m "$BT")" "$(m "$JAR")" "$(m "$JDK")" "$CC" \
      || { rm -f "$OUT/$name.apks" "$OUT/$name.xapk" "$OUT/$name-base.apk"; echo "the split test app $name could not be built" >&2; exit 1; }
    continue
  fi
  apk="$OUT/$name.apk"
  if [ "$FORCE" = 0 ] && [ -f "$apk" ] && [ -z "$(find "$app" "$0" -newer "$apk" -type f -print -quit)" ]; then
    echo "build/guest/java/$name.apk is newer than its sources"
    continue
  fi
  if [ -d "$app/jni" ] && [ ! -f "$NDKCC" ]; then
    echo "(no NDK at $NDK (MEGLET_NDK): the test app $name, which has native code, is not built)"
    rm -f "$apk"
    continue
  fi
  W="$OUT/$name.tmp"
  rm -rf "$W" && mkdir -p "$W/gen" "$W/classes" "$W/dex" "$W/lib"
  if [ -d "$app/jni" ]; then
    # The libraries first: a failure here should not leave an APK without them.
    NDKFLAGS=(--target=armv7a-linux-androideabi21 -mthumb -O2 -fPIC -shared -Wall -Wextra -Werror=implicit-function-declaration)
    DEPS=()
    for src in "$app"/jni/*_dep.c; do
      [ -f "$src" ] || continue
      lib="$(basename "$src" .c)"
      "$NDKCC" "${NDKFLAGS[@]}" "-Wl,-soname,lib$lib.so" -o "$(m "$W/lib/lib$lib.so")" "$(m "$src")" 2> "$W/ndk.log" \
        || { head -40 "$W/ndk.log" >&2; echo "the NDK failed for $name ($lib)" >&2; exit 1; }
      DEPS+=("-l$lib")
    done
    for src in "$app"/jni/*.c; do
      case "$src" in *_dep.c) continue ;; esac
      lib="$(basename "$src" .c)"
      # (EGL and OpenGL ES only where a library uses them: --as-needed leaves them out of the others' DT_NEEDED.)
      "$NDKCC" "${NDKFLAGS[@]}" "-Wl,-soname,lib$lib.so" -o "$(m "$W/lib/lib$lib.so")" "$(m "$src")" "-L$(m "$W/lib")" "${DEPS[@]}" -llog -landroid -ljnigraphics -ldl \
        -Wl,--as-needed -lEGL -lGLESv2 2> "$W/ndk.log" \
        || { head -40 "$W/ndk.log" >&2; echo "the NDK failed for $name ($lib)" >&2; exit 1; }
    done
  fi
  ASSETS=()
  [ -d "$app/assets" ] && ASSETS=(-A "$(m "$app/assets")")
  "$BT/aapt2.exe" compile --dir "$(m "$app/res")" -o "$(m "$W/res.zip")"
  # (aapt2 leaves PNGs as they are with --no-crunch-like care only when asked; a 16 x 16 one comes out a valid PNG either way.)
  "$BT/aapt2.exe" link -o "$(m "$W/base.apk")" -I "$(m "$JAR")" --manifest "$(m "$app/AndroidManifest.xml")" --java "$(m "$W/gen")" \
    --min-sdk-version 21 --target-sdk-version 28 "${ASSETS[@]}" "$(m "$W/res.zip")" 2>&1 | { grep -v "is compressed" || true; }
  # (android.jar has no java.lang.invoke.LambdaMetafactory, which javac wants to see before it will compile a lambda; the build tools carry it apart.)
  find "$app/src" "$W/gen" -name '*.java' | sed -E 's|^/([a-zA-Z])/|\1:/|; s|.*|"&"|' > "$W/sources.arg"
  "$JDK/bin/javac" -nowarn -source 8 -target 8 -encoding UTF-8 -bootclasspath "$(m "$JAR");$(m "$BT/core-lambda-stubs.jar")" -d "$(m "$W/classes")" "@$(m "$W/sources.arg")" 2> "$W/javac.log" \
    || { grep -v '^Note:\|^warning:' "$W/javac.log" | head -40 >&2; echo "javac failed for $name" >&2; exit 1; }
  (cd "$W/classes" && "$JDK/bin/jar" cf ../classes.jar .)
  "$JDK/bin/java" -cp "$(m "$BT/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api 26 --lib "$(m "$JAR")" --output "$(m "$W/dex")" "$(m "$W/classes.jar")" 2> "$W/d8.log" \
    || { head -40 "$W/d8.log" >&2; echo "d8 failed for $name" >&2; exit 1; }
  python - "$(m "$W/base.apk")" "$(m "$W/dex/classes.dex")" "$(m "$apk")" "$(m "$W/lib")" <<'EOF'
import os, shutil, sys, zipfile
base, dex, out, libs = sys.argv[1:5]
shutil.copyfile(base, out)
with zipfile.ZipFile(out, 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(dex, 'classes.dex')
    for name in sorted(os.listdir(libs)):
        z.write(os.path.join(libs, name), 'lib/armeabi-v7a/' + name)
EOF
  rm -rf "$W"
  echo "built build/guest/java/$name.apk ($(stat -c%s "$apk") bytes: $(python -c "import zipfile,sys; print(' '.join(sorted(zipfile.ZipFile(sys.argv[1]).namelist())))" "$(m "$apk")"))"
done
