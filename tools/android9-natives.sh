#!/bin/bash
# The native half of Android 9's libcore, compiled with an Android NDK for one
# ABI: plain 32-bit ARMv7-A (the phones', the default) into build/android9/lib,
# or arm64-v8a, x86_64 or x86 (desktop Windows on ARM64, x64 and x86) into
# build/android9/lib-<abi>.
#
#   tools/android9-natives.sh [abi] [clean]          (from Git Bash)
#
#   abi   armeabi-v7a (the default), arm64-v8a, x86_64 or x86
#
# Why: the Java runtime (docs/JAVA.md, "The boot class path: libcore") runs
# Android 9's own bytecode, and that bytecode's native methods are in
# libjavacore.so and libopenjdk.so. Google's Android 9 images carry those for
# arm64 only, and a 32-bit phone build of them is compiled for ARMv8 cores. So
# they are built here from the sources tools/android9-sources.sh fetched. Megler
# runs an app's native code on the machine's own processor, never another's, so
# each port needs them for its own ABI, and every ABI's are made the same way
# (below), which no image's are.
#
# What is built, in this order:
#
#   libcrypto.so, libssl.so      BoringSSL (external/boringssl), which conscrypt
#                                and java.math's NativeBN stand on: for
#                                arm64-v8a alone, from Android 9's sources at
#                                the tag, as its Android.bp and sources.bp say,
#                                the aarch64 assembly included (see arm64-v8a
#                                below: x18). For the other ABIs they are not
#                                built (below).
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
#   libcrypto.so, libssl.so      armeabi-v7a, x86_64, x86: not built, Android 9's
#                                own for the ABI (the VNDK snapshot's), copied.
#                                For ARMv7 they alone are compiled for NEON, as
#                                their folder's name says; every Windows phone's
#                                processor has it. For x86_64 and x86, each
#                                architecture's base variant: arch-x86_64-x86_64,
#                                arch-x86-x86. (arm64's own, arch-arm64-armv8-a,
#                                is what arm64-v8a's build is held against.)
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
# files (tools/android9-natives/bp.py, with what Soong adds for the ABI's
# architecture), not globbed. Soong's global flags are not in the fetched tree
# (build/soong is not fetched); GLOBAL and the ABI's flags below repeat the
# ones that change the code, as Android 9's cc/config/global.go and
# <arch>_device.go have them (held against those files at the tag on
# android.googlesource.com). -Werror and the warning flags are dropped: a newer
# compiler than Android 9's reads this code. An assembly source (.S, BoringSSL's)
# gets what Soong gives one: the global and the ABI's flags, the module's
# include folders, -D__ASSEMBLY__ and the module's asflags and clang_asflags --
# not its cflags.
#
# The target, at API 28 (the NDK's libraries of Android 9), is the base variant
# Soong has for the architecture:
#
#   armeabi-v7a   ARMv7-A, Thumb-2, VFPv3-D16, softfp: the NDK's own default
#                 for armv7a, said out loud. No NEON, nothing of ARMv8; the
#                 script checks the build attributes of what it linked. -Os and
#                 -fomit-frame-pointer, Soong's for Thumb code.
#   arm64-v8a     -march=armv8-a (the armv8-a variant, no CPU named), -O2,
#                 frame pointers kept. And -ffixed-x18: on Windows on ARM64, x18
#                 holds the thread's TEB, so no code in the process may touch
#                 it. The NDK's clang keeps off x18 for Android already; the flag
#                 says it out loud, and the script checks that no instruction of
#                 what it linked names x18, the NDK's static libc++ and libgcc
#                 included. (The driver also links with --fix-cortex-a53-843419.)
#                 That is why libcrypto.so and libssl.so are built for this ABI:
#                 Android 9's own were compiled by a clang that had x18 as one
#                 more register, and its libcrypto.so writes it in 772
#                 instructions of plain C (X25519, AES, DES, MD5, the big
#                 numbers, poly1305, the TLS PRF) that every TLS connection of a
#                 Java app runs through; a thread that ran one ends at its next
#                 call into Windows or its next exception (GitHub issue 4, and
#                 docs/DESKTOP.md). BoringSSL's aarch64 assembly (OpenSSL's
#                 perlasm, linux-aarch64) keeps off x18 by itself -- none of its
#                 seven files names it -- so it is assembled as Android
#                 assembles it (-march=armv8-a+crypto, the module's
#                 clang_asflags; the C stays armv8-a) and checked with the rest.
#   x86_64        -march=x86-64 with the features of Soong's base x86_64
#                 variant, SSSE3, SSE4.1, SSE4.2 and POPCNT (the NDK's x86_64
#                 ABI: clang turns on SSE4.2 and POPCNT for x86_64 Android by
#                 itself). -O2.
#   x86           -march=prescott -msse3 -mstackrealign, Soong's base x86
#                 variant, with no features of its own (clang adds SSSE3 for any
#                 i686 Android target, as it did for Android 9's own build).
#                 -O2. -mstackrealign: a function realigns its stack, which a
#                 caller from outside (JNI, or Windows' x86 code, which keeps 4
#                 bytes) may have left unaligned for SSE.
#
# On arm64, x86_64 and x86 a function's stack guard is read out of bionic's TLS
# slot 5 through the thread pointer (TPIDR_EL0+0x28, %fs:0x28, %gs:0x14), as in
# the code of every NDK app built for API 17 or later, and not out of
# __stack_chk_guard as on ARMv7; the script counts those reads. (On x86 the
# NDK's C++ runtime, built for API 16, and Android 9's own libcrypto and libssl,
# compiled for no API level, read __stack_chk_guard still.)
#
# How the libraries are linked follows from what Megler's linker
# (core/src/ld/mg_ld.c, core/src/arm/mg_patch.c) does:
#
#   - ld.lld: its PLT entries are a shape mg_patch.c recognises and rewrites
#     (and the shape the core's own test libraries have).
#   - Plain REL relocations, no packing (no APS2, no RELR): mg_ld.c reads all
#     three, and the plain table is the one with nothing to go wrong in. (RELA
#     on the 64-bit ABIs, the only kind they have.)
#   - Both hash tables: lookup uses the GNU one, and the SysV one gives
#     mg_ld.c the exact number of symbols instead of a derived one.
#   - Emulated TLS (the compiler's default below API 29; -femulated-tls on the
#     desktop ABIs), so no PT_TLS and no TLS relocations; thread-local data
#     goes through pthread keys, as on Android 9 itself.
#   - No IFUNCs, no text relocations (a warning made fatal), 4 KB pages
#     (lld's own default for AArch64 is 64 KB).
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
# Each library is checked once linked: relocation types a loader of the ABI
# applies (outside ARM: none for TLS, no IRELATIVE), no PT_TLS segment, no text
# relocations; for armeabi-v7a its build attributes; for the other ABIs its
# machine, no instruction that names x18 on arm64, and its DT_NEEDED list and
# imported names held against those of the armeabi-v7a library of the same name
# in build/android9/lib (tools/android9-natives/abidiff.py): the same, but for
# the names the ABI accounts for, which are said with their reason. arm64's
# libcrypto.so and libssl.so are held against Android's own arm64 ones instead,
# which they stand in for (abidiff.py's `replaces`): the same SONAME and
# DT_NEEDED, every name Android's exports but the few its link put there by
# accident, the same imports but for what is said with its reason. What is built
# on them here (libjavacore, libopenjdk, libjavacrypto) is linked against them
# with --no-undefined, so every name those import from them is there. The
# copies of x86_64's and x86's are checked as a built library is, but their
# differences from Android's ARM build are said, not fatal: they are Android's
# own.
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
# tables to build/android9/obj/unstripped, stripped ones to build/android9/lib;
# for the other ABIs obj-<abi> and lib-<abi>. On arm64 a library is held to no
# word that Megler's loader would count as a write of x18
# (tools/android9-natives/x18.py, its count done on the file); one whose code
# has data among it that reads so (libcrypto.so: BoringSSL's assembly keeps
# its constants in .text) keeps its symbol table, whose mapping symbols are how
# the loader tells those words from instructions. A source is compiled again when it
# is newer than its object, a module when its flags changed, a library is linked
# again when anything before it was compiled or this script is newer than it.
# Afterwards, for armeabi-v7a:
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

