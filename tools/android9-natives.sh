#!/bin/bash
# The native half of Android 9's libcore, compiled for plain 32-bit ARMv7-A
# with an Android NDK, into build/android9/lib.
#
#   tools/android9-natives.sh [clean]          (from Git Bash)
#
# Why: the Java runtime (docs/JAVA.md, "The boot class path: libcore") runs
# Android 9's own bytecode, and that bytecode's native methods are in
# libjavacore.so and libopenjdk.so. Google's Android 9 images carry those for
# arm64 only, and a 32-bit phone build of them is compiled for ARMv8 cores. So
# they are built here from the sources tools/android9-sources.sh fetched.
#
# What is built, in this order:
#
#   libicuuc.so, libicui18n.so   ICU 60 (external/icu). No data linked in:
#                                libjavacore hands ICU the data file at run
#                                time, as on Android (see below).
#   libnativehelper.so           JNIHelp, JniConstants, JniInvocation
#   libexpat.so                  the XML parser under org.apache.harmony.xml
#   libbase.a, libfdlibm.a       static, linked into the two below as
#                                libcore/NativeCode.bp says
#   libjavacore.so               libcore/luni/src/main/native
#   libopenjdk.so                libcore/ojluni/src/main/native
#   libjavacrypto.so             conscrypt's native half (external/conscrypt,
#                                common/src/jni): NativeCrypto's methods over
#                                BoringSSL, TLS included
#   libcrypto.so, libssl.so      not built: Android 9's own for ARMv7 (the VNDK
#                                snapshot's), copied. They alone are compiled
#                                for NEON, as their folder's name says; every
#                                Windows phone's processor has it.
#
# Not built: libopenjdkjvm.so and libart.so (the runtime itself answers their
# functions), libc, libm, libdl, liblog, libz (Megler's layer is those). Where a
# library has to be linked against one of them, it is linked against the NDK's
# stub, or against a stub made here from a list of names
# (tools/android9-natives/*.syms, stub.py), so that the result imports the same
# names from the same libraries as Android's does. Two entries of Android's
# DT_NEEDED lists are not in these: libc++.so (see below), and libutils.so,
# from which Android's own libraries import nothing; nor is libopenjdk's
# libssl.so, for the same reason (libjavacrypto's is).
#
# Source lists and each module's own flags are read out of AOSP's Android.bp
# files (tools/android9-natives/bp.py), not globbed. Soong's global flags are
# not in the fetched tree (build/soong is not fetched); GLOBAL below repeats
# the ones that change the code, as Android 9's cc/config/global.go and
# arm_device.go have them, from knowledge of those files and not from a copy.
# -Werror and the warning flags are dropped: a newer compiler than Android 9's
# reads this code.
#
# The target is ARMv7-A, Thumb-2, VFPv3-D16, softfp: the NDK's own default for
# armv7a, said out loud. No NEON, nothing of ARMv8; the script checks the
# build attributes of what it linked.
#
# How the libraries are linked follows from what Megler's linker
# (core/src/ld/mg_ld.c, core/src/arm/mg_patch.c) does:
#
#   - ld.lld: its PLT entries are a shape mg_patch.c recognises and rewrites
#     (and the shape the core's own test libraries have).
#   - Plain REL relocations, no packing (no APS2, no RELR): mg_ld.c reads all
#     three, and the plain table is the one with nothing to go wrong in.
#   - Both hash tables: lookup uses the GNU one, and the SysV one gives
#     mg_ld.c the exact number of symbols instead of a derived one.
#   - Emulated TLS (the compiler's default below API 29), so no PT_TLS and no
#     TLS relocations; thread-local data goes through pthread keys, as on
#     Android 9 itself.
#   - No IFUNCs, no text relocations (a warning made fatal), 4 KB pages.
#   - --no-undefined: every import is a name some library on the link line
#     defines.
#   - libc++ is linked statically into each library and hidden. Megler's linker
#     has one name space keyed by file name and looks in the APK first
#     (read_lib_file), and most NDK apps bring a libc++_shared.so of their own
#     NDK's version: a second one beside these libraries would shadow it or be
#     shadowed by it. Android itself avoids that with a separately named
#     platform library in its own linker namespace. Nothing of the C++ runtime
#     crosses a boundary between these libraries (no std:: type in an exported
#     interface, no exceptions: all of this is compiled -fno-exceptions, as
#     Soong does), so separate hidden copies are safe here.
#
# ICU's data: Android's ICU is built without ICU_DATA_DIR and with a stub in
# place of linked-in data (icudt60_dat, an empty table of contents). When
# libjavacore is loaded its ICURegistration (libcore_icu_ICU.cpp) maps
# $ANDROID_ROOT/usr/icu/icudt60l.dat and, if it exists,
# $ANDROID_DATA/misc/zoneinfo/current/icu/icu_tzdata.dat, and gives them to ICU
# with udata_setCommonData(). That is kept as it is: the runtime has to set
# ANDROID_ROOT and ANDROID_DATA and have build/android9/system/usr/icu/icudt60l.dat
# under the first. The stub's source is not among what was fetched; a stand-in
# with the same 56 bytes is tools/android9-natives/icu_stubdata.cpp.
#
# Objects go to build/android9/obj/<module>, libraries with their symbol
# tables to build/android9/obj/unstripped, stripped ones to build/android9/lib.
# A source is compiled again when it is newer than its object, a module when
# its flags changed, a library is linked again when anything before it was
# compiled or this script is newer than it. Afterwards:
#
#   python tools/android9-natives/report.py    compares the result with Android's own
#                                              libraries, the bytecode and the layer
#   tools/android9-natives/loadtest.sh         has Megler's linker load it under qemu-arm
#
#   MEGLER_NDK        the NDK                 (C:\Users\User\Documents\android-ndk-r21e)
#   MEGLER_NDK_HOST   its prebuilt host tag   (windows-x86_64)
#   MEGLER_JOBS       compilers at once       (8)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SELF="$ROOT/tools/android9-natives.sh"
cd "$ROOT"      # every path below is relative to here: nothing for msys to rewrite on the way to clang.exe
A=build/android9/aosp
OBJ=build/android9/obj
LIB=build/android9/lib
UNSTRIPPED=$OBJ/unstripped
STUBS=$OBJ/stubs
SHIM=tools/android9-natives
NDK="${MEGLER_NDK:-/c/Users/User/Documents/android-ndk-r21e}"
HOST="${MEGLER_NDK_HOST:-windows-x86_64}"
JOBS="${MEGLER_JOBS:-8}"
TC="$NDK/toolchains/llvm/prebuilt/$HOST"
EXE=""; case "$HOST" in windows-*) EXE=".exe" ;; esac
CC="$TC/bin/clang$EXE"
CXX="$TC/bin/clang++$EXE"
AR="$TC/bin/llvm-ar$EXE"
NM="$TC/bin/llvm-nm$EXE"
STRIP="$TC/bin/llvm-strip$EXE"
READELF="$TC/bin/llvm-readelf$EXE"
GNU_READELF="$TC/bin/arm-linux-androideabi-readelf$EXE"
SYSLIB="$TC/sysroot/usr/lib/arm-linux-androideabi/28"

