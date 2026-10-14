#!/usr/bin/env bash
# The interpreter's benchmark (core/tests/hk_dvm_bench.c) on this PC and under qemu-arm, on Android's libcore:
#
#   tools/dvm-bench.sh [win|arm|both] [milliseconds an item, 300]
#
#   win   the test program built for x64 with /O2, as the app is built (into build/tests-win-o2): the interpreter's own
#         speed on this machine, the steadiest of the numbers. libcore's native libraries do not run here; none of the
#         items needs them.
#   arm   the ARM test program tools/build-tests.sh arm builds (gcc -O1), under qemu-arm: what qemu makes of it, which
#         is not the phone's speed but moves with it.
#
# Each prints the table: for each item, nanoseconds a turn, instructions a turn, millions of instructions a second.
#
#   tools/dvm-bench.sh profile <item> [function]
#
#   profile  the x64 /O2 program with a linker map, the item run ten times as long while a second thread samples where
#            it is (hk_dvm_bench.c); tools/dvm-profile.py names the functions the samples fall in and, for the function
#            named (the interpreter's `hk_dvm_interpret` by default), shows its hottest instructions.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-both}"
MS="${2:-300}"
bash "$ROOT/tools/dvm-android-root.sh" > /dev/null
# The interpreter's loop first in the program's code, where nothing else moves it (tools/build-tests.sh, MEGLER_TESTS_ORDER).
mkdir -p "$ROOT/build/tests-win-o2"
ORDER="$ROOT/build/tests-win-o2/order.txt"
echo hk_dvm_interpret > "$ORDER"
if [ "$MODE" = profile ]; then
  ITEM="$2"
  FUNC="${3:-hk_dvm_interpret}"
  MEGLER_TESTS_OPT=/O2 MEGLER_TESTS_OUT="$ROOT/build/tests-win-o2" MEGLER_TESTS_MAP=1 MEGLER_TESTS_ORDER="$ORDER" bash "$ROOT/tools/build-tests.sh" win --no-run --no-libcore > /dev/null
  OUTF="$ROOT/build/tests-win-o2/samples-$ITEM.txt"
  HK_BENCH_PROFILE="$ITEM" HK_BENCH_PROFILE_OUT="$(cygpath -w "$OUTF")" "$ROOT/build/tests-win-o2/hk_tests.exe" --dvm-bench \
    "$(cygpath -w "$ROOT/build/guest/armeabi-v7a/dvm")" "$(cygpath -w "$ROOT/build/android9/root")" 200 | grep -E "samples|$ITEM" || true
  VC=""
  for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe; do [ -f "$cl" ] && VC="${cl%/cl.exe}"; done
  VCBIN="$(cygpath -w "$VC")" python "$ROOT/tools/dvm-profile.py" "$(cygpath -w "$OUTF")" "$(cygpath -w "$ROOT/build/tests-win-o2/hk_tests.map")" \
    "$(cygpath -w "$ROOT/build/tests-win-o2/hk_tests.exe")" "$FUNC" "${4:-40}"
  exit 0
fi
if [ "$MODE" = win ] || [ "$MODE" = both ]; then
  MEGLER_TESTS_OPT=/O2 MEGLER_TESTS_OUT="$ROOT/build/tests-win-o2" MEGLER_TESTS_ORDER="$ORDER" bash "$ROOT/tools/build-tests.sh" win --no-run --no-libcore > /dev/null
  echo "== x64, /O2"
  "$ROOT/build/tests-win-o2/hk_tests.exe" --dvm-bench "$(cygpath -w "$ROOT/build/guest/armeabi-v7a/dvm")" "$(cygpath -w "$ROOT/build/android9/root")" "$MS"
fi
if [ "$MODE" = arm ] || [ "$MODE" = both ]; then
  bash "$ROOT/tools/build-tests.sh" arm --no-run > /dev/null
  WB="${MEGLER_WSL_BUILD:-~/megler-w10m-build}"
  WROOT="/mnt/${ROOT:1:1}${ROOT:2}"
  echo "== qemu-arm, gcc -O1"
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "mkdir -p $WB/android-root && cp -ru '$WROOT/build/android9/root/.' $WB/android-root/ && qemu-arm -L /usr/arm-linux-gnueabihf $WB/hk_tests --dvm-bench '$WROOT/build/guest/armeabi-v7a/dvm' $WB/android-root $MS 2>&1"
fi