ABI=armeabi-v7a
case "${1:-}" in armeabi-v7a|arm64-v8a|x86_64|x86) ABI="$1"; shift ;; esac
case "$#:${1:-}" in
  0:|1:clean) ;;
  *) echo "usage: tools/android9-natives.sh [armeabi-v7a|arm64-v8a|x86_64|x86] [clean]" >&2; exit 2 ;;
esac

A=build/android9/aosp
ARMLIB=build/android9/lib       # armeabi-v7a's libraries, which the other ABIs' are held against
if [ "$ABI" = armeabi-v7a ]; then OBJ=build/android9/obj; LIB=$ARMLIB
else OBJ=build/android9/obj-$ABI; LIB=build/android9/lib-$ABI; fi
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
OBJDUMP="$TC/bin/llvm-objdump$EXE"
CXXFILT="$TC/bin/arm-linux-androideabi-c++filt$EXE"     # C++ names are mangled the same for every processor

# ------------------------------------------------------------------ the ABI

# TRIPLE       the NDK's folder of the ABI's libraries
# SOONG_ARCH   Soong's name of the architecture (bp.py, stub.py)
# VNDK_ARCH    the folder of the VNDK snapshot libcrypto.so and libssl.so are copied from, or held against
# BORINGSSL    built (from external/boringssl: arm64, whose Android 9 copies write x18) or copied (the VNDK's)
# ARCH         the compiler's flags for the ABI (Soong's for the architecture's base variant; see the header)
# OPT, OMIT_FP what GLOBAL takes from the ABI
# COMPILER_RT  the compiler's runtime, as the NDK's libgcc.a (a linker script) names it for the ABI
# KEEP         the sections llvm-strip keeps
# RELOCS_OK    the relocation types a library may have: what the ABI's loader applies
# MACHINE      llvm-readelf's name of the ELF machine
# TP_RE, X18_RE  what an instruction that reads the thread pointer or names x18 looks like in llvm-objdump's output
BORINGSSL=copied
case "$ABI" in
  armeabi-v7a)
    TRIPLE=arm-linux-androideabi; SOONG_ARCH=arm; VNDK_ARCH=arm/arch-arm-armv7-a-neon
    ARCH=(--target=armv7a-linux-androideabi28 -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp -mthumb)
    OPT=(-Os); OMIT_FP=(-fomit-frame-pointer)
    COMPILER_RT=(-lunwind -lcompiler_rt-extras -lgcc_real)
    KEEP=(--keep-section=.ARM.attributes)
    # What mg_ld.c applies; anything else in a library is a reason to stop.
    RELOCS_OK="R_ARM_RELATIVE R_ARM_ABS32 R_ARM_REL32 R_ARM_GLOB_DAT R_ARM_JUMP_SLOT R_ARM_IRELATIVE R_ARM_TLS_DTPMOD32 R_ARM_TLS_DTPOFF32 R_ARM_TLS_TPOFF32 R_ARM_NONE"
    MACHINE=ARM; TP_RE=""; X18_RE="" ;;
  arm64-v8a)
    TRIPLE=aarch64-linux-android; SOONG_ARCH=arm64; VNDK_ARCH=arm64/arch-arm64-armv8-a; BORINGSSL=built
    ARCH=(--target=aarch64-linux-android28 -march=armv8-a -ffixed-x18 -femulated-tls)
    OPT=(-O2); OMIT_FP=()
    COMPILER_RT=(-lcompiler_rt-extras -lgcc_real)
    KEEP=()
    RELOCS_OK="R_AARCH64_RELATIVE R_AARCH64_ABS64 R_AARCH64_GLOB_DAT R_AARCH64_JUMP_SLOT R_AARCH64_NONE"
    MACHINE=AArch64; TP_RE="mrs[ \t]+x[0-9]+, *tpidr_el0"; X18_RE="(^|[^a-z0-9_])[xw]18([^a-z0-9_]|$)" ;;
  x86_64)
    TRIPLE=x86_64-linux-android; SOONG_ARCH=x86_64; VNDK_ARCH=x86_64/arch-x86_64-x86_64
    ARCH=(--target=x86_64-linux-android28 -march=x86-64 -DUSE_SSSE3 -mssse3 -msse4 -msse4.1 -msse4.2 -mpopcnt -femulated-tls)
    OPT=(-O2); OMIT_FP=()
    COMPILER_RT=(-lcompiler_rt-extras -lgcc_real)
    KEEP=()
    RELOCS_OK="R_X86_64_RELATIVE R_X86_64_64 R_X86_64_GLOB_DAT R_X86_64_JUMP_SLOT R_X86_64_NONE"
    MACHINE="Advanced Micro Devices X86-64"; TP_RE="%fs:"; X18_RE="" ;;
  x86)
    TRIPLE=i686-linux-android; SOONG_ARCH=x86; VNDK_ARCH=x86/arch-x86-x86
    ARCH=(--target=i686-linux-android28 -march=prescott -msse3 -mstackrealign -femulated-tls)
    OPT=(-O2); OMIT_FP=()
    COMPILER_RT=(-lcompiler_rt-extras -lgcc_real)
    KEEP=()
    RELOCS_OK="R_386_RELATIVE R_386_32 R_386_PC32 R_386_GLOB_DAT R_386_JUMP_SLOT R_386_NONE"
    MACHINE="Intel 80386"; TP_RE="%gs:"; X18_RE="" ;;
