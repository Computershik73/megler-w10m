#!/usr/bin/env bash
# The measurements docs/JAVA.md quotes about Android 9's libcore under the
# Dalvik runtime. Nothing here is a test: each prints what it found.
#
#   tools/dvm-report.sh natives [arm]
#       every native method the boot class path declares, and who answers for
#       it: the runtime in C, a library of libcore's, or nobody
#   tools/dvm-report.sh init [arm]
#       the static initialiser of every class of the boot class path, run:
#       which fail, and what each failure comes down to
#   tools/dvm-report.sh link <file.dex>...
#       what those DEX files need of libcore and do not find: their classes
#       linked, and every class, method and field they name looked up
#   tools/dvm-report.sh link-framework
#       the same for Android 9's own framework (build/android9/boot/framework*.dex)
#
# They run in the PC's build of the tests (build/tests-win/mg_tests.exe), where
# libcore's native libraries cannot be loaded; `arm` runs the first two under
# qemu-arm in WSL, with those libraries. The folder libcore is started from is
# build/android9/root (tools/dvm-android-root.sh). MEGLET_WSL_BUILD names the
# WSL folder of that build, as for tools/build-tests.sh (~/meglet-build).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WB="${MEGLET_WSL_BUILD:-}"
[ -n "$WB" ] || WB='~/meglet-build'
WHAT="${1:-}"
shift || true
w() { cygpath -w "$1"; }
bash "$ROOT/tools/dvm-android-root.sh" > /dev/null
EXE="$ROOT/build/tests-win/mg_tests.exe"
A="$(w "$ROOT/build/android9/root")"
pc() {
  [ -f "$EXE" ] || bash "$ROOT/tools/build-tests.sh" win --no-run > /dev/null
  "$EXE" "$@"
}
arm() { # the mode
  local wroot="/mnt/${ROOT:1:1}${ROOT:2}"
  bash "$ROOT/tools/build-tests.sh" arm --no-run > /dev/null
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "mkdir -p $WB/android-root && cp -ru '$wroot/build/android9/root/.' $WB/android-root/ && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests $1 $WB/android-root 2>&1"
}
case "$WHAT" in
  natives|init)
    if [ "${1:-}" = arm ]; then arm "--dvm-$WHAT"; else pc "--dvm-$WHAT" "$A"; fi ;;
  link)
    [ $# -gt 0 ] || { echo "link: which DEX files?" >&2; exit 2; }
    FILES=()
    for f in "$@"; do FILES+=("$(w "$f")"); done
    pc --dvm-link "$A" "${FILES[@]}" ;;
  link-framework)
    B="$ROOT/build/android9/boot"
    pc --dvm-link "$A" "$(w "$B/framework.dex")" "$(w "$B/framework-classes2.dex")" "$(w "$B/framework-classes3.dex")" ;;
  *)
    sed -n '2,20p' "$0"; exit 2 ;;
esac
