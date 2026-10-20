#!/usr/bin/env bash
# Rebuilds the one fixture of the Dalvik runtime's tests that a real compiler makes:
#
#   core/tests/dvm/d8/d8t.dex           src/d8t/Main.java compiled by javac, then by d8: java.lang alone
#   core/tests/dvm/d8/expected.txt      what the same classes print when the JVM runs them
#   core/tests/dvm/d8/lib.dex           src/d8t/Lib.java the same way: java.util, java.io, java.text
#   core/tests/dvm/d8/lib-expected.txt  and what the JVM prints for those
# (lib-android.txt beside them is written by hand: where Android's libcore answers differently, and why.)
#
# Both are kept in the repository, so building and running the tests needs neither
# a JDK nor d8 (tools/build-dvm-fixtures.sh only copies them). This script is for when
# the Java source changes, and needs both:
#
#   MEGLET_JDK          a JDK's folder (default: C:\Program Files\Java\jdk1.8.0_202)
#   MEGLET_ANDROID_SDK  an Android SDK with build-tools 29.0.3 and platforms/android-29
#                          (default: the user's Documents folder, where this PC has one)
#
# d8 is run with --min-api 26, so interfaces keep their default and static methods;
# lambdas it always turns into classes. Nothing is downloaded.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JDK="${MEGLET_JDK:-/c/Program Files/Java/jdk1.8.0_202}"
SDK="${MEGLET_ANDROID_SDK:-$USERPROFILE/Documents}"
D8="$SDK/build-tools/29.0.3/lib/d8.jar"
LIB="$SDK/platforms/android-29/android.jar"
[ -f "$JDK/bin/javac.exe" ] || [ -f "$JDK/bin/javac" ] || { echo "no javac in $JDK (set MEGLET_JDK)" >&2; exit 1; }
[ -f "$D8" ] && [ -f "$LIB" ] || { echo "no build-tools/29.0.3/lib/d8.jar or platforms/android-29/android.jar in $SDK (set MEGLET_ANDROID_SDK)" >&2; exit 1; }
w() { cygpath -w "$1"; }
export MSYS2_ARG_CONV_EXCL="*"
SRC="$ROOT/core/tests/dvm/d8"
OUT="$ROOT/build/dvm/d8"
rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex"
# The JVM that says what the answers are is told to write UTF-8 and to be in the United States, whatever this PC is set to.
JVM=("$JDK/bin/java" -Dfile.encoding=UTF-8 -Duser.language=en -Duser.country=US -cp "$(w "$OUT/classes")")
"$JDK/bin/javac" -nowarn -source 8 -target 8 -encoding UTF-8 -d "$(w "$OUT/classes")" "$(w "$SRC/src/d8t/Main.java")" "$(w "$SRC/src/d8t/Lib.java")"
"${JVM[@]}" d8t.Main | tr -d '\r' > "$SRC/expected.txt"
mkdir -p "$OUT/tmp"
"${JVM[@]}" "-Djava.io.tmpdir=$(w "$OUT/tmp")" d8t.Lib 2> /dev/null | tr -d '\r' | grep -E '^(pure|lib)_[a-z]+=' > "$SRC/lib-expected.txt"
dex() {  # <name of the .dex> <class files...>: d8, one file out
  local name="$1"; shift
  rm -rf "$OUT/dex"; mkdir -p "$OUT/dex"
  "$JDK/bin/java" -cp "$(w "$D8")" com.android.tools.r8.D8 --release --min-api 26 --lib "$(w "$LIB")" --output "$(w "$OUT/dex")" "$@"
  cp "$OUT/dex/classes.dex" "$SRC/$name"
}
MAIN=(); LIBC=()
for c in "$OUT"/classes/d8t/Main*.class; do MAIN+=("$(w "$c")"); done
for c in "$OUT"/classes/d8t/Lib*.class; do LIBC+=("$(w "$c")"); done
dex d8t.dex "${MAIN[@]}"
dex lib.dex "${LIBC[@]}"
echo "wrote d8t.dex ($(wc -c < "$SRC/d8t.dex") bytes, ${#MAIN[@]} classes), lib.dex ($(wc -c < "$SRC/lib.dex") bytes, ${#LIBC[@]} classes), expected.txt and lib-expected.txt:"
cat "$SRC/expected.txt" "$SRC/lib-expected.txt"