esac
SYSLIB="$TC/sysroot/usr/lib/$TRIPLE/28"

die() { echo "android9-natives: $*" >&2; exit 1; }
[ -x "$CC" ] || die "no clang at $CC (set MEGLER_NDK to an Android NDK, r21 or near it)"
[ -f "$SYSLIB/libc.so" ] || die "the NDK has no API 28 libraries for $ABI ($SYSLIB)"
[ -f "$A/libcore/NativeCode.bp" ] || die "no $A/libcore: run tools/android9-sources.sh first"
VNDK="$A/prebuilts/vndk/v28/$VNDK_ARCH/shared/vndk-core"
for f in libcrypto.so libssl.so; do [ -f "$VNDK/$f" ] || die "no $VNDK/$f: run tools/android9-sources.sh first"; done
[ -f "$A/external/conscrypt/Android.bp" ] || die "no $A/external/conscrypt: run tools/android9-sources.sh first"
SSL=$A/external/boringssl
if [ "$BORINGSSL" = built ]; then
  for f in Android.bp sources.bp err_data.c src/crypto/fipsmodule/bcm.c src/ssl/ssl_lib.cc src/third_party/fiat/curve25519.c linux-aarch64; do
    [ -e "$SSL/$f" ] || die "no $SSL/$f (BoringSSL's sources, for $ABI's libcrypto.so and libssl.so): run tools/android9-sources.sh"
  done
