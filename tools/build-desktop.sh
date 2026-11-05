#!/usr/bin/env bash
# Builds Megler for desktop Windows on x64, ARM64 or x86 (docs/DESKTOP.md).
#
#   tools/build-desktop.sh x64|arm64|x86 tests [--no-run] [-- arguments for mg_tests]
#   tools/build-desktop.sh x64|arm64|x86 app
#
#   tests   the core's ELF with the tests in it (build/desktop-<arch>/megler-core.so) and the test program that loads it
#           (build/desktop-<arch>/mg_tests.exe); then, on a machine of that processor, runs it as tools/build-tests.sh
#           win runs the x64 build of the tests -- everything that needs no guest code, the Dalvik runtime on its own
#           java.lang -- and the guest-code tests on the guest test libraries of the processor's ABI
#           (build/guest/x86_64, arm64-v8a or x86: tools/build-guest.sh <abi>, built here when they are not there).
#   app     what tools/build-appx.sh links Megler.exe of for that processor: the core's ELF as the package carries it
#           (build/desktop-<arch>/app/megler-core.so: the tests the Self-test runs, no test program's main) and Megler.exe's
#           half of the core compiled for an app container (build/desktop-<arch>/app/coff/*.obj).
#
# The core is compiled by clang for Android's ABI of the processor (--target=x86_64-linux-android29 and so on) against
# the bionic headers of the NDK's sysroot, into an ELF shared object with ld.lld; Megler.exe's half -- the host layer, the
# loader of that ELF, the C library bridge, and the glue tools/desktop-stubs.py writes from the ELF's own symbols -- by
# clang-cl for Windows. See docs/DESKTOP.md for why the core is an ELF.
#
# Environment:
#   MEGLER_NDK    the NDK whose sysroot and compiler-rt the core is built with (C:\Users\User\Documents\android-ndk-r21e)
#   MEGLER_LLVM   LLVM's bin: clang, clang-cl, ld.lld, lld-link, llvm-nm (C:\Program Files\LLVM\bin)
#   MEGLER_VC     the MSVC toolset whose libraries Megler.exe links (the newest under VS 2022)
#   MEGLER_JOBS   compilers at once (12)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARCH="${1:-}"
WHAT="${2:-tests}"
shift 2 || true
RUN=1
EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --no-run) RUN=0 ;;
    --) shift; EXTRA=("$@"); break ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done
case "$ARCH" in
  x64) TRIPLE=x86_64-linux-android29; RT=x86_64; WIN=x86_64-pc-windows-msvc; VCARCH=x64; MACHINE=X64 ;;
  arm64) TRIPLE=aarch64-linux-android29; RT=aarch64; WIN=aarch64-pc-windows-msvc; VCARCH=arm64; MACHINE=ARM64 ;;
  x86) TRIPLE=i686-linux-android29; RT=i686; WIN=i686-pc-windows-msvc; VCARCH=x86; MACHINE=X86 ;;
  *) echo "usage: tools/build-desktop.sh x64|arm64|x86 tests [--no-run]" >&2; exit 2 ;;
esac
case "$WHAT" in tests|app) ;; *) echo "tests or app, not $WHAT" >&2; exit 2 ;; esac

NDK="${MEGLER_NDK:-/c/Users/User/Documents/android-ndk-r21e}"
LLVM="${MEGLER_LLVM:-/c/Program Files/LLVM/bin}"
JOBS="${MEGLER_JOBS:-12}"
SYSROOT="$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot"
BUILTINS="$(ls "$NDK"/toolchains/llvm/prebuilt/windows-x86_64/lib64/clang/*/lib/linux/libclang_rt.builtins-$RT-android.a 2>/dev/null | head -1)"
[ -d "$SYSROOT/usr/include" ] || { echo "no NDK sysroot at $SYSROOT (set MEGLER_NDK)" >&2; exit 1; }
[ -f "$BUILTINS" ] || { echo "no compiler-rt builtins for $RT in $NDK" >&2; exit 1; }
[ -x "$LLVM/clang.exe" ] || { echo "no clang in $LLVM (set MEGLER_LLVM)" >&2; exit 1; }

OUT="$ROOT/build/desktop-$ARCH"
mkdir -p "$OUT/elf" "$OUT/coff" "$OUT/app/coff"
cd "$ROOT"
# (clang.exe is handed the ELF's paths as msys writes them, /c/...: tools/build-appx.sh, which calls this, has turned
# msys's conversion of them off for its own tools. Megler.exe's half turns it off again below.)
unset MSYS2_ARG_CONV_EXCL

# MARK: the core's ELF

