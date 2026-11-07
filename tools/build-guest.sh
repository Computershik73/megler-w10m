#!/usr/bin/env bash
# Builds the guest test libraries (core/tests/guest/*.c) as real Android
# shared objects of one ABI, into build/guest/<abi>.
#
#   tools/build-guest.sh [armeabi-v7a | arm64-v8a | x86_64 | x86]
#
# armeabi-v7a unless another is named: the phone's, which the tests run under
# qemu-arm and the phone's package carries. The others are desktop Windows'
# (docs/DESKTOP.md), whose tests run them natively: x86_64 on this PC and on
# Linux x86_64 (tools/build-tests.sh linux64).
#
# clang and ld.lld from an LLVM install do it; no NDK is involved. The sources
# declare what they use from Android themselves (core/tests/guest/t_abi.h has
# what differs between the ABIs), and the "system libraries" they are linked
# against are empty stand-ins made here, which is all a linker needs to record
# DT_NEEDED: the loader binds the imports by name at run time.
#
# Libraries, to cover what real ones differ in:
#   libt_basic.so      both hash tables, stack protector, initial-exec TLS; Thumb-2 on armeabi-v7a. On the x86s its
#                      thread-local accesses go through the thread pointer read as bionic's __get_tls() reads it,
#                      `mov %fs:0` (-mno-tls-direct-seg-refs), which the loader redirects (x86/mg_patch_x86.c)
#   libt_basic_arm.so  armeabi-v7a only: the same source in ARM mode, GNU hash only, packed (APS2) and RELR relocations
#   libt_basic_packed.so  the other ABIs: the same source with GNU hash only, packed (APS2) and RELR relocations and
#                      global-dynamic TLS -- the loader's paths libt_basic_arm.so takes on ARM, here on RELA (and x86's REL)
#   libt_dep.so        needs libt_basic.so: its calls go through a PLT
#   libt_jni.so        native methods of a class the Dalvik runtime's tests have in DEX
#   libt_xposed.so     stands in for Aliucord's libaliuhook.so: XposedBridge's natives by their JNI names, a JNI_OnLoad
#                      that fails (t_xposed.c); the runtime loads it without the one and answers the others itself
#   libt_svc.so, libt_svc_diag.so, libt_svc_arm.so
#                      armeabi-v7a only: direct ARM EABI system calls (t_svc.c), Thumb and ARM
#   libt_aaudio.so     AAudio looked up by name
#   libt_go.so         every ABI but x86: one of Go's by its build ID note, a goroutine's stack in its heap given back
#                      with madvise by number (t_go.c); x86's own system calls (int 0x80) are not led out to the core
#   libt_same.so, libt_user.so, and in runtime/ another libt_same.so and libt_ruser.so:
#                      two libraries of one name, the app's and the runtime's, and one
#                      of each kind that needs "libt_same.so" (t_same.c, t_user.c)
# and an app: native-test.apk, a NativeActivity "game" (t_native.c) with a
# manifest and an asset, put together by tools/make-test-apk.py, its library in
# lib/<abi>/.
# And the demo, megler-demo.apk (t_demo.c): the app the package carries for a
# first look at the picture, touch and sound on a phone.
#
# Where armeabi-v7a's go is where they always went: build/guest/armeabi-v7a, the
# stand-ins in build/guest/stubs, the apps' libraries in build/guest/apk and the
# demo in build/guest/demo. Another ABI's stand-ins, apps' libraries and demo go
# into a folder of its name in each of those.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM="${MEGLER_LLVM:-/c/Program Files/LLVM/bin}"
CLANG="$LLVM/clang.exe"
[ -f "$CLANG" ] || { echo "no clang at $CLANG (set MEGLER_LLVM)" >&2; exit 1; }
ABI="${1:-armeabi-v7a}"
SRC="$ROOT/core/tests/guest"
OUT="$ROOT/build/guest/$ABI"
w() { cygpath -w "$1"; }
export MSYS2_ARG_CONV_EXCL="*"

