#!/bin/bash
# The parts of Android 9's source the Java side is built from or written against,
# from AOSP's own server, into build/android9/aosp/<project>/...
#
#   tools/android9-sources.sh            (from Git Bash or WSL; needs curl and tar)
#
# Google's emulator image for Android 9 (tools/android9-unpack.sh) has the boot
# class path's bytecode but only 64-bit native libraries, and no image of
# Android 9 in the SDK's repository has 32-bit ARM ones. So libcore's native
# half (libjavacore, libopenjdk, and ICU, libnativehelper, fdlibm, expat, zlib
# under them) is compiled here from source with the NDK, for plain ARMv7 --
# which a phone's own build would not be (a Pixel's 32-bit libraries are
# compiled for ARMv8 cores, and many Windows phones are ARMv7) -- and, for
# desktop Windows, for arm64, x86_64 and x86 the same way.
#
# What is fetched, all at one tag (android-9.0.0_r47, the August 2019 security
# patch level the image has):
#
#   libcore                  whole: the natives to build, and the Java source of
#                            the bytecode being run, to read
#   libnativehelper          whole
#   external/icu             icu4c/source/common and icu4c/source/i18n
#   external/fdlibm          whole
#   external/expat           whole
#   external/zlib            whole
#   system/core              base, liblog/include, libcutils/include
#   external/boringssl       src/include (headers for java.math's NativeBN and
#                            for conscrypt); and, for arm64-v8a's libcrypto.so
#                            and libssl.so, which are built here (below), the
#                            sources Android.bp and sources.bp name: src/crypto,
#                            src/ssl, src/third_party/fiat, err_data.c, the
#                            aarch64 assembly (linux-aarch64), and the two files
#   external/conscrypt       common/src/jni (the native half of conscrypt,
#                            libjavacrypto.so) and Android.bp (its build rule)
#   art                      runtime and openjdkjvm: not built -- they are what says
#                            how ART answers the native methods libcore leaves to
#                            the runtime, and what libopenjdkjvm.so's JVM_* do
#   prebuilts/vndk/v28       libcrypto.so and libssl.so, Android 9's own, for each
#                            ABI the natives are built for
#                            (tools/android9-natives.sh [abi]): ARMv7 with NEON,
#                            x86 and x86_64 take them as they are; arm64's
#                            (ARMv8-A) are what its own build is held against.
#                            Android 9's compiler had x18 as an ordinary
#                            register, and Windows on ARM64 keeps the thread's
#                            TEB there, so arm64's are compiled from BoringSSL's
#                            sources at this tag instead
#
# gitiles makes each archive when asked, so there are no sizes to check against;
# the whole comes to about 180 MB unpacked (BoringSSL 31 MB of it, its tests'
# data mostly). A part already there is not fetched again (remove its folder to
# fetch it anew).
set -euo pipefail

TAG="${MEGLER_AOSP_TAG:-android-9.0.0_r47}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/android9/aosp"
BASE="https://android.googlesource.com/platform"
mkdir -p "$OUT"

# part <project> <folder in it, or . for all of it>
part() {
  local project="$1" sub="$2" dest url
  if [ "$sub" = . ]; then dest="$OUT/$project"; url="$BASE/$project/+archive/refs/tags/$TAG.tar.gz"
  else dest="$OUT/$project/$sub"; url="$BASE/$project/+archive/refs/tags/$TAG/$sub.tar.gz"; fi
  if [ -d "$dest" ] && [ -n "$(ls -A "$dest" 2> /dev/null)" ]; then echo "    have $project/$sub"; return; fi
  mkdir -p "$dest"
  local tmp="$OUT/.fetch.tar.gz" try
  for try in 1 2 3 4; do
    if curl -s -S -L -f -m 1800 -o "$tmp" "$url"; then break; fi
    echo "    $project/$sub: try $try failed, waiting" >&2
    sleep $((try * 20))
    [ "$try" = 4 ] && { echo "cannot fetch $url" >&2; exit 1; }
  done
  # A symbolic link in an archive (expat's NOTICE is one, to its LICENSE) cannot always be made on Windows; nothing here needs one.
  if ! tar -xzf "$tmp" -C "$dest" 2> "$OUT/.tar.err"; then
    if grep -v -e "Cannot create symlink" -e "Exiting with failure status" "$OUT/.tar.err" | grep -q .; then cat "$OUT/.tar.err" >&2; exit 1; fi
  fi
  rm -f "$OUT/.tar.err"
  echo "    $project/$sub: $(stat -c%s "$tmp") bytes fetched, $(du -sh "$dest" | cut -f1) unpacked"
  rm -f "$tmp"
}

# file <project> <ref> <path>: one file (gitiles hands it over in base64). Tried again as a part is (gitiles answers
# 503 now and then), and given its name only when whole, so that a fetch cut short is not taken for the file later.
file() {
  local project="$1" ref="$2" path="$3" dest="$OUT/$1/$3" try
  if [ -s "$dest" ]; then echo "    have $project/$path"; return; fi
  mkdir -p "$(dirname "$dest")"
  for try in 1 2 3 4; do
    if curl -s -S -L -f -m 600 -o "$dest.b64" "$BASE/$project/+/$ref/$path?format=TEXT"; then break; fi
    echo "    $project/$path: try $try failed, waiting" >&2
    [ "$try" = 4 ] && { echo "cannot fetch $BASE/$project/+/$ref/$path" >&2; exit 1; }
    sleep $((try * 20))
  done
  base64 -d "$dest.b64" > "$dest.part"
  rm -f "$dest.b64"
  mv "$dest.part" "$dest"
  echo "    $project/$path: $(stat -c%s "$dest") bytes"
}

echo "=== AOSP at $TAG, into $OUT ==="
part libcore .
part libnativehelper .
part external/icu icu4c/source/common
part external/icu icu4c/source/i18n
part external/fdlibm .
part external/expat .
part external/zlib .
part system/core base
part system/core liblog/include
part system/core libcutils/include
part external/boringssl src/include
# BoringSSL itself, for arm64-v8a's libcrypto.so and libssl.so: what its Android.bp and sources.bp name. (Each folder is a
# part of its own, so that a tree fetched before, which has src/include alone, gets them too.)
part external/boringssl src/crypto
part external/boringssl src/ssl
part external/boringssl src/third_party/fiat
part external/boringssl linux-aarch64
file external/boringssl "refs/tags/$TAG" Android.bp
file external/boringssl "refs/tags/$TAG" sources.bp
file external/boringssl "refs/tags/$TAG" err_data.c
part external/conscrypt common/src/jni
file external/conscrypt "refs/tags/$TAG" Android.bp
part art runtime
part art openjdkjvm
file prebuilts/vndk/v28 refs/heads/main arm/arch-arm-armv7-a-neon/shared/vndk-core/libcrypto.so
file prebuilts/vndk/v28 refs/heads/main arm/arch-arm-armv7-a-neon/shared/vndk-core/libssl.so
# The other ABIs' folders: each device's own architecture, at its base variant (not arm64's arch-arm-armv8-a or
# x86_64's arch-x86-x86_64, the 32-bit libraries of a 64-bit device).
for d in arm64/arch-arm64-armv8-a x86/arch-x86-x86 x86_64/arch-x86_64-x86_64; do
  file prebuilts/vndk/v28 refs/heads/main $d/shared/vndk-core/libcrypto.so
  file prebuilts/vndk/v28 refs/heads/main $d/shared/vndk-core/libssl.so
done
du -sh "$OUT" | sed 's/^/in all: /'