fi

if [ "${1:-}" = clean ]; then
  rm -rf "$OBJ" "$LIB"/*.so "$LIB/report"       # the libraries and what report.py wrote; anything else lying there stays
  echo "removed $OBJ, the libraries in $LIB and $LIB/report"
  exit 0
fi
mkdir -p "$OBJ" "$LIB" "$UNSTRIPPED" "$STUBS"
[ "$ABI" = armeabi-v7a ] || echo "=== $ABI, into $LIB ==="
bp() { python "$SHIM/bp.py" "$@" "$SOONG_ARCH" | tr -d '\r'; }
# Below, bp is read through process substitution, where a failure would pass for an empty list. So every question
# is asked once here, where a failure stops the script.
for q in "external/icu/icu4c/source/common/Android.bp libicuuc" "external/icu/icu4c/source/i18n/Android.bp libicui18n" \
         "libnativehelper/Android.bp libnativehelper" "external/expat/Android.bp libexpat" "system/core/base/Android.bp libbase" \
         "external/fdlibm/Android.bp libfdlibm" "libcore/NativeCode.bp libjavacore" "libcore/NativeCode.bp libopenjdk" \
         "libcore/luni/src/main/native/Android.bp luni_native_srcs" "libcore/ojluni/src/main/native/Android.bp libopenjdk_native_srcs" \
         "external/conscrypt/Android.bp libjavacrypto" \
         $([ "$BORINGSSL" = built ] && echo "external/boringssl/Android.bp=libcrypto external/boringssl/Android.bp=libssl"); do
  set -- ${q/=/ }
  [ -n "$(bp "$A/$1" "$2" srcs)" ] || die "cannot read the sources of $2 out of $A/$1"
done

# ------------------------------------------------------------------ flags

# Soong's own, for every module of a device of the ABI (see the header).
GLOBAL=(-DANDROID -DNDEBUG -UDEBUG "${OPT[@]}" -fPIC -fno-exceptions -fno-strict-aliasing -ffunction-sections -fdata-sections
        -fno-short-enums -funwind-tables -fstack-protector-strong "${OMIT_FP[@]}" -D_FORTIFY_SOURCE=2 -Wa,--noexecstack)
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

# objects MODULE: compiles SRCS (paths from the repository's root) with MOD_C / MOD_CXX after the global flags, and
# assembly (.S) with MOD_AS after the global ones (its cflags are not an assembly source's, as in Soong).
# Leaves $OBJ/MODULE/objects.rsp, the list a link or an archive takes.
DIRTY=0           # 1 once anything was compiled or a stub changed: what is linked from then on is linked anew
MOD_AS=()         # a module with assembly sets it (BoringSSL)
objects() {
  local name="$1" dir="$OBJ/$1" src base obj n failed=0 rsp
  local -A seen=()
  local kinds=(c cxx)
  mkdir -p "$dir"
  printf '%s\n' "${ARCH[@]}" "${GLOBAL[@]}" "${GLOBAL_C[@]}" "${MOD_C[@]}" "${QUIET[@]}" > "$dir/c.rsp.new"
  printf '%s\n' "${ARCH[@]}" "${GLOBAL[@]}" "${GLOBAL_CXX[@]}" "${MOD_CXX[@]}" "${QUIET[@]}" > "$dir/cxx.rsp.new"
  # (as.rsp only for a module that has assembly: the others' objects stay what they were.)
  if printf '%s\n' "${SRCS[@]}" | grep -q '\.S$'; then
    kinds+=(as)
    printf '%s\n' "${ARCH[@]}" "${GLOBAL[@]}" "${MOD_AS[@]}" "${QUIET[@]}" > "$dir/as.rsp.new"
  fi
  failed=0
  for rsp in "${kinds[@]}"; do cmp -s "$dir/$rsp.rsp.new" "$dir/$rsp.rsp" || failed=1; done
  if [ "$failed" = 1 ]; then
    rm -f "$dir"/*.o
    for rsp in "${kinds[@]}"; do mv "$dir/$rsp.rsp.new" "$dir/$rsp.rsp"; done
  else
    for rsp in "${kinds[@]}"; do rm -f "$dir/$rsp.rsp.new"; done
  fi
  failed=0
  : > "$dir/todo.c"; : > "$dir/todo.cxx"; : > "$dir/todo.as"; : > "$dir/objects.rsp"; : > "$dir/compile.log"
  for src in "${SRCS[@]}"; do
    [ -f "$src" ] || die "$name: no source $src"
    base="$(basename "$src")"; obj="$dir/${base%.*}.o"
    [ -z "${seen[$obj]:-}" ] || die "$name: two sources named $base"
    seen[$obj]=1
    echo "$obj" >> "$dir/objects.rsp"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
      case "$src" in
        *.c) echo "$src -o $obj" >> "$dir/todo.c" ;;
        *.S) echo "$src -o $obj" >> "$dir/todo.as" ;;
        *) echo "$src -o $obj" >> "$dir/todo.cxx" ;;
      esac
    fi
  done
  n=$(cat "$dir/todo.c" "$dir/todo.cxx" "$dir/todo.as" | wc -l)
  if [ "$n" != 0 ]; then
    DIRTY=1
    echo "    $name: compiling $n of ${#SRCS[@]}"
    xargs -r -P "$JOBS" -L 1 "$CC" "@$dir/c.rsp" -c < "$dir/todo.c" >> "$dir/compile.log" 2>&1 || failed=1
    xargs -r -P "$JOBS" -L 1 "$CXX" "@$dir/cxx.rsp" -c < "$dir/todo.cxx" >> "$dir/compile.log" 2>&1 || failed=1
    xargs -r -P "$JOBS" -L 1 "$CC" "@$dir/as.rsp" -c < "$dir/todo.as" >> "$dir/compile.log" 2>&1 || failed=1
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
SYSTEM=(-lc -lm -ldl)

needs() { "$READELF" -d "$1" | tr -d '\r' | sed -n 's/.*Shared library: \[\(.*\)\]/ \1/p' | tr -d '\n'; }
imports() { "$NM" -D --undefined-only "$1" | tr -d '\r' | awk '{print $NF}' | sed 's/@.*//' | sort -u; }

