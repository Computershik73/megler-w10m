#!/usr/bin/env bash
# Builds the platform's shared libraries that are Megler's own, each a DEX file of its own that tools/dvm-android-root.sh
# puts in the Android root's framework folder, beside org.apache.http.legacy.dex (Android 9's own, from its image):
#
#   tools/build-shared-libraries.sh
#
#   core/shared-libraries/<name>/**/*.java   ->   build/shared-libraries/<name>.dex
#
# An app has such a library on its class path, ahead of its own code, where Android's package manager would give it to
# it (core/src/drivers/mg_java.c, add_shared_libraries). There is one:
#
#   android.test.base   JUnit 3's junit.framework and Android's android.test base classes (AndroidTestCase,
#                       InstrumentationTestCase, the test annotations), and com.android.internal.util.Predicate: what
#                       every app had on Android's boot class path up to Android 9, and since Android 10 a shared
#                       library the package manager adds to an app made for Android 10 or before. Facebook's SDK of
#                       those years asserts with junit.framework.Assert on the main thread. Android's own classes are
#                       JUnit 3.8's code, under the Common Public License 1.0, which does not mix with the GPL; these
#                       are written here to the API Android documents, its failures worded as Android's.
#
# javac against the SDK's android.jar (platforms/android-29, which has neither junit.framework nor android.test), then d8.
# A library is built again only when one of its sources, or this script, is newer than its DEX file. Nothing is
# downloaded.
#
#   MEGLER_JDK          a JDK's folder (C:\Program Files\Java\jdk1.8.0_202)
#   MEGLER_ANDROID_SDK  an Android SDK with build-tools 29.0.3 and platforms/android-29 (the user's Documents folder)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JDK="${MEGLER_JDK:-/c/Program Files/Java/jdk1.8.0_202}"
SDK="${MEGLER_ANDROID_SDK:-$USERPROFILE/Documents}"
JAR="$SDK/platforms/android-29/android.jar"
D8="$SDK/build-tools/29.0.3/lib/d8.jar"
OUT="$ROOT/build/shared-libraries"
m() { cygpath -m "$1"; }
[ -x "$JDK/bin/javac" ] || [ -f "$JDK/bin/javac.exe" ] || { echo "no javac in $JDK (MEGLER_JDK)" >&2; exit 1; }
[ -f "$JAR" ] && [ -f "$D8" ] || { echo "no platforms/android-29/android.jar or build-tools/29.0.3/lib/d8.jar in $SDK (MEGLER_ANDROID_SDK)" >&2; exit 1; }
mkdir -p "$OUT"
for lib in "$ROOT"/core/shared-libraries/*/; do
  name="$(basename "$lib")"
  dex="$OUT/$name.dex"
  if [ -f "$dex" ] && [ -z "$(find "$lib" "$0" -type f -newer "$dex" -print -quit)" ]; then continue; fi
  work="$OUT/$name"
  rm -rf "$work"
  mkdir -p "$work/classes" "$work/dex"
  # (javac is a Windows program: it is given the list as a file, with drive letters and forward slashes.)
  find "$lib" -name '*.java' | sed -E 's|^/([a-zA-Z])/|\1:/|; s|.*|"&"|' > "$work/sources.arg"
  "$JDK/bin/javac" -nowarn -source 8 -target 8 -encoding UTF-8 -bootclasspath "$(m "$JAR")" -d "$(m "$work/classes")" "@$(m "$work/sources.arg")" 2> "$work/javac.log" \
    || { grep -v '^Note:\|^warning:' "$work/javac.log" | head -40 >&2; echo "javac failed for $name" >&2; exit 1; }
  (cd "$work/classes" && "$JDK/bin/jar" cf ../classes.jar .)
  # (Line numbers kept: a failure's stack through these classes says where.)
  "$JDK/bin/java" -cp "$(m "$D8")" com.android.tools.r8.D8 --min-api 26 --lib "$(m "$JAR")" --output "$(m "$work/dex")" "$(m "$work/classes.jar")" 2> "$work/d8.log" \
    || { head -40 "$work/d8.log" >&2; echo "d8 failed for $name" >&2; exit 1; }
  mv "$work/dex/classes.dex" "$dex"
  echo "built build/shared-libraries/$name.dex: $(find "$work/classes" -name '*.class' | wc -l) classes, $(stat -c%s "$dex") bytes"
done