# SQLite and libwebp's decoder go in where their sources are, as in the other builds (tools/sqlite-fetch.sh, webp-fetch.sh).
OPT=()
[ -f build/sqlite/sqlite3.c ] && OPT+=(-DMG_HAVE_SQLITE=1 -Ibuild/sqlite)
[ -f build/webp/src/dec/webp_dec.c ] && OPT+=(-DMG_HAVE_WEBP=1 -Ibuild/webp)
ELF_FLAGS=(--target=$TRIPLE "--sysroot=$SYSROOT" -std=gnu11 -O2 -g -fPIC -fvisibility=hidden -femulated-tls
  -fno-stack-protector -fstack-clash-protection -fno-omit-frame-pointer -ffunction-sections -fdata-sections
  -D_GNU_SOURCE -DMG_CORE_ELF=1 -Icore/third_party/zlib "${OPT[@]}"
  -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers -Werror=incompatible-function-pointer-types
  -Werror=implicit-function-declaration -Werror=int-conversion)
# (ARM64: x18 is Windows' own, the thread's TEB, and the core's code must never touch it; Android's code does not either.)
[ "$ARCH" = arm64 ] && ELF_FLAGS+=(-ffixed-x18)
# (x86: Windows' callers leave the stack at 4 bytes; what Windows calls realigns it (MG_HOSTCB, MEGLER_API).)
mapfile -t SRCS < <(find core/src core/tests -maxdepth 3 -name '*.c' ! -path 'core/tests/guest/*' ! -path 'core/src/host/*' ! -path 'core/src/appx/*' | sort)
mapfile -t ZSRCS < <(find core/third_party/zlib -maxdepth 1 -name '*.c' ! -name 'gz*.c' | sort)
[ "$(cat "$OUT/elf/flags" 2>/dev/null)" = "${ELF_FLAGS[*]}" ] || rm -f "$OUT/elf/"*.o
echo "${ELF_FLAGS[*]}" > "$OUT/elf/flags"
newest_header="$(find core/include core/src core/tests -name '*.h' -o -name '*.inc' | xargs ls -t | head -1)"
compile_elf() {
  local src="$1" extra="$2" obj="$OUT/elf/$(echo "$1" | tr '/' '_').o"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$newest_header" -nt "$obj" ]; then
    "$LLVM/clang.exe" "${ELF_FLAGS[@]}" $extra -c "$src" -o "$obj" 2> "$obj.log" || { cat "$obj.log" >&2; echo "FAILED: $src" >&2; return 1; }
    grep -q 'warning:' "$obj.log" && grep -E 'warning:' "$obj.log" | head -5 >&2
  fi
  return 0
}
fails=0
for s in "${SRCS[@]}"; do
  compile_elf "$s" "" || fails=$((fails + 1)) &
  while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do sleep 0.1; done
done
for s in "${ZSRCS[@]}"; do
  compile_elf "$s" "-w" || fails=$((fails + 1)) &
  while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do sleep 0.1; done
done
wait
ls "$OUT/elf/"*.o > /dev/null
missing=0
for s in "${SRCS[@]}" "${ZSRCS[@]}"; do
  [ -f "$OUT/elf/$(echo "$s" | tr '/' '_').o" ] || { echo "not compiled: $s" >&2; missing=1; }
done
[ "$missing" = 0 ] || exit 1
# The ELF: one shared object Megler.exe's loader takes as it is (host/win32/mg_core_loader.c): each segment on pages of
# its own, both hash tables, plain relocations, everything bound at load. Undefined names are what Megler.exe gives.
# (The app's is the same but for the test program's main.)
CORE="$OUT/megler-core.so"
GLUE="$OUT/coff/core_glue.c"
ELF_OBJS=("$OUT/elf/"*.o)
if [ "$WHAT" = app ]; then
  CORE="$OUT/app/megler-core.so"
  GLUE="$OUT/app/coff/core_glue.c"
  ELF_OBJS=()
  for o in "$OUT/elf/"*.o; do [ "$(basename "$o")" = core_tests_mg_tests_main.c.o ] || ELF_OBJS+=("$o"); done
fi
"$LLVM/ld.lld.exe" -shared -soname megler-core.so -o "$CORE" "${ELF_OBJS[@]}" "$BUILTINS" \
  --hash-style=both -z max-page-size=4096 -z separate-loadable-segments --pack-dyn-relocs=none -z now -z relro \
  --exclude-libs=ALL --gc-sections --build-id=sha1
echo "built $CORE ($(stat -c %s "$CORE") bytes)"

# MARK: Megler.exe's half

python tools/desktop-stubs.py "$CORE" "$ARCH" "$GLUE" "$LLVM/llvm-nm.exe"