# scan FILE: "R X N", how many of FILE's instructions read the thread pointer (TP_RE) and how many name x18 (X18_RE),
# of the N lines llvm-objdump wrote. A function's own name (its header line, a branch's <target>) is no operand.
scan() {
  "$OBJDUMP" -d --no-show-raw-insn "$1" | tr -d '\r' | awk -v tp="$TP_RE" -v x18="$X18_RE" '
    /^[0-9a-f]+ [^ \t]+:$/ { next }
    { gsub(/<[^>]*>/, ""); l = tolower($0) }
    tp != "" && l ~ tp { r++ }
    x18 != "" && l ~ x18 { x++ }
    END { print r + 0, x + 0, NR }'
}

# held SO FILE: what a library of the ABI has to be, whoever made it. ARM's build attributes are not here: link
# checks them on what it built (Android's own ARM libcrypto and libssl are built for NEON). x18 is fatal in any of
# arm64's: nothing of arm64's is copied (Android 9's own libcrypto and libssl were why the check was once a warning).
held() {
  local so="$1" f="$2" t bad="" r x n
  for t in $("$READELF" -r "$f" | tr -d '\r' | awk '$3 ~ /^R_/ {print $3}' | sort -u); do
    case " $RELOCS_OK " in *" $t "*) ;; *) bad="$bad $t" ;; esac
  done
  case "$bad" in *IRELATIVE*) die "$so has IRELATIVE relocations (IFUNCs):$bad" ;; *TLS*|*DTP*|*TPOFF*) die "$so has TLS relocations; emulated TLS was expected:$bad" ;; esac
  [ -z "$bad" ] || die "$so has relocations outside what its loader applies (RELOCS_OK):$bad"
  "$READELF" -lW "$f" | tr -d '\r' | grep -qE "^ +TLS " && die "$so has a PT_TLS segment; emulated TLS was expected"
  "$READELF" -d "$f" | tr -d '\r' | grep -q "TEXTREL" && die "$so has text relocations"
  [ "$ABI" != armeabi-v7a ] || return 0
  t="$("$READELF" -h "$f" | tr -d '\r' | sed -n 's/^ *Machine: *//p')"
  [ "$t" = "$MACHINE" ] || die "$so is for the machine $t, not $MACHINE"
  read -r r x n < <(scan "$f")
  [ "${n:-0}" -gt 1000 ] || die "$so: llvm-objdump could not read it"
  [ "$x" = 0 ] || die "$so names x18 in $x instructions; on Windows on ARM64 x18 is the thread's TEB"
  echo "      reads the thread pointer in $r place$([ "$r" = 1 ] || echo s)$([ -z "$X18_RE" ] || echo "; no instruction names x18")"
}

