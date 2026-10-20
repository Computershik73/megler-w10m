#!/usr/bin/env bash
# Makes what the Dalvik runtime (core/src/dvm) and its tests are built from and
# run on. tools/build-tests.sh and tools/build-appx.sh call it; it is quick and
# changes no file whose contents would come out the same.
#
#   core/src/dvm/mg_dvm_boot_dex.h   the built-in java.lang (tools/dvm-boot.py)
#   core/src/dvm/mg_dvm_art_dex.h    the classes the runtime adds to Android's libcore, from core/src/dvm/art/*.smali
#   core/src/dvm/mg_dvm_ops.h        the instruction table (tools/dexasm.py ctable)
#   core/tests/mg_dvm_ops_cases.h    what each arithmetic instruction must return (tools/dvm-ops.py)
#   build/guest/armeabi-v7a/dvm/     the tests' DEX files, beside the guest test libraries:
#       a.dex b.dex c.dex d.dex      one for each folder of core/tests/dvm/fixtures, assembled
#                                    from its .smali files by tools/dexasm.py
#       d8t.dex expected.txt         a program javac and d8 compiled, and what the JVM printed
#                                    for it: copied from core/tests/dvm/d8 as they are kept there
#                                    (tools/dvm-d8-fixture.sh rebuilds them, and needs a JDK and d8)
#       bench.dex                    the interpreter's benchmark, copied from core/tests/dvm/bench likewise
#                                    (tools/dvm-bench-fixture.sh rebuilds it)
#
# Python is all it needs.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="${MEGLET_PYTHON:-python}"
# Paths relative to the repository: Python here is a Windows program, and a caller may have told the shell not to translate /c/... for it.
OUT=build/guest/armeabi-v7a/dvm
GEN=build/dvm/gen
cd "$ROOT"
mkdir -p "$OUT" "$GEN"
keep() { cmp -s "$1" "$2" || cp "$1" "$2"; }

"$PY" tools/dvm-boot.py > /dev/null
"$PY" tools/dexasm.py asm -o "$GEN/art.dex" core/src/dvm/art/*.smali > /dev/null
"$PY" tools/dexasm.py carray -o "$GEN/mg_dvm_art_dex.h" --name k_dvm_art_dex "$GEN/art.dex"
keep "$GEN/mg_dvm_art_dex.h" core/src/dvm/mg_dvm_art_dex.h
"$PY" tools/dexasm.py ctable -o "$GEN/mg_dvm_ops.h"
keep "$GEN/mg_dvm_ops.h" core/src/dvm/mg_dvm_ops.h
"$PY" tools/dvm-ops.py "$GEN"
keep "$GEN/mg_dvm_ops_cases.h" core/tests/mg_dvm_ops_cases.h
for dir in core/tests/dvm/fixtures/*/; do
  name="$(basename "$dir")"
  extra=()
  [ "$name" = a ] && extra=("$GEN/Ops.smali")
  "$PY" tools/dexasm.py asm -o "$GEN/$name.dex" "$dir"*.smali "${extra[@]}" > /dev/null
  keep "$GEN/$name.dex" "$OUT/$name.dex"
done
# m.dex: a class for the tests to hand over in memory (InMemoryDexClassLoader); it is not put on the path.
"$PY" tools/dexasm.py asm -o "$GEN/m.dex" core/tests/dvm/memory/*.smali > /dev/null
keep "$GEN/m.dex" "$OUT/m.dex"
keep core/tests/dvm/d8/d8t.dex "$OUT/d8t.dex"
keep core/tests/dvm/d8/expected.txt "$OUT/expected.txt"
for f in lib.dex lib-expected.txt lib-android.txt; do [ -f "core/tests/dvm/d8/$f" ] && keep "core/tests/dvm/d8/$f" "$OUT/$f"; done
# The interpreter's benchmark (core/tests/mg_dvm_bench.c), kept compiled like d8t.dex (tools/dvm-bench-fixture.sh).
[ -f core/tests/dvm/bench/bench.dex ] && keep core/tests/dvm/bench/bench.dex "$OUT/bench.dex"