if [ -z "${MEGLER_VC:-}" ]; then
  for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe; do
    [ -f "$cl" ] && MEGLER_VC="${cl%/bin/Hostx64/x64/cl.exe}"
  done
fi
[ -n "${MEGLER_VC:-}" ] || { echo "no MSVC found (set MEGLER_VC)" >&2; exit 1; }
SDK="/c/Program Files (x86)/Windows Kits/10"
SDKV="${MEGLER_SDK_VERSION:-10.0.22621.0}"
w() { cygpath -w "$1"; }
export INCLUDE="$(w "$MEGLER_VC/include");$(w "$SDK/Include/$SDKV/ucrt");$(w "$SDK/Include/$SDKV/shared");$(w "$SDK/Include/$SDKV/um")"
export LIB="$(w "$MEGLER_VC/lib/$VCARCH");$(w "$SDK/Lib/$SDKV/ucrt/$VCARCH");$(w "$SDK/Lib/$SDKV/um/$VCARCH")"
export MSYS2_ARG_CONV_EXCL="*"
HOST_SRCS=(core/src/host/mg_host_win32.c core/src/host/mg_host_common.c core/src/host/win32/mg_core_loader.c
           core/src/host/win32/mg_crt_bridge.c core/src/host/win32/mg_host_base.c "$GLUE")
[ "$WHAT" = tests ] && HOST_SRCS+=(core/src/host/win32/mg_desktop_tests.c)
for s in core/src/appx/*.c; do HOST_SRCS+=("$s"); done
COFF_FLAGS=(--target=$WIN /nologo /c /O2 /MT /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS /DMG_ELF_HOST=1
            /Icore/src /Icore/third_party/zlib -Wno-microsoft-anon-tag -Wno-unused-parameter)
COFF_OUT="$OUT/coff"
if [ "$WHAT" = app ]; then
  # As the phone's package compiles the core: for an app container, whose headers show what an app may call.
  COFF_FLAGS+=(/DWINAPI_FAMILY=WINAPI_FAMILY_APP /DWIN32 /D_WIN32 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DNDEBUG /Z7)
  COFF_OUT="$OUT/app/coff"
fi
rm -f "$COFF_OUT/"*.obj
objs=()
for s in "${HOST_SRCS[@]}" "${ZSRCS[@]}"; do
  case "$s" in /*) src="$s" ;; *) src="$ROOT/$s" ;; esac
  obj="$COFF_OUT/$(basename "$(dirname "$s")")_$(basename "${s%.c}").obj"
  "$LLVM/clang-cl.exe" "${COFF_FLAGS[@]}" "/Fo$(w "$obj")" "$(w "$src")" || { echo "FAILED: $s" >&2; exit 1; }
  objs+=("$(w "$obj")")
done
if [ "$WHAT" = app ]; then
  echo "built ${#objs[@]} objects of Megler.exe's half of the core in $COFF_OUT"
  exit 0
fi
"$LLVM/lld-link.exe" /nologo /machine:$MACHINE /subsystem:console "/out:$(w "$OUT/mg_tests.exe")" "${objs[@]}" \
  bcrypt.lib synchronization.lib ws2_32.lib iphlpapi.lib ole32.lib oleaut32.lib d3d11.lib dxgi.lib
echo "built $OUT/mg_tests.exe"

if [ "$RUN" = 1 ]; then
  case "$ARCH:$PROCESSOR_ARCHITECTURE" in
    x64:AMD64|x86:AMD64|x86:x86|arm64:ARM64|x64:ARM64|x86:ARM64) ;;
    *) echo "(not run: this machine's processor is $PROCESSOR_ARCHITECTURE)"; exit 0 ;;
  esac
  # The guest test libraries of the processor's ABI, built again when any of their sources is newer than the first of them.
  case "$ARCH" in x64) GUEST_ABI=x86_64 ;; arm64) GUEST_ABI=arm64-v8a ;; *) GUEST_ABI=x86 ;; esac
  GUEST="$ROOT/build/guest/$GUEST_ABI"
  { [ -f "$GUEST/libt_basic.so" ] && [ -z "$(find "$ROOT/core/tests/guest" -type f -newer "$GUEST/libt_basic.so" -print -quit)" ]; } \
    || bash "$ROOT/tools/build-guest.sh" "$GUEST_ABI" > /dev/null
  rm -rf "$OUT/scratch"
  "$OUT/mg_tests.exe" "$(w "$GUEST")" "$(w "$OUT/scratch")" --dvm "$(w "$ROOT/build/guest/armeabi-v7a/dvm")" ${EXTRA[@]+"${EXTRA[@]}"}
fi