die() { echo "android9-natives: $*" >&2; exit 1; }
[ -x "$CC" ] || die "no clang at $CC (set MEGLER_NDK to an Android NDK, r21 or near it)"
[ -f "$SYSLIB/libc.so" ] || die "the NDK has no API 28 libraries for ARM ($SYSLIB)"
[ -f "$A/libcore/NativeCode.bp" ] || die "no $A/libcore: run tools/android9-sources.sh first"
VNDK="$A/prebuilts/vndk/v28/arm/arch-arm-armv7-a-neon/shared/vndk-core"
for f in libcrypto.so libssl.so; do [ -f "$VNDK/$f" ] || die "no $VNDK/$f: run tools/android9-sources.sh first"; done
[ -f "$A/external/conscrypt/Android.bp" ] || die "no $A/external/conscrypt: run tools/android9-sources.sh first"

if [ "${1:-}" = clean ]; then
  rm -rf "$OBJ" "$LIB"/*.so "$LIB/report"       # the libraries and what report.py wrote; anything else lying there stays
  echo "removed $OBJ, the libraries in $LIB and $LIB/report"
  exit 0
fi
mkdir -p "$OBJ" "$LIB" "$UNSTRIPPED" "$STUBS"
bp() { python "$SHIM/bp.py" "$@" | tr -d '\r'; }
# Below, bp is read through process substitution, where a failure would pass for an empty list. So every question
# is asked once here, where a failure stops the script.
for q in "external/icu/icu4c/source/common/Android.bp libicuuc" "external/icu/icu4c/source/i18n/Android.bp libicui18n" \
         "libnativehelper/Android.bp libnativehelper" "external/expat/Android.bp libexpat" "system/core/base/Android.bp libbase" \
         "external/fdlibm/Android.bp libfdlibm" "libcore/NativeCode.bp libjavacore" "libcore/NativeCode.bp libopenjdk" \
         "libcore/luni/src/main/native/Android.bp luni_native_srcs" "libcore/ojluni/src/main/native/Android.bp libopenjdk_native_srcs" \
         "external/conscrypt/Android.bp libjavacrypto"; do
  set -- $q
  [ -n "$(bp "$A/$1" "$2" srcs)" ] || die "cannot read the sources of $2 out of $A/$1"
done

# ------------------------------------------------------------------ flags

ARCH=(--target=armv7a-linux-androideabi28 -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp -mthumb)
# Soong's own, for every module of an ARM device (see the header).
GLOBAL=(-DANDROID -DNDEBUG -UDEBUG -Os -fPIC -fno-exceptions -fno-strict-aliasing -ffunction-sections -fdata-sections
        -fno-short-enums -funwind-tables -fstack-protector-strong -fomit-frame-pointer -D_FORTIFY_SOURCE=2 -Wa,--noexecstack)
GLOBAL_C=(-std=gnu99)
GLOBAL_CXX=(-std=gnu++14 -fvisibility-inlines-hidden)
# Quiet, except for what would silently miscompile C: a function nobody declared, a pointer squeezed through an int.
QUIET=(-Wno-everything -Werror=implicit-function-declaration -Werror=int-conversion)

# Headers. Platform ones come from the fetched tree, before the NDK's own of the same name.
I_JNI=(-I$A/libnativehelper/include_jni -I$A/libnativehelper/include -I$A/libnativehelper/header_only_include -I$A/libnativehelper/platform_include)
I_LOG=(-I$A/system/core/liblog/include -I$A/system/core/libcutils/include)
I_BASE=(-I$A/system/core/base/include)
I_ICU=(-I$A/external/icu/icu4c/source/common -I$A/external/icu/icu4c/source/i18n)
I_CORE=(-I$A/libcore/include "${I_JNI[@]}" "${I_LOG[@]}" "${I_BASE[@]}" "${I_ICU[@]}" -I$A/external/expat/lib
        -I$A/external/boringssl/src/include -I$A/external/zlib)

# A module's flags as its Android.bp gives them, less -Werror and the warning switches.
bp_flags() {
  local f
  bp "$1" "$2" "$3" | while read -r f; do
    case "$f" in -Wa,*|-Wl,*) echo "$f" ;; -W*) ;; *) echo "$f" ;; esac
  done
}

# ------------------------------------------------------------------ compiling

# objects MODULE: compiles SRCS (paths from the repository's root) with MOD_C / MOD_CXX after the global flags.
# Leaves $OBJ/MODULE/objects.rsp, the list a link or an archive takes.
DIRTY=0           # 1 once anything was compiled or a stub changed: what is linked from then on is linked anew
objects() {
  local name="$1" dir="$OBJ/$1" src base obj n failed=0
  local -A seen=()
  mkdir -p "$dir"
  printf '%s\n' "${ARCH[@]}" "${GLOBAL[@]}" "${GLOBAL_C[@]}" "${MOD_C[@]}" "${QUIET[@]}" > "$dir/c.rsp.new"
  printf '%s\n' "${ARCH[@]}" "${GLOBAL[@]}" "${GLOBAL_CXX[@]}" "${MOD_CXX[@]}" "${QUIET[@]}" > "$dir/cxx.rsp.new"
  if ! cmp -s "$dir/c.rsp.new" "$dir/c.rsp" || ! cmp -s "$dir/cxx.rsp.new" "$dir/cxx.rsp"; then
    rm -f "$dir"/*.o
    mv "$dir/c.rsp.new" "$dir/c.rsp"; mv "$dir/cxx.rsp.new" "$dir/cxx.rsp"
  else
    rm -f "$dir/c.rsp.new" "$dir/cxx.rsp.new"
  fi
  : > "$dir/todo.c"; : > "$dir/todo.cxx"; : > "$dir/objects.rsp"; : > "$dir/compile.log"
  for src in "${SRCS[@]}"; do
    [ -f "$src" ] || die "$name: no source $src"
    base="$(basename "$src")"; obj="$dir/${base%.*}.o"
    [ -z "${seen[$obj]:-}" ] || die "$name: two sources named $base"
    seen[$obj]=1
    echo "$obj" >> "$dir/objects.rsp"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
      case "$src" in *.c) echo "$src -o $obj" >> "$dir/todo.c" ;; *) echo "$src -o $obj" >> "$dir/todo.cxx" ;; esac
    fi
  done
  n=$(cat "$dir/todo.c" "$dir/todo.cxx" | wc -l)
  if [ "$n" != 0 ]; then
    DIRTY=1
    echo "    $name: compiling $n of ${#SRCS[@]}"
    xargs -r -P "$JOBS" -L 1 "$CC" "@$dir/c.rsp" -c < "$dir/todo.c" >> "$dir/compile.log" 2>&1 || failed=1
    xargs -r -P "$JOBS" -L 1 "$CXX" "@$dir/cxx.rsp" -c < "$dir/todo.cxx" >> "$dir/compile.log" 2>&1 || failed=1
    if [ "$failed" = 1 ]; then
      echo "android9-natives: $name did not compile; the first errors (all of it: $dir/compile.log):" >&2
      grep -E "error:|fatal error:" "$dir/compile.log" | head -30 >&2
      exit 1
    fi
  fi
}

archive() {       # archive MODULE: $OBJ/MODULE/libMODULE.a from its objects
  local dir="$OBJ/$1"
  [ "$DIRTY" = 1 ] || [ ! -f "$dir/lib$1.a" ] || return 0
  rm -f "$dir/lib$1.a"
  "$AR" rcs "$dir/lib$1.a" "@$dir/objects.rsp" || die "$1: llvm-ar failed"
}

# ------------------------------------------------------------------ linking

LDFLAGS=(-shared -fuse-ld=lld -nodefaultlibs
         -Wl,--no-undefined -Wl,--gc-sections -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,-z,max-page-size=4096
         -Wl,--hash-style=both -Wl,--pack-dyn-relocs=none -Wl,--build-id=sha1 -Wl,--warn-shared-textrel -Wl,--fatal-warnings
         -Wl,--exclude-libs,libunwind.a -Wl,--exclude-libs,libcompiler_rt-extras.a -Wl,--exclude-libs,libgcc_real.a
         -Wl,--exclude-libs,libatomic.a -Wl,--exclude-libs,libc++_static.a -Wl,--exclude-libs,libc++abi.a)
# What the driver would add by itself (-nodefaultlibs keeps it from doing so), spelled out so that the order is
# ours. Archives come before every shared library, and libc++ before a module's own static libraries:
#   - the compiler's helpers (__aeabi_idiv and its like) are then each library's own. After a shared library that
#     happens to export them they become imports from it, and Android's libcrypto.so does export them.
#   - libbase.a's properties.o holds its own copy of a few std::string functions; met there before libc++, they
#     pull the whole of properties.o, with its __system_property_* imports, into a library that never calls it.
# The system libraries come last, in the order Android's DT_NEEDED has them.
CXXRT=(-lc++_static -lc++abi)
COMPILER_RT=(-lunwind -lcompiler_rt-extras -lgcc_real)
SYSTEM=(-lc -lm -ldl)

# What mg_ld.c applies; anything else in a library is a reason to stop.
RELOCS_OK="R_ARM_RELATIVE R_ARM_ABS32 R_ARM_REL32 R_ARM_GLOB_DAT R_ARM_JUMP_SLOT R_ARM_IRELATIVE R_ARM_TLS_DTPMOD32 R_ARM_TLS_DTPOFF32 R_ARM_TLS_TPOFF32 R_ARM_NONE"

# link SONAME c|c++ MODULE [its static libraries, then its shared ones]...
# Links MODULE's objects into $UNSTRIPPED/SONAME, checks the result, and puts a stripped copy in $LIB.
link() {
  local so="$1" lang="$2" module="$3" out="$UNSTRIPPED/$1" driver="$CC" t bad attrs
  local rt=("${COMPILER_RT[@]}")
  shift 3
  if [ "$DIRTY" = 0 ] && [ -f "$LIB/$so" ] && [ "$out" -nt "$SELF" ]; then
    echo "    $so: $(stat -c%s "$LIB/$so") bytes; nothing it is made of has changed"
    return 0
  fi
  DIRTY=1
  if [ "$lang" = c++ ]; then driver="$CXX"; rt=("${CXXRT[@]}" "${COMPILER_RT[@]}"); fi
  "$driver" "${ARCH[@]}" "${LDFLAGS[@]}" "-Wl,-soname,$so" -o "$out" "@$OBJ/$module/objects.rsp" "${rt[@]}" "$@" "${SYSTEM[@]}" \
      "-L$STUBS" "-L$UNSTRIPPED" > "$OBJ/link-$so.log" 2>&1 || {
    echo "android9-natives: $so did not link; the first lines (all of it: $OBJ/link-$so.log):" >&2
    head -40 "$OBJ/link-$so.log" >&2
    exit 1
  }
  bad=""
  for t in $("$READELF" -r "$out" | tr -d '\r' | awk '$3 ~ /^R_ARM_/ {print $3}' | sort -u); do
    case " $RELOCS_OK " in *" $t "*) ;; *) bad="$bad $t" ;; esac
  done
  [ -z "$bad" ] || die "$so has relocations mg_ld.c does not apply:$bad"
  "$READELF" -lW "$out" | tr -d '\r' | grep -qE "^ +TLS " && die "$so has a PT_TLS segment; emulated TLS was expected"
  "$READELF" -d "$out" | tr -d '\r' | grep -q "TEXTREL" && die "$so has text relocations"
  attrs="$("$GNU_READELF" -A "$out" | tr -d '\r')"
  echo "$attrs" | grep -q "Tag_CPU_arch: v7$" || die "$so is not built for ARMv7: $(echo "$attrs" | grep Tag_CPU_arch:)"
  echo "$attrs" | grep -q "Tag_Advanced_SIMD_arch" && die "$so has objects built for NEON"
  echo "$attrs" | grep -q "Tag_FP_arch: VFPv3-D16$" || die "$so: FP architecture is not VFPv3-D16: $(echo "$attrs" | grep Tag_FP_arch:)"
  "$STRIP" --strip-all --keep-section=.ARM.attributes -o "$LIB/$so" "$out" || die "$so: llvm-strip failed"
  echo "    $so: $(stat -c%s "$LIB/$so") bytes; needs$("$READELF" -d "$LIB/$so" | tr -d '\r' | sed -n 's/.*Shared library: \[\(.*\)\]/ \1/p' | tr -d '\n')"
}

# stub SONAME ARGS-FOR-stub.py...: a library of names to link against, in $STUBS
stub() {
  local so="$1" s="$STUBS/${1%.so}.S"
  shift
  python "$SHIM/stub.py" "$s.new" "$@" || die "stub.py failed for $so"
  if [ -f "$STUBS/$so" ] && cmp -s "$s.new" "$s"; then rm -f "$s.new"; return 0; fi
  mv "$s.new" "$s"
  DIRTY=1
  "$CC" "${ARCH[@]}" -shared -nostdlib -fuse-ld=lld "-Wl,-soname,$so" -o "$STUBS/$so" "$s" || die "$so: the stub did not link"
}

# ------------------------------------------------------------------ stubs

echo "=== stubs, in $STUBS ==="
stub libopenjdkjvm.so --names "$SHIM/libopenjdkjvm.syms"
for l in libc libdl; do
  "$NM" -D --defined-only "$SYSLIB/$l.so" | tr -d '\r' > "$STUBS/$l.ndk.txt"
  stub "$l.so" --nm "$STUBS/$l.ndk.txt" --names "$SHIM/$l-platform.syms"
done
for f in libcrypto.so libssl.so; do
  cp "$VNDK/$f" "$UNSTRIPPED/$f"
  cp "$VNDK/$f" "$LIB/$f"
  echo "    $f: $(stat -c%s "$LIB/$f") bytes, Android 9's own (copied)"
done

# ------------------------------------------------------------------ ICU

echo "=== ICU 60 ==="
D=$A/external/icu/icu4c/source/common
mapfile -t SRCS < <(bp $D/Android.bp libicuuc srcs | sed "s|^|$D/|")
SRCS+=("$SHIM/icu_stubdata.cpp")        # for AOSP's libicuuc_stubdata
mapfile -t MOD_CXX < <(bp_flags $D/Android.bp libicuuc cflags; bp_flags $D/Android.bp libicuuc cppflags)
[ "$(bp $D/Android.bp libicuuc rtti)" = true ] && MOD_CXX+=(-frtti) || MOD_CXX+=(-fno-rtti)
MOD_CXX+=(-I$D)
MOD_C=()
objects icuuc
link libicuuc.so c++ icuuc

D=$A/external/icu/icu4c/source/i18n
mapfile -t SRCS < <(bp $D/Android.bp libicui18n srcs | sed "s|^|$D/|")
mapfile -t MOD_CXX < <(bp_flags $D/Android.bp libicui18n cflags; bp_flags $D/Android.bp libicui18n cppflags)
[ "$(bp $D/Android.bp libicui18n rtti)" = true ] && MOD_CXX+=(-frtti) || MOD_CXX+=(-fno-rtti)
MOD_CXX+=("${I_ICU[@]}")
MOD_C=()
objects icui18n
link libicui18n.so c++ icui18n -licuuc

# The stand-in for ICU's stub data is right if it is, byte for byte, what Android's own library has there.
REF=build/android9/system/lib64-for-reference/libicuuc.so
if [ -f "$REF" ]; then
  python "$SHIM/report.py" stubdata "$REF" "$UNSTRIPPED/libicuuc.so" || die "icudt60_dat is not what Android's libicuuc.so has"
fi

# ------------------------------------------------------------------ libnativehelper, libexpat

echo "=== libnativehelper, libexpat ==="
D=$A/libnativehelper
mapfile -t SRCS < <(bp $D/Android.bp libnativehelper srcs | sed "s|^|$D/|")
mapfile -t MOD_CXX < <(bp_flags $D/Android.bp libnativehelper cflags)
MOD_CXX+=(-fno-rtti "${I_JNI[@]}" "${I_LOG[@]}" -I$D)
MOD_C=()
objects nativehelper
link libnativehelper.so c++ nativehelper -llog

D=$A/external/expat
mapfile -t SRCS < <(bp $D/Android.bp libexpat srcs | sed "s|^|$D/|")
mapfile -t MOD_C < <(bp_flags $D/Android.bp libexpat cflags)
MOD_C+=(-I$D -I$D/lib)
MOD_CXX=()
objects expat
# Android builds libexpat against the NDK with its "system" C++ library, which leaves a libstdc++.so in DT_NEEDED.
link libexpat.so c expat -lstdc++

# ------------------------------------------------------------------ the static libraries

echo "=== libbase, libfdlibm (static) ==="
D=$A/system/core/base
mapfile -t SRCS < <(bp $D/Android.bp libbase srcs | sed "s|^|$D/|")
mapfile -t MOD_CXX < <(bp_flags $D/Android.bp libbase cflags; bp_flags $D/Android.bp libbase cppflags)
MOD_CXX+=(-fno-rtti "${I_BASE[@]}" "${I_LOG[@]}")
MOD_C=()
objects base
archive base

D=$A/external/fdlibm
mapfile -t SRCS < <(bp $D/Android.bp libfdlibm srcs | sed "s|^|$D/|")
mapfile -t MOD_C < <(bp_flags $D/Android.bp libfdlibm cflags)
MOD_C+=("-std=$(bp $D/Android.bp libfdlibm c_std)")
MOD_CXX=()
objects fdlibm
archive fdlibm

# ------------------------------------------------------------------ libjavacore, libopenjdk

# Soong would also link libziparchive (static) and libutils into libjavacore, and libssl into libopenjdk. Nothing in
# either library uses them (Android's own import nothing from them), their sources are not fetched, and a DT_NEEDED
# for a library that does not exist would only make Megler's linker warn. They are left out.
echo "=== libjavacore ==="
D=$A/libcore
L=$D/luni/src/main/native
mapfile -t SRCS < <(bp $L/Android.bp luni_native_srcs srcs | sed "s|^|$L/|"; bp $D/NativeCode.bp libjavacore srcs | grep -v '^:' | sed "s|^|$D/|")
mapfile -t MOD_CXX < <(bp_flags $D/NativeCode.bp libjavacore cflags; bp_flags $D/NativeCode.bp libjavacore cppflags)
MOD_CXX+=(-fno-rtti "${I_CORE[@]}")
MOD_C=()
objects javacore
link libjavacore.so c++ javacore $OBJ/base/libbase.a $OBJ/fdlibm/libfdlibm.a \
  -llog -lcrypto -lexpat -licuuc -licui18n -lnativehelper -lz

echo "=== libopenjdk ==="
L=$D/ojluni/src/main/native
mapfile -t SRCS < <(bp $L/Android.bp libopenjdk_native_srcs srcs | sed "s|^|$L/|")
mapfile -t MOD_C < <(bp_flags $D/NativeCode.bp libopenjdk cflags)
MOD_C+=("${I_CORE[@]}" -I$L)
mapfile -t MOD_CXX < <(bp_flags $D/NativeCode.bp libopenjdk cflags; bp_flags $D/NativeCode.bp libopenjdk cppflags)
MOD_CXX+=(-fno-rtti "${I_CORE[@]}" -I$L)
objects openjdk
link libopenjdk.so c++ openjdk $OBJ/base/libbase.a $OBJ/fdlibm/libfdlibm.a \
  -llog -lcrypto -licuuc -lz -lnativehelper -lopenjdkjvm
echo "    from libopenjdkjvm.so it imports: $("$NM" -D --undefined-only "$LIB/libopenjdk.so" | tr -d '\r' | awk '{print $NF}' | grep -xFf <(grep -v '^#' "$SHIM/libopenjdkjvm.syms") | tr '\n' ' ')"

# ------------------------------------------------------------------ libjavacrypto

# conscrypt's native half, as Android 9 builds it for the platform: the classes it answers for are com.android.org.conscrypt.*
# (JNI_JARJAR_PREFIX, the jarjar rule conscrypt.dex was made with), and an fd waiting in a socket call is announced to
# libnativehelper's AsynchronousCloseMonitor, so that a close from another thread wakes it. Its source list is a glob
# (common/src/jni/main/cpp/**/*.cc), which bp.py hands back as it is: it is expanded here.
echo "=== libjavacrypto ==="
D=$A/external/conscrypt
SRCS=()
while read -r g; do
  case "$g" in
    */\*\*/\*.*) mapfile -t -O "${#SRCS[@]}" SRCS < <(find "$D/${g%%/\*\*/*}" -name "${g##*/}" -type f | sort) ;;
    *) SRCS+=("$D/$g") ;;
  esac
done < <(bp $D/Android.bp libjavacrypto srcs)
[ "${#SRCS[@]}" -gt 0 ] || die "libjavacrypto: its source glob found nothing under $D"
mapfile -t MOD_CXX < <(bp_flags $D/Android.bp libjavacrypto cflags)
MOD_CXX+=(-fno-rtti -I$D/common/src/jni/main/include -I$A/libcore/luni/src/main/native "${I_JNI[@]}" "${I_LOG[@]}"
          -I$A/external/boringssl/src/include)
MOD_C=()
objects javacrypto
link libjavacrypto.so c++ javacrypto -lcrypto -ljavacore -llog -lnativehelper -lssl
echo "    registers its natives for: $(grep -ao 'com/android/org/conscrypt/[A-Za-z]*' "$UNSTRIPPED/libjavacrypto.so" | sort -u | tr '\n' ' ')"

echo "=== in $LIB ==="
ls -l "$LIB"/*.so | awk '{printf "    %9d  %s\n", $5, $NF}'
echo "tools/android9-natives/report.py compares them with Android's own, the bytecode and the layer."