# THUMB: how the libraries are built, Thumb-2 on armeabi-v7a; TLS_IE: libt_basic.so's initial-exec thread-local accesses.
case "$ABI" in
  armeabi-v7a)
    TARGET=(--target=armv7a-linux-androideabi29 -march=armv7-a -mfloat-abi=softfp -mfpu=neon)
    THUMB=(-mthumb)
    TLS_IE=(-ftls-model=initial-exec)
    STUBS="$ROOT/build/guest/stubs"
    APKLIBS="$ROOT/build/guest/apk"
    DEMO="$ROOT/build/guest/demo"
    ;;
  arm64-v8a|x86_64|x86)
    case "$ABI" in
      # (Atomics inline, as an NDK build has them: clang's default calls __aarch64_ldadd4_acq_rel and its kin, which an
      # NDK build links in from compiler-rt and which -nostdlib leaves an import that no library provides.)
      arm64-v8a) TARGET=(--target=aarch64-linux-android29 -mno-outline-atomics); TLS_IE=(-ftls-model=initial-exec) ;;
      x86_64) TARGET=(--target=x86_64-linux-android29); TLS_IE=(-ftls-model=initial-exec -mno-tls-direct-seg-refs) ;;
      x86) TARGET=(--target=i686-linux-android29); TLS_IE=(-ftls-model=initial-exec -mno-tls-direct-seg-refs) ;;
    esac
    THUMB=()
    STUBS="$ROOT/build/guest/stubs/$ABI"
    APKLIBS="$ROOT/build/guest/apk/$ABI"
    DEMO="$ROOT/build/guest/demo/$ABI"
    ;;
  *) echo "usage: tools/build-guest.sh [armeabi-v7a | arm64-v8a | x86_64 | x86]" >&2; exit 2 ;;
esac
mkdir -p "$OUT" "$STUBS"

COMMON=("${TARGET[@]}" -O2 -fPIC -shared -nostdlib -fuse-ld=lld -fno-unwind-tables -fno-asynchronous-unwind-tables
        -fvisibility=default -Wall -Wno-incompatible-library-redeclaration -Wno-builtin-requires-header
        -Wl,-z,relro -Wl,-z,now -Wl,-z,max-page-size=4096 -Wl,--no-rosegment)

echo "int megler_stub;" > "$STUBS/empty.c"
for lib in c m dl log android EGL GLESv2 OpenSLES; do
  "$CLANG" "${TARGET[@]}" -shared -nostdlib -fuse-ld=lld "-Wl,-soname,lib$lib.so" -o "$(w "$STUBS/lib$lib.so")" "$(w "$STUBS/empty.c")"
done
SYS=("-L$(w "$STUBS")" -lc -lm -ldl -llog)

"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -fstack-protector-strong "${TLS_IE[@]}" -Wl,--hash-style=both \
  -Wl,-soname,libt_basic.so -o "$(w "$OUT/libt_basic.so")" "$(w "$SRC/t_basic.c")" "${SYS[@]}"
if [ "$ABI" = armeabi-v7a ]; then
  "$CLANG" "${COMMON[@]}" -marm -ftls-model=global-dynamic -Wl,--hash-style=gnu -Wl,--pack-dyn-relocs=android+relr \
    -Wl,-soname,libt_basic_arm.so -o "$(w "$OUT/libt_basic_arm.so")" "$(w "$SRC/t_basic.c")" "${SYS[@]}"
else
  "$CLANG" "${COMMON[@]}" -ftls-model=global-dynamic -Wl,--hash-style=gnu -Wl,--pack-dyn-relocs=android+relr \
    -Wl,-soname,libt_basic_packed.so -o "$(w "$OUT/libt_basic_packed.so")" "$(w "$SRC/t_basic.c")" "${SYS[@]}"
fi
"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both \
  -Wl,-soname,libt_dep.so -o "$(w "$OUT/libt_dep.so")" "$(w "$SRC/t_dep.c")" "-L$(w "$OUT")" -lt_basic "${SYS[@]}"