# against_arm SO FILE built|copied: FILE's DT_NEEDED list and imported names held against those of the armeabi-v7a
# library of the same name (abidiff.py, which says how they differ and why). What FILE imports goes to
# $OBJ/imports-SO.txt.
against_arm() {
  local so="$1" f="$2" how="$3" ref="$ARMLIB/$1"
  imports "$f" > "$OBJ/imports-$so.txt"
  if [ ! -f "$ref" ]; then echo "      not held against armeabi-v7a's: no $ref (build armeabi-v7a first)"; return 0; fi
  python "$SHIM/abidiff.py" "$CXXFILT" "$ABI" "$f" "$ref" "$how" || die "$so differs from armeabi-v7a's $so in what the ABI does not account for (above)"
}

# against_android SO FILE: FILE, built in place of Android's own library of the same name and ABI (arm64's libcrypto.so
# and libssl.so, the VNDK snapshot's), held against that one (abidiff.py replaces): the same SONAME and DT_NEEDED,
# every name it exports, the same imports, but for what is said with its reason. What FILE imports goes to
# $OBJ/imports-SO.txt.
against_android() {
  local so="$1" f="$2"
  imports "$f" > "$OBJ/imports-$so.txt"
  python "$SHIM/abidiff.py" "$CXXFILT" "$ABI" "$f" "$VNDK/$so" replaces || die "$so does not stand in for Android's own $so (above)"
}

