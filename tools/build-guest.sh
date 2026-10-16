#!/usr/bin/env bash
# Builds the guest test libraries (core/tests/guest/*.c) as real Android
# armeabi-v7a shared objects, into build/guest/armeabi-v7a.
#
#   tools/build-guest.sh
#
# clang and ld.lld from an LLVM install do it; no NDK is involved. The sources
# declare what they use from Android themselves, and the "system libraries"
# they are linked against are empty stand-ins made here, which is all a linker
# needs to record DT_NEEDED: the loader binds the imports by name at run time.
#
# Three libraries, to cover what real ones differ in:
#   libt_basic.so      Thumb-2, both hash tables, stack protector, initial-exec TLS
#   libt_basic_arm.so  the same source in ARM mode, GNU hash only, packed (APS2) and RELR relocations
#   libt_dep.so        Thumb-2, needs libt_basic.so: its calls go through a PLT
#   libt_jni.so        native methods of a class the Dalvik runtime's tests have in DEX
#   libt_same.so, libt_user.so, and in runtime/ another libt_same.so and libt_ruser.so:
#                      two libraries of one name, the app's and the runtime's, and one
#                      of each kind that needs "libt_same.so" (t_same.c, t_user.c)
# and an app: native-test.apk, a NativeActivity "game" (t_native.c) with a
# manifest and an asset, put together by tools/make-test-apk.py.
# And the demo, build/guest/demo/megler-demo.apk (t_demo.c): the app the package
# carries for a first look at the picture, touch and sound on a phone.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LLVM="${MEGLER_LLVM:-/c/Program Files/LLVM/bin}"
CLANG="$LLVM/clang.exe"
[ -f "$CLANG" ] || { echo "no clang at $CLANG (set MEGLER_LLVM)" >&2; exit 1; }
SRC="$ROOT/core/tests/guest"
OUT="$ROOT/build/guest/armeabi-v7a"
STUBS="$ROOT/build/guest/stubs"
mkdir -p "$OUT" "$STUBS"
w() { cygpath -w "$1"; }
export MSYS2_ARG_CONV_EXCL="*"

TARGET=(--target=armv7a-linux-androideabi29 -march=armv7-a -mfloat-abi=softfp -mfpu=neon)
COMMON=("${TARGET[@]}" -O2 -fPIC -shared -nostdlib -fuse-ld=lld -fno-unwind-tables -fno-asynchronous-unwind-tables
        -fvisibility=default -Wall -Wno-incompatible-library-redeclaration -Wno-builtin-requires-header
        -Wl,-z,relro -Wl,-z,now -Wl,-z,max-page-size=4096 -Wl,--no-rosegment)

echo "int megler_stub;" > "$STUBS/empty.c"
for lib in c m dl log android EGL GLESv2 OpenSLES; do
  "$CLANG" "${TARGET[@]}" -shared -nostdlib -fuse-ld=lld "-Wl,-soname,lib$lib.so" -o "$(w "$STUBS/lib$lib.so")" "$(w "$STUBS/empty.c")"
done
SYS=("-L$(w "$STUBS")" -lc -lm -ldl -llog)

"$CLANG" "${COMMON[@]}" -mthumb -fstack-protector-strong -ftls-model=initial-exec -Wl,--hash-style=both \
  -Wl,-soname,libt_basic.so -o "$(w "$OUT/libt_basic.so")" "$(w "$SRC/t_basic.c")" "${SYS[@]}"
"$CLANG" "${COMMON[@]}" -marm -ftls-model=global-dynamic -Wl,--hash-style=gnu -Wl,--pack-dyn-relocs=android+relr \
  -Wl,-soname,libt_basic_arm.so -o "$(w "$OUT/libt_basic_arm.so")" "$(w "$SRC/t_basic.c")" "${SYS[@]}"
"$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both \
  -Wl,-soname,libt_dep.so -o "$(w "$OUT/libt_dep.so")" "$(w "$SRC/t_dep.c")" "-L$(w "$OUT")" -lt_basic "${SYS[@]}"
# Native methods for the Dalvik runtime's tests: softfp, as an app's library is.
"$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both   -Wl,-soname,libt_jni.so -o "$(w "$OUT/libt_jni.so")" "$(w "$SRC/t_jni.c")" "${SYS[@]}"
"$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both   -Wl,-soname,libt_svc.so -o "$(w "$OUT/libt_svc.so")" "$(w "$SRC/t_svc.c")" "${SYS[@]}"

# One file name twice: the app's, and the one that stands for a library of the Android root's.
mkdir -p "$OUT/runtime"
"$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both -DWHICH=1 -Wl,-soname,libt_same.so -o "$(w "$OUT/libt_same.so")" "$(w "$SRC/t_same.c")" "${SYS[@]}"
"$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both -DWHICH=2 -Wl,-soname,libt_same.so -o "$(w "$OUT/runtime/libt_same.so")" "$(w "$SRC/t_same.c")" "${SYS[@]}"
"$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both -Wl,-soname,libt_user.so -o "$(w "$OUT/libt_user.so")" "$(w "$SRC/t_user.c")" "-L$(w "$OUT")" -lt_same "${SYS[@]}"
"$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both -DRUNTIME -Wl,-soname,libt_ruser.so -o "$(w "$OUT/runtime/libt_ruser.so")" "$(w "$SRC/t_user.c")" "-L$(w "$OUT/runtime")" -lt_same "${SYS[@]}"

APKLIBS="$ROOT/build/guest/apk"
mkdir -p "$APKLIBS"
"$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both   -Wl,-soname,libnative-test.so -o "$(w "$APKLIBS/libnative-test.so")" "$(w "$SRC/t_native.c")" "${SYS[@]}" -landroid -lEGL -lGLESv2 -lOpenSLES
python "$(w "$ROOT/tools/make-test-apk.py")" "$(w "$OUT/native-test.apk")" --package org.megler.test.native --label "Megler Native Test"   --lib-name native-test --lib "$(w "$APKLIBS/libnative-test.so")" --asset "hello.txt=hello asset"

DEMO="$ROOT/build/guest/demo"
mkdir -p "$DEMO"
"$CLANG" "${COMMON[@]}" -mthumb -Wl,--hash-style=both -Wl,-soname,libmegler-demo.so -o "$(w "$APKLIBS/libmegler-demo.so")" "$(w "$SRC/t_demo.c")" "${SYS[@]}" -landroid -lEGL -lGLESv2 -lOpenSLES
python "$(w "$ROOT/tools/make-test-apk.py")" "$(w "$DEMO/megler-demo.apk")" --package org.megler.demo --label "Megler Demo" --lib-name megler-demo --lib "$(w "$APKLIBS/libmegler-demo.so")"

ls -la "$OUT" "$DEMO"