# Native methods for the Dalvik runtime's tests: softfp on armeabi-v7a, as an app's library is.
"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both   -Wl,-soname,libt_jni.so -o "$(w "$OUT/libt_jni.so")" "$(w "$SRC/t_jni.c")" "${SYS[@]}"
# And one that stands in for Aliucord's libaliuhook.so: loaded without its JNI_OnLoad, its natives the runtime's own.
"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both   -Wl,-soname,libt_xposed.so -o "$(w "$OUT/libt_xposed.so")" "$(w "$SRC/t_xposed.c")" "${SYS[@]}"
if [ "$ABI" = armeabi-v7a ]; then
  "$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both   -Wl,-soname,libt_svc.so -o "$(w "$OUT/libt_svc.so")" "$(w "$SRC/t_svc.c")" "${SYS[@]}"
  "$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both   -Wl,-soname,libt_svc_diag.so -o "$(w "$OUT/libt_svc_diag.so")" "$(w "$SRC/t_svc.c")" "${SYS[@]}"
  "$CLANG" "${COMMON[@]}" -marm -Wl,--hash-style=both   -Wl,-soname,libt_svc_arm.so -o "$(w "$OUT/libt_svc_arm.so")" "$(w "$SRC/t_svc.c")" "${SYS[@]}"
fi
# AAudio looked up by name, as an app that decodes its own sound does (core/src/ndk/mg_aaudio.c).
"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both   -Wl,-soname,libt_aaudio.so -o "$(w "$OUT/libt_aaudio.so")" "$(w "$SRC/t_aaudio.c")" "${SYS[@]}"
# One of Go's, by its build ID note: a goroutine's stack in its heap given back by number (bionic/mg_libc_mem.c, madvise).
if [ "$ABI" != x86 ]; then
  "$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both -Wl,-soname,libt_go.so -o "$(w "$OUT/libt_go.so")" "$(w "$SRC/t_go.c")" "${SYS[@]}"
fi

# One file name twice: the app's, and the one that stands for a library of the Android root's.
mkdir -p "$OUT/runtime"
"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both -DWHICH=1 -Wl,-soname,libt_same.so -o "$(w "$OUT/libt_same.so")" "$(w "$SRC/t_same.c")" "${SYS[@]}"
"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both -DWHICH=2 -Wl,-soname,libt_same.so -o "$(w "$OUT/runtime/libt_same.so")" "$(w "$SRC/t_same.c")" "${SYS[@]}"
"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both -Wl,-soname,libt_user.so -o "$(w "$OUT/libt_user.so")" "$(w "$SRC/t_user.c")" "-L$(w "$OUT")" -lt_same "${SYS[@]}"
"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both -DRUNTIME -Wl,-soname,libt_ruser.so -o "$(w "$OUT/runtime/libt_ruser.so")" "$(w "$SRC/t_user.c")" "-L$(w "$OUT/runtime")" -lt_same "${SYS[@]}"

# (The APK's libraries in lib/<abi>/: make-test-apk.py's own default is armeabi-v7a's.)
APKABI=()
[ "$ABI" = armeabi-v7a ] || APKABI=(--abi "$ABI")
mkdir -p "$APKLIBS"
"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both   -Wl,-soname,libnative-test.so -o "$(w "$APKLIBS/libnative-test.so")" "$(w "$SRC/t_native.c")" "${SYS[@]}" -landroid -lEGL -lGLESv2 -lOpenSLES
python "$(w "$ROOT/tools/make-test-apk.py")" "$(w "$OUT/native-test.apk")" --package org.megler.test.native --label "Megler Native Test"   --lib-name native-test --lib "$(w "$APKLIBS/libnative-test.so")" --asset "hello.txt=hello asset" ${APKABI[@]+"${APKABI[@]}"}

mkdir -p "$DEMO"
"$CLANG" "${COMMON[@]}" ${THUMB[@]+"${THUMB[@]}"} -Wl,--hash-style=both -Wl,-soname,libmegler-demo.so -o "$(w "$APKLIBS/libmegler-demo.so")" "$(w "$SRC/t_demo.c")" "${SYS[@]}" -landroid -lEGL -lGLESv2 -lOpenSLES
python "$(w "$ROOT/tools/make-test-apk.py")" "$(w "$DEMO/megler-demo.apk")" --package org.megler.demo --label "Megler Demo" --lib-name megler-demo --lib "$(w "$APKLIBS/libmegler-demo.so")" ${APKABI[@]+"${APKABI[@]}"}

ls -la "$OUT" "$DEMO"