# link SONAME c|c++ MODULE [its static libraries, then its shared ones]...
# Links MODULE's objects into $UNSTRIPPED/SONAME, checks the result, and puts a stripped copy in $LIB.
link() {
  local so="$1" lang="$2" module="$3" out="$UNSTRIPPED/$1" driver="$CC" attrs
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
  [ "$ABI" = armeabi-v7a ] || echo "    $so:"
  held "$so" "$out"
  if [ "$ABI" = armeabi-v7a ]; then
    attrs="$("$GNU_READELF" -A "$out" | tr -d '\r')"
    echo "$attrs" | grep -q "Tag_CPU_arch: v7$" || die "$so is not built for ARMv7: $(echo "$attrs" | grep Tag_CPU_arch:)"
    echo "$attrs" | grep -q "Tag_Advanced_SIMD_arch" && die "$so has objects built for NEON"
    echo "$attrs" | grep -q "Tag_FP_arch: VFPv3-D16$" || die "$so: FP architecture is not VFPv3-D16: $(echo "$attrs" | grep Tag_FP_arch:)"
  fi
  "$STRIP" --strip-all "${KEEP[@]}" -o "$LIB/$so" "$out" || die "$so: llvm-strip failed"
  # arm64: what Megler's loader will count in it as writes of x18 (x18.py), held to none. held() saw no instruction name
  # x18, so words the loader takes for such writes are data among the code -- hand-written assembly's constants and
  # banners, BoringSSL's -- which the library's own mapping symbols mark: it keeps its symbol table, where the loader
  # reads them (core/src/ld/mg_ld.c, read_data_spans).
  if [ -n "$X18_RE" ] && ! python "$SHIM/x18.py" "$LIB/$so" > /dev/null; then
    python "$SHIM/x18.py" "$out" > /dev/null || die "$so: Megler's loader would count $(python "$SHIM/x18.py" "$out") writes of x18 in it (tools/android9-natives/x18.py)"
    "$STRIP" --strip-debug "${KEEP[@]}" -o "$LIB/$so" "$out" || die "$so: llvm-strip failed"
    python "$SHIM/x18.py" "$LIB/$so" > /dev/null || die "$so: its symbol table, kept, does not mark its data"
    echo "      keeps its symbol table: $(python "$SHIM/x18.py" "$out" --raw) words among its code read as writes of x18, all of them data its mapping symbols mark"
  fi
  if [ "$ABI" = armeabi-v7a ]; then
    echo "    $so: $(stat -c%s "$LIB/$so") bytes; needs$(needs "$LIB/$so")"
  else
    echo "      $(stat -c%s "$LIB/$so") bytes; needs$(needs "$LIB/$so")"
    # What Android has a library of its own of for the ABI (BoringSSL's two) is held against that one; the rest against
    # armeabi-v7a's.
    case "$so" in
      libcrypto.so|libssl.so) against_android "$so" "$LIB/$so" ;;
      *) against_arm "$so" "$LIB/$so" built ;;
    esac
  fi
}

