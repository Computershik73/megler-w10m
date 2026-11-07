#!/usr/bin/env bash
# The interpreter's benchmark (core/tests/mg_dvm_bench.c) on this PC and under qemu-arm, on Android's libcore:
#
#   tools/dvm-bench.sh [win|arm|arm64|both] [milliseconds an item, 300]
#
#   win   the test program built for x64 with /O2, as the app is built (into build/tests-win-o2): the interpreter's own
#         speed on this machine, the steadiest of the numbers. libcore's native libraries do not run here; none of the
#         items needs them.
#   arm   the ARM test program tools/build-tests.sh arm builds (gcc -O1), under qemu-arm: what qemu makes of it, which
#         is not the phone's speed but moves with it.
#   arm64 the arm64 test program tools/build-tests.sh arm64 builds (clang -O1), under qemu-aarch64, on Android's libcore
#         with arm64-v8a's natives: what qemu makes of A64, an ARM64 PC's own speed even less than qemu-arm the phone's.
#
# The JIT compiles what is hot in the ARM programs (docs/JIT.md); MEGLER_JIT_THRESHOLD, when set, is handed to them (0:
# everything interpreted; 1000 the runtime's own), so that one run can be put beside the other.
#
# Each prints the table: for each item, nanoseconds a turn, instructions a turn, millions of instructions a second.
#
#   tools/dvm-bench.sh profile <item> [function]
#
#   profile  the x64 /O2 program with a linker map, the item run ten times as long while a second thread samples where
#            it is (mg_dvm_bench.c); tools/dvm-profile.py names the functions the samples fall in and, for the function
#            named (the interpreter's `mg_dvm_interpret` by default), shows its hottest instructions.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-both}"
MS="${2:-300}"
if [ -n "${MEGLER_JIT_THRESHOLD:-}" ]; then export WSLENV="${WSLENV:+$WSLENV:}MEGLER_JIT_THRESHOLD"; fi
bash "$ROOT/tools/dvm-android-root.sh" > /dev/null
# The interpreter's loop first in the program's code, where nothing else moves it (tools/build-tests.sh, MEGLER_TESTS_ORDER).
mkdir -p "$ROOT/build/tests-win-o2"
ORDER="$ROOT/build/tests-win-o2/order.txt"
echo mg_dvm_interpret > "$ORDER"
if [ "$MODE" = profile ]; then
  ITEM="$2"
  FUNC="${3:-mg_dvm_interpret}"
  MEGLER_TESTS_OPT=/O2 MEGLER_TESTS_OUT="$ROOT/build/tests-win-o2" MEGLER_TESTS_MAP=1 MEGLER_TESTS_ORDER="$ORDER" bash "$ROOT/tools/build-tests.sh" win --no-run --no-libcore > /dev/null
  OUTF="$ROOT/build/tests-win-o2/samples-$ITEM.txt"
  MG_BENCH_PROFILE="$ITEM" MG_BENCH_PROFILE_OUT="$(cygpath -w "$OUTF")" "$ROOT/build/tests-win-o2/mg_tests.exe" --dvm-bench \
    "$(cygpath -w "$ROOT/build/guest/armeabi-v7a/dvm")" "$(cygpath -w "$ROOT/build/android9/root")" 200 | grep -E "samples|$ITEM" || true
  VC=""
  for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe; do [ -f "$cl" ] && VC="${cl%/cl.exe}"; done
  VCBIN="$(cygpath -w "$VC")" python "$ROOT/tools/dvm-profile.py" "$(cygpath -w "$OUTF")" "$(cygpath -w "$ROOT/build/tests-win-o2/mg_tests.map")" \
    "$(cygpath -w "$ROOT/build/tests-win-o2/mg_tests.exe")" "$FUNC" "${4:-40}"
  exit 0
fi
if [ "$MODE" = win ] || [ "$MODE" = both ]; then
  MEGLER_TESTS_OPT=/O2 MEGLER_TESTS_OUT="$ROOT/build/tests-win-o2" MEGLER_TESTS_ORDER="$ORDER" bash "$ROOT/tools/build-tests.sh" win --no-run --no-libcore > /dev/null
  echo "== x64, /O2"
  "$ROOT/build/tests-win-o2/mg_tests.exe" --dvm-bench "$(cygpath -w "$ROOT/build/guest/armeabi-v7a/dvm")" "$(cygpath -w "$ROOT/build/android9/root")" "$MS"
fi
if [ "$MODE" = arm ] || [ "$MODE" = both ]; then
  bash "$ROOT/tools/build-tests.sh" arm --no-run > /dev/null
  WB="${MEGLER_WSL_BUILD:-~/megler-build}"
  WROOT="/mnt/${ROOT:1:1}${ROOT:2}"
  echo "== qemu-arm, gcc -O1"
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "mkdir -p $WB/android-root && cp -ru '$WROOT/build/android9/root/.' $WB/android-root/ && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --dvm-bench '$WROOT/build/guest/armeabi-v7a/dvm' $WB/android-root $MS 2>&1"
fi
if [ "$MODE" = arm64 ]; then
  bash "$ROOT/tools/build-tests.sh" arm64 --no-run > /dev/null
  bash "$ROOT/tools/dvm-android-root.sh" --abi arm64-v8a > /dev/null
  WB="${MEGLER_WSL_BUILD:-~/megler-build}-arm64"
  WROOT="/mnt/${ROOT:1:1}${ROOT:2}"
  echo "== qemu-aarch64, clang -O1${MEGLER_JIT_THRESHOLD:+, MEGLER_JIT_THRESHOLD=$MEGLER_JIT_THRESHOLD}"
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "mkdir -p $WB/android-root && cp -ru '$WROOT/build/android9/root-arm64-v8a/.' $WB/android-root/ && cp '$WROOT/build/tests-arm64/mg_tests' $WB/mg_tests && qemu-aarch64 $WB/mg_tests --dvm-bench '$WROOT/build/guest/armeabi-v7a/dvm' $WB/android-root $MS 2>&1"
fi
