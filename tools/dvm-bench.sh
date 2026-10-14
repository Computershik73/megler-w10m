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
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-both}"
MS="${2:-300}"
bash "$ROOT/tools/dvm-android-root.sh" > /dev/null
if [ "$MODE" = win ] || [ "$MODE" = both ]; then
  MEGLER_TESTS_OPT=/O2 MEGLER_TESTS_OUT="$ROOT/build/tests-win-o2" bash "$ROOT/tools/build-tests.sh" win --no-run --no-libcore > /dev/null
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