# stub SONAME ARGS-FOR-stub.py...: a library of names to link against, in $STUBS
stub() {
  local so="$1" s="$STUBS/${1%.so}.S"
  shift
  python "$SHIM/stub.py" "$s.new" --arch "$SOONG_ARCH" "$@" || die "stub.py failed for $so"
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
if [ "$BORINGSSL" = copied ]; then
  for f in libcrypto.so libssl.so; do
    cp "$VNDK/$f" "$UNSTRIPPED/$f"
    cp "$VNDK/$f" "$LIB/$f"
    echo "    $f: $(stat -c%s "$LIB/$f") bytes, Android 9's own (copied)"
    if [ "$ABI" != armeabi-v7a ]; then
      held "$f" "$LIB/$f"
      echo "      needs$(needs "$LIB/$f")"
      against_arm "$f" "$LIB/$f" copied
    fi
  done
fi

# ------------------------------------------------------------------ BoringSSL

# libcrypto and libssl as Android 9's Android.bp and sources.bp make them, for an ABI whose own copies cannot be taken
# (arm64: x18, see the header). Their sources are named relative to external/boringssl; libcrypto's assembly is
# sources.bp's target.linux_arm64 (bp.py reads it as Soong does for an Android device). libcrypto_defaults' exclude_srcs
# and the whole_static_libs it stands for (libcrypto_no_clang) are 32-bit ARM's alone, empty here. boringssl_defaults'
# sdk_version (9) is not followed: they are compiled at API 28 as everything here is. Both are linked as every library
# here is (libc++ for libssl, as the module's stl says, static and hidden), and with the same libraries in the same
# order as Android's: libc, libm, libdl, and libcrypto first for libssl.
if [ "$BORINGSSL" = built ]; then
  echo "=== BoringSSL: libcrypto, libssl (built: Android 9's own for $ABI name x18) ==="
  D=$SSL
  mapfile -t INC < <(bp $D/Android.bp libcrypto local_include_dirs | sed "s|^|-I$D/|")
  mapfile -t SRCS < <(bp $D/Android.bp libcrypto srcs | grep -vxFf <(bp $D/Android.bp libcrypto exclude_srcs) | sed "s|^|$D/|")
  mapfile -t MOD_C < <(bp_flags $D/Android.bp libcrypto cflags; bp_flags $D/Android.bp libcrypto conlyflags)
  MOD_C+=("${INC[@]}")
  mapfile -t MOD_AS < <(echo -D__ASSEMBLY__; bp_flags $D/Android.bp libcrypto asflags; bp_flags $D/Android.bp libcrypto clang_asflags)
  MOD_AS+=("${INC[@]}")
  MOD_CXX=()
  echo "    libcrypto: ${#SRCS[@]} sources, $(printf '%s\n' "${SRCS[@]}" | grep -c '\.S$') of them assembly; assembled with ${MOD_AS[*]:1}"
  objects crypto
  link libcrypto.so c crypto
  MOD_AS=()

  mapfile -t INC < <(bp $D/Android.bp libssl local_include_dirs | sed "s|^|-I$D/|")
  mapfile -t SRCS < <(bp $D/Android.bp libssl srcs | sed "s|^|$D/|")
  mapfile -t MOD_CXX < <(bp_flags $D/Android.bp libssl cflags; bp_flags $D/Android.bp libssl cppflags)
  [ "$(bp $D/Android.bp libssl rtti)" = true ] && MOD_CXX+=(-frtti) || MOD_CXX+=(-fno-rtti)
  MOD_CXX+=("${INC[@]}")
  MOD_C=()
  objects ssl
  link libssl.so c++ ssl -lcrypto
fi

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
if [ "$ABI" = armeabi-v7a ]; then
  echo "tools/android9-natives/report.py compares them with Android's own, the bytecode and the layer."
fi
