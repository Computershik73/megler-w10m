#!/usr/bin/env bash
# Builds, packs and signs the Megler app package.
#
#   tools/build-appx.sh [arm|x64|arm64|x86] [--auto MODES] [--libcore] [--java] [--bitgapps ZIP] [--gms APK] [--play-store APK] [--clean] [--no-pack]
#
#   arm        (default) the shipping package for Windows 10 Mobile, ARM32
#   x64, arm64, x86
#              the package for desktop Windows 10 and 11 on that processor
#              (docs/DESKTOP.md), which runs Android code of that processor:
#              x86_64, arm64-v8a, x86. The core is an ELF of Android's ABI
#              beside Megler.exe (tools/build-desktop.sh <arch> app makes it
#              and Megler.exe's half of it); the Android root is that ABI's
#              (tools/dvm-android-root.sh --abi). No ANGLE yet.
#   --auto MODES  an unattended on-device run at start-up (app/Megler/auto,
#              tools/phone.py auto): `probe` runs megler_probe, `selftest` runs
#              megler_selftest, or `probe,selftest`; `gfx` draws the scenes of
#              core/tests/mg_gfx_scenes.c with the shell's own Direct2D,
#              DirectWrite and WIC (player/GfxHost.cpp), checks them, and
#              leaves them in LocalState as shot-gfx-*.bmp; `storage` goes
#              through the phone's folders in an app's storage as an app
#              would (player/StorageHost.cpp, megler_storage_check), in a
#              folder of its own that it removes again; `net` goes through
#              the network as an app would (megler_net_check): a public name,
#              a connection to its port 80 and a HEAD request, the same
#              against a server of its own on loopback, a UDP round trip on
#              loopback, and the interfaces; `codec` runs the decoders a
#              Java app's MediaCodec has (player/CodecHost.cpp: Media
#              Foundation's transforms) through the tests of
#              core/tests/mg_codec_tests.c, on the data app's streams and on
#              streams made here to time them by (720p and 1080p H.264, 1080p
#              H.265; tools/media-test-streams.py, ffmpeg), all in the
#              package's folder `codec`; `play` opens the
#              library's game; `java` starts the packaged Java test app hello.apk, takes
#              its picture every five seconds, taps its button and presses
#              Back (it brings --java with it). Default: none -- the normal
#              app.
#   --libcore  the package also carries what Java runs on, as the folder
#              `android` (tools/dvm-android-root.sh puts it together: Android
#              9's libcore as six DEX files, its native libraries, ICU's
#              data, the time zones, and the Java framework where
#              tools/build-framework.sh has built it; 47 MB, which makes the
#              package some 20 MB larger; and Android's System WebView where
#              tools/webview-fetch.sh has fetched it, 87 MB more). An app that
#              is Java alone can then
#              be started (megler_run finds the folder there), and the
#              Self-test runs the Dalvik runtime's tests on that libcore;
#              without it they run on the runtime's built-in java.lang and
#              a Java app is told what is missing.
#   --java     --libcore, and six Java test apps beside the demo
#              (hello, demos, gl, data, network and webview APKs, built by
#              tools/build-test-app.sh, with the Java framework
#              tools/build-framework.sh builds in the folder `android`): what
#              a phone needs to show a Java app's picture. The library offers
#              to add them while it is empty, as it offers the demo.
#   --bitgapps ZIP  include the GMS, GSF and Play Store APKs from a locally
#              supplied BiTGApps archive in bitgapps/; implies --java. The
#              archive and its extracted APKs are not stored in the repository.
#   --gms APK, --play-store APK  with --bitgapps: these APKs of Google Play
#              services and Play Store take the place of the archive's (its
#              GSF stays), as bitgapps/PrebuiltGmsCore.apk and Phonesky.apk,
#              which the library offers. Not stored in the repository either.
#   --microg APK  with --bitgapps: microG's Google Play services, as
#              bitgapps/MicroGGmsCore.apk, which the library publishes for
#              the apps set to microG (Settings, Google Services).
#   --clean    recompile everything
#   --no-pack  stop after staging (no makeappx, no signing)
#
# The output: app/Megler/AppPackages/Megler_<version>_<ARCH>.appx, the
# certificate to trust (Megler.cer) beside it, the staged folder under
# build/appx-<arch>/stage, and the linker map kept as
# build/maps/Megler_<version>.map.
#
# Why a script and not MSBuild (podium-w10m's and gecko-w10m's tools/build-appx.sh
# learned it first): MSBuild's appx signing fails here with APPX0107, so the
# compiler, linker, makepri, makeappx and signtool are called directly. cl.exe,
# not clang: C++/WinRT needs C++ exceptions and clang has no SEH on 32-bit ARM
# Windows.
#
# Headers (C++/WinRT included) come from SDK 10.0.22621.0, the newest with ARM32
# libraries; the libraries the executable links against come from SDK
# 10.0.15063.0, so it imports only api-sets a 1703 phone has.
#
# The C runtime is linked statically (/MT): Megler.exe then imports nothing but
# OS api-sets, d3d11 and dxgi, and needs neither the VCLibs framework nor
# app-local runtime DLLs of its own. (ANGLE's DLLs do need a runtime; see
# "ANGLE" below.)
#
# All of core/ is compiled in, as C11: core/src/**/*.c, core/tests/*.c but
# mg_tests_main.c (megler_selftest calls the tests), and core/third_party/zlib/*.c
# but its gz*.c. The lists are globbed on every run, since the core is still
# growing. Every object goes onto the link line, so a core file with an
# unresolved symbol fails the build here, not on the phone.
#
# Environment:
#   MEGLER_VC           MSVC toolset with ARM32 tools (newest under VS 2022)
#   MEGLER_SDK          Windows Kits 10
#   MEGLER_SDK_VERSION  SDK for headers and tools    (10.0.22621.0)
#   MEGLER_SDK_LIB      SDK the executable links against (10.0.15063.0)
#   MEGLER_JOBS         parallel compile jobs
#   MEGLER_ANGLE        where libEGL.dll, libGLESv2.dll and mozglue.dll are
#                          taken from (build/angle/dist, Megler's own ANGLE, where
#                          tools/build-angle.py has made it; else C:/rw-obj/dist/bin,
#                          gecko-w10m's build)
#   MEGLER_ANGLE_MAPS   ANGLE's linker maps (build/angle/maps for Megler's own;
#                          else <ANGLE>/../../third_party/angle)
#   MEGLER_GECKO        gecko-w10m's checkout, for its packaging scripts
#                          (../gecko-w10m)
#   MEGLER_LLVM         LLVM bin, for lld-link (C:/Program Files/LLVM/bin)
#   MEGLER_PFX          the certificate to sign with, and
#   MEGLER_PFX_PASSWORD its password (see "sign" below for the defaults)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/app/Megler"
ARCH=arm
CLEAN=0
PACK=1
AUTO_PROBE=0
AUTO_SELFTEST=0
AUTO_PLAY=0
AUTO_JAVA=0
AUTO_GFX=0
AUTO_STORAGE=0
AUTO_NET=0
AUTO_CODEC=0
LIBCORE=0
JAVA=0
BITGAPPS=""
GMS_APK=""
PLAY_STORE_APK=""
MICROG_APK=""
while [ $# -gt 0 ]; do
  arg="$1"; shift
  case "$arg" in
    arm|x64|arm64|x86) ARCH="$arg" ;;
    --auto|--auto=*)
      if [ "$arg" = --auto ]; then
        [ $# -gt 0 ] || { echo "--auto needs probe, selftest or probe,selftest" >&2; exit 2; }
        modes="$1"; shift
      else
        modes="${arg#--auto=}"
      fi
      for mode in ${modes//,/ }; do
        case "$mode" in
          probe) AUTO_PROBE=1 ;;
          selftest) AUTO_SELFTEST=1 ;;
          play) AUTO_PLAY=1 ;;
          java) AUTO_JAVA=1 ;;
          gfx) AUTO_GFX=1 ;;
          storage) AUTO_STORAGE=1 ;;
          net) AUTO_NET=1 ;;
          codec) AUTO_CODEC=1 ;;
          none) ;;
          *) echo "unknown --auto mode: $mode (probe, selftest, gfx, storage, net, codec, play, java)" >&2; exit 2 ;;
        esac
      done ;;
    --libcore) LIBCORE=1 ;;
    --java) JAVA=1 ;;
    --bitgapps) [ $# -gt 0 ] || { echo "--bitgapps needs a ZIP path" >&2; exit 2; }; BITGAPPS="$1"; shift ;;
    --bitgapps=*) BITGAPPS="${arg#--bitgapps=}" ;;
    --gms) [ $# -gt 0 ] || { echo "--gms needs an APK path" >&2; exit 2; }; GMS_APK="$1"; shift ;;
    --microg) [ $# -gt 0 ] || { echo "--microg needs an APK path" >&2; exit 2; }; MICROG_APK="$1"; shift ;;
    --microg=*) MICROG_APK="${arg#--microg=}" ;;
    --gms=*) GMS_APK="${arg#--gms=}" ;;
    --play-store) [ $# -gt 0 ] || { echo "--play-store needs an APK path" >&2; exit 2; }; PLAY_STORE_APK="$1"; shift ;;
    --play-store=*) PLAY_STORE_APK="${arg#--play-store=}" ;;
    --clean) CLEAN=1 ;;
    --no-pack) PACK=0 ;;
    -h|--help) sed -n '2,41p' "$0"; exit 0 ;;
    *) echo "unknown argument: $arg" >&2; exit 2 ;;
  esac
done
AUTO_MODES=""
[ "$AUTO_PROBE" = 1 ] && AUTO_MODES="probe"
[ "$AUTO_SELFTEST" = 1 ] && AUTO_MODES="${AUTO_MODES:+$AUTO_MODES,}selftest"
[ "$AUTO_GFX" = 1 ] && AUTO_MODES="${AUTO_MODES:+$AUTO_MODES,}gfx"
[ "$AUTO_STORAGE" = 1 ] && AUTO_MODES="${AUTO_MODES:+$AUTO_MODES,}storage"
[ "$AUTO_NET" = 1 ] && AUTO_MODES="${AUTO_MODES:+$AUTO_MODES,}net"
[ "$AUTO_CODEC" = 1 ] && AUTO_MODES="${AUTO_MODES:+$AUTO_MODES,}codec"
[ "$AUTO_PLAY" = 1 ] && AUTO_MODES="${AUTO_MODES:+$AUTO_MODES,}play"
[ "$AUTO_JAVA" = 1 ] && AUTO_MODES="${AUTO_MODES:+$AUTO_MODES,}java"
# The self-test starts a small app of its own, and one app is all a start of Megler can run.
if [ "$AUTO_SELFTEST" = 1 ] && [ "$AUTO_PLAY" = 1 ]; then echo "--auto: selftest and play cannot go together" >&2; exit 2; fi
if [ "$AUTO_JAVA" = 1 ] && { [ "$AUTO_SELFTEST" = 1 ] || [ "$AUTO_PLAY" = 1 ]; }; then echo "--auto: java cannot go with selftest or play" >&2; exit 2; fi
# The Java test app is of no use without what it runs on.
if [ "$AUTO_JAVA" = 1 ]; then JAVA=1; fi
if [ -n "$BITGAPPS" ]; then
  [ "$ARCH" = arm ] || { echo "--bitgapps requires arm" >&2; exit 2; }
  [ -f "$BITGAPPS" ] || { echo "--bitgapps archive not found: $BITGAPPS" >&2; exit 2; }
  JAVA=1
fi
for apk in "$GMS_APK" "$PLAY_STORE_APK"; do
  [ -z "$apk" ] && continue
  [ -n "$BITGAPPS" ] || { echo "--gms and --play-store go with --bitgapps (whose GSF is used)" >&2; exit 2; }
  [ -f "$apk" ] || { echo "APK not found: $apk" >&2; exit 2; }
done
if [ "$JAVA" = 1 ]; then LIBCORE=1; fi

# MARK: Tools

if [ -z "${MEGLER_VC:-}" ]; then
  for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/arm/cl.exe; do
    [ -f "$cl" ] && MEGLER_VC="${cl%/bin/Hostx64/arm/cl.exe}"
  done
fi
VC="${MEGLER_VC:-}"
if [ -z "$VC" ] || [ ! -f "$VC/bin/Hostx64/arm/cl.exe" ]; then
  echo "No MSVC toolset with ARM32 tools (VS 2022 17.14 is the last to ship them); set MEGLER_VC." >&2
  exit 1
fi
SDK="${MEGLER_SDK:-/c/Program Files (x86)/Windows Kits/10}"
SDKV="${MEGLER_SDK_VERSION:-10.0.22621.0}"
# (Desktop Windows: the SDK's own libraries, which every Windows 10 the package says it runs on has.)
if [ "$ARCH" = arm ]; then SDKV_LIB="${MEGLER_SDK_LIB:-10.0.15063.0}"; else SDKV_LIB="${MEGLER_SDK_LIB:-$SDKV}"; fi
SDKBIN="$SDK/bin/$SDKV/x64"
CL="$VC/bin/Hostx64/$ARCH/cl.exe"
LINK="$VC/bin/Hostx64/$ARCH/link.exe"
case "$ARCH" in
  arm) MACHINE=ARM; PKGARCH=arm; ABI=armeabi-v7a ;;
  x64) MACHINE=X64; PKGARCH=x64; ABI=x86_64 ;;
  arm64) MACHINE=ARM64; PKGARCH=arm64; ABI=arm64-v8a ;;
  x86) MACHINE=X86; PKGARCH=x86; ABI=x86 ;;
esac
DESKTOP=$([ "$ARCH" = arm ] && echo 0 || echo 1)
for tool in "$CL" "$LINK" "$SDKBIN/makeappx.exe" "$SDKBIN/makepri.exe"; do
  [ -f "$tool" ] || { echo "missing: $tool" >&2; exit 1; }
done
[ -d "$SDK/Lib/$SDKV_LIB/um/$ARCH" ] || { echo "missing: SDK $SDKV_LIB libraries for $ARCH" >&2; exit 1; }

w() { cygpath -w "$1"; }
export INCLUDE="$(w "$VC/include");$(w "$SDK/Include/$SDKV/ucrt");$(w "$SDK/Include/$SDKV/shared");$(w "$SDK/Include/$SDKV/um");$(w "$SDK/Include/$SDKV/winrt");$(w "$SDK/Include/$SDKV/cppwinrt")"
export LIB="$(w "$VC/lib/$ARCH");$(w "$SDK/Lib/$SDKV_LIB/ucrt/$ARCH");$(w "$SDK/Lib/$SDKV_LIB/um/$ARCH")"
# The cross compiler loads its helper DLLs (mspdb and friends) from the x64 host bin.
export PATH="$VC/bin/Hostx64/x64:$PATH"
# English diagnostics whatever the system language.
export VSLANG=1033
# msys2 would rewrite /switches as paths; every path below goes through cygpath.
export MSYS2_ARG_CONV_EXCL="*"

BUILD="$ROOT/build/appx-$ARCH"
OBJ="$BUILD/obj"
STAGE="$BUILD/stage"
OUT="$APP/AppPackages"
[ "$CLEAN" = 1 ] && rm -rf "$OBJ"
mkdir -p "$OBJ" "$OUT"
JOBS="${MEGLER_JOBS:-$(nproc 2>/dev/null || echo 4)}"

echo "=== Megler for $([ "$DESKTOP" = 1 ] && echo "desktop Windows" || echo "Windows 10 Mobile"): $ARCH${AUTO_MODES:+, auto run: $AUTO_MODES} ==="
echo "    MSVC $(basename "$VC"), headers SDK $SDKV, libraries SDK $SDKV_LIB"

# MARK: Compile

COMMON=(/nologo /c /O2 /MT /utf-8 /Zi /FS
        /DWINAPI_FAMILY=WINAPI_FAMILY_APP /DWIN32 /D_WIN32 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN
        /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS /DNDEBUG)
[ "$ARCH" = arm ] && COMMON+=(/D_ARM_)
CXX=(/EHsc /GR /permissive- /Zc:__cplusplus)

# newest_mtime <dir> <name pattern>...: the newest modification time among
# the matching files, so an edited header recompiles what may include it.
newest_mtime() {
  local dir="$1"; shift
  local args=() first=1
  for pattern in "$@"; do
    [ $first = 1 ] || args+=(-o)
    args+=(-name "$pattern")
    first=0
  done
  find "$dir" -type f \( "${args[@]}" \) -printf '%T@\n' 2>/dev/null | sort -n | tail -1 | cut -d. -f1
}

# compile_group <object dir> <newest dependency mtime> <flags...> -- <sources...>
# One cl invocation (parallel through /MP) for the sources of one directory
# whose object is missing or older than the source or the newest header.
compile_group() {
  local objdir="$1" depmtime="${2:-0}"; shift 2
  local flags=()
  while [ "$1" != "--" ]; do flags+=("$1"); shift; done
  shift
  stale_object() {
    local obj="$objdir/$(basename "${1%.*}").obj"
    [ ! -f "$obj" ] || [ "$1" -nt "$obj" ] || [ "$(stat -c %Y "$obj")" -lt "$depmtime" ]
  }
  local stale=()
  for src in "$@"; do
    if stale_object "$src"; then stale+=("$(w "$src")"); fi
  done
  [ ${#stale[@]} -eq 0 ] && return 0
  mkdir -p "$objdir"
  # A failed compile must not leave last time's object to be linked.
  for src in "$@"; do
    if stale_object "$src"; then rm -f "$objdir/$(basename "${src%.*}").obj"; fi
  done
  "$CL" "${COMMON[@]}" "/MP$JOBS" "${flags[@]}" "/Fo$(w "$objdir")\\" "/Fd$(w "$objdir")\\vc.pdb" "${stale[@]}" \
    | grep -v -E '^[A-Za-z0-9_+-]+\.(c|cpp)$' || true
  for src in "$@"; do
    [ -f "$objdir/$(basename "${src%.*}").obj" ] || { echo "compile failed: $src" >&2; exit 1; }
  done
  echo "    $(basename "$objdir"): ${#stale[@]} compiled"
}

if [ "$DESKTOP" = 1 ]; then
  # The core as an ELF of the processor's Android ABI, and Megler.exe's half of it (docs/DESKTOP.md).
  echo "=== core (an ELF of Android's $ABI ABI) ==="
  bash "$ROOT/tools/build-dvm-fixtures.sh"
  bash "$ROOT/tools/build-desktop.sh" "$ARCH" app | sed 's/^/    /'
  [ -f "$ROOT/build/desktop-$ARCH/app/megler-core.so" ] || { echo "the core's ELF was not built" >&2; exit 1; }
else
echo "=== zlib ==="
ZLIB="$ROOT/core/third_party/zlib"
mapfile -t ZSRC < <(find "$ZLIB" -maxdepth 1 -name '*.c' ! -name 'gz*.c' | sort)
compile_group "$OBJ/zlib" "$(newest_mtime "$ZLIB" '*.h')" /TC /w "/I$(w "$ZLIB")" -- "${ZSRC[@]}"

echo "=== core (C11) ==="
CORE="$ROOT/core/src"
TESTS="$ROOT/core/tests"
# One newest-header time for the core and its tests: they include each other
# (mg_diag.c the tests' header, the tests the core's).
CORE_HEADERS="$(newest_mtime "$ROOT/core" '*.h')"
CORE_FLAGS=(/TC /std:c11 /W3 "/I$(w "$ZLIB")")
# SQLite is compiled into the core where its amalgamation is (build/sqlite/sqlite3.c: tools/sqlite-fetch.sh fetches it;
# core/src/sqlite/mg_sqlite3.c includes it). Without it the package is built all the same, and a Java app that asks for
# a database is told, once and in a sentence, that this build has none.
if [ -f "$ROOT/build/sqlite/sqlite3.c" ] && [ -f "$ROOT/build/sqlite/sqlite3.h" ]; then
  CORE_FLAGS+=(/DMG_HAVE_SQLITE=1 "/I$(w "$ROOT/build/sqlite")")
  echo "    with SQLite ($(cut -d, -f1 "$ROOT/build/sqlite/FROM" 2> /dev/null || echo "build/sqlite"))"
else
  echo "    WITHOUT SQLite: no build/sqlite/sqlite3.c (tools/sqlite-fetch.sh)"
fi
# libwebp's decoder likewise, where its sources are (build/webp/src: tools/webp-fetch.sh fetches them; core/src/webp/mg_webp_*.c
# each compile one of its files, in plain C). The phone's own picture decoder has no WebP, so without it an app's WebP pictures
# do not decode at all.
if [ -f "$ROOT/build/webp/src/dec/webp_dec.c" ] && [ -f "$ROOT/build/webp/src/webp/decode.h" ]; then
  CORE_FLAGS+=(/DMG_HAVE_WEBP=1 "/I$(w "$ROOT/build/webp")")
  echo "    with the WebP decoder ($(cut -d, -f1 "$ROOT/build/webp/FROM" 2> /dev/null || echo "build/webp"))"
else
  echo "    WITHOUT the WebP decoder: no build/webp/src (tools/webp-fetch.sh)"
fi
# The Dalvik runtime's generated sources and its tests' DEX files, before anything is compiled.
SQLITE_FEATURE=0
WEBP_FEATURE=0
[[ " ${CORE_FLAGS[*]} " == *" /DMG_HAVE_SQLITE=1 "* ]] && SQLITE_FEATURE=1
[[ " ${CORE_FLAGS[*]} " == *" /DMG_HAVE_WEBP=1 "* ]] && WEBP_FEATURE=1
python "$(w "$ROOT/tools/invalidate-core-features.py")" "$(w "$ROOT")" "$(w "$OBJ")" "$SQLITE_FEATURE" "$WEBP_FEATURE"
bash "$ROOT/tools/build-dvm-fixtures.sh"
mapfile -t CORE_DIRS < <(find "$CORE" -type f -name '*.c' -printf '%h\n' | sort -u)
for dir in "${CORE_DIRS[@]}"; do
  rel="${dir#$CORE}"; rel="${rel#/}"
  mapfile -t srcs < <(find "$dir" -maxdepth 1 -name '*.c' | sort)
  compile_group "$OBJ/core/${rel:-root}" "$CORE_HEADERS" "${CORE_FLAGS[@]}" -- "${srcs[@]}"
done
# The tests megler_selftest runs; mg_tests_main.c is the command-line program's
# main, and core/tests/guest is Android code (tools/build-guest.sh).
mapfile -t TEST_SRC < <(find "$TESTS" -maxdepth 1 -name '*.c' ! -name 'mg_tests_main.c' | sort)
compile_group "$OBJ/tests" "$CORE_HEADERS" "${CORE_FLAGS[@]}" -- "${TEST_SRC[@]}"
fi

echo "=== shell (C++/WinRT, C++20) ==="
# What the unattended run does (auto/AutoRun.cpp), as a header of its own: a
# change of modes recompiles what includes it and nothing else.
AUTO_CONFIG="$OBJ/auto-config"
mkdir -p "$AUTO_CONFIG"
AUTO_HEADER="$(printf '#define MEGLER_AUTO_PROBE %s\n#define MEGLER_AUTO_SELFTEST %s\n#define MEGLER_AUTO_PLAY %s\n#define MEGLER_AUTO_JAVA %s\n#define MEGLER_AUTO_GFX %s\n#define MEGLER_AUTO_STORAGE %s\n#define MEGLER_AUTO_NET %s\n#define MEGLER_AUTO_CODEC %s\n' "$AUTO_PROBE" "$AUTO_SELFTEST" "$AUTO_PLAY" "$AUTO_JAVA" "$AUTO_GFX" "$AUTO_STORAGE" "$AUTO_NET" "$AUTO_CODEC")"
if [ "$(cat "$AUTO_CONFIG/AutoConfig.h" 2>/dev/null)" != "$AUTO_HEADER" ]; then
  echo "$AUTO_HEADER" > "$AUTO_CONFIG/AutoConfig.h"
  rm -f "$OBJ/shell/AutoRun.obj"
fi
SHELL_FLAGS=("${CXX[@]}" /std:c++20 /W3 /bigobj "/I$(w "$APP")" "/I$(w "$ROOT/core/include")" "/I$(w "$AUTO_CONFIG")"
             "/Fp$(w "$OBJ/shell/pch.pch")")
# (Desktop: the shell loads the core's ELF before it calls it, shell/Platform.cpp.)
[ "$DESKTOP" = 1 ] && SHELL_FLAGS+=(/DMG_ELF_HOST=1)
mkdir -p "$OBJ/shell"
SHELL_HEADERS="$(newest_mtime "$APP" '*.h')"
MEGLER_H="$(stat -c %Y "$ROOT/core/include/megler.h")"
[ "$MEGLER_H" -gt "$SHELL_HEADERS" ] && SHELL_HEADERS="$MEGLER_H"
if [ ! -f "$OBJ/shell/pch.pch" ] || [ "$APP/pch.h" -nt "$OBJ/shell/pch.pch" ]; then
  rm -f "$OBJ/shell/pch.pch"
  "$CL" "${COMMON[@]}" "${SHELL_FLAGS[@]}" /Yc"pch.h" "/Fo$(w "$OBJ/shell")\\pch.obj" \
    "/Fd$(w "$OBJ/shell")\\vc.pdb" "$(w "$APP/pch.cpp")" | grep -v '^pch.cpp$' || true
  [ -f "$OBJ/shell/pch.pch" ] || { echo "the precompiled header didn't build" >&2; exit 1; }
  find "$OBJ/shell" -name '*.obj' ! -name pch.obj -delete
  echo "    precompiled header"
fi
mapfile -t SHELL_SRC < <(find "$APP" -name '*.cpp' ! -name pch.cpp ! -name GfxHost.cpp ! -path '*/AppPackages/*' | sort)
# Shell file names are unique across its folders, so one object folder does.
compile_group "$OBJ/shell" "$SHELL_HEADERS" "${SHELL_FLAGS[@]}" /Yu"pch.h" -- "${SHELL_SRC[@]}"
# A Java app's drawing (player/GfxHost.cpp: Direct2D, DirectWrite, WIC) is plain COM with no WinRT in it and is also
# built into the core's test program on a PC (tools/build-tests.sh win): the one shell file compiled without the
# precompiled header, so that it cannot come to lean on what that brings in.
compile_group "$OBJ/gfx" "$SHELL_HEADERS" "${CXX[@]}" /std:c++20 /W3 "/I$(w "$APP")" "/I$(w "$ROOT/core/include")" -- "$APP/player/GfxHost.cpp"

# MARK: Link

echo "=== link Megler.exe ==="
if [ "$DESKTOP" = 1 ]; then
  mapfile -t OBJS < <(find "$OBJ/shell" "$OBJ/gfx" "$ROOT/build/desktop-$ARCH/app/coff" -name '*.obj' | sort)
else
  mapfile -t OBJS < <(find "$OBJ/zlib" "$OBJ/core" "$OBJ/tests" "$OBJ/shell" "$OBJ/gfx" -name '*.obj' | sort)
fi
OBJS_W=()
for o in "${OBJS[@]}"; do OBJS_W+=("$(w "$o")"); done
rm -rf "$BUILD/link" && mkdir -p "$BUILD/link"
# WindowsApp.lib first, then OneCoreUAP.lib for what the static CRT and
# C++/WinRT's error path need beyond it (CreateFileW, the console api-set,
# GetErrorInfo -- podium-w10m and gecko-w10m link the same pair); the desktop
# umbrella libraries the CRT objects name are switched off, so everything
# resolves to api-sets and nothing to kernel32.dll by name. oleaut32.dll is the
# one exception: C++/WinRT reaches SysAllocString/SysFreeString/SysStringLen
# and Get/SetErrorInfo through it, on error paths only, and gecko-w10m met
# phones whose OS lacks it -- delay-loaded, it is resolved on first use, not at
# launch. XAudio2_9.dll likewise (an app's sound; player/AudioHost.cpp
# catches its failure to load), and Media Foundation's mfplat.dll and
# mfreadwrite.dll (an app's films, player/MediaHost.cpp; a Java app's
# MediaCodec, player/CodecHost.cpp), and Direct2D's
# d2d1.dll and DirectWrite's dwrite.dll (a Java app's drawing;
# player/GfxHost.cpp, which reaches WIC through CoCreateInstance and so names
# no third DLL). No /guard:cf: nothing here is built with it,
# and gecko-w10m found the guard's tables wrong for Thumb on these phones.
# The interpreter's loop first in the program's code: its speed moves by up to two thirds when unrelated code before it
# shifts the loop's head across a fetch window (docs/JAVA.md, the interpreter's speed). Pinned, a change elsewhere in the
# program cannot make Java apps slower or faster on the phone.
echo mg_dvm_interpret > "$BUILD/link/order.txt"
# (On the desktop the interpreter is in the core's ELF, which this link does not see.)
ORDER=("/ORDER:@$(w "$BUILD/link/order.txt")")
[ "$DESKTOP" = 1 ] && ORDER=()
"$LINK" /nologo "${OBJS_W[@]}" "/OUT:$(w "$BUILD/link/Megler.exe")" "/PDB:$(w "$BUILD/link/Megler.pdb")" \
  "/MAP:$(w "$BUILD/link/Megler.map")" ${ORDER[@]+"${ORDER[@]}"} /DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO \
  /APPCONTAINER /SUBSYSTEM:WINDOWS "/MACHINE:$MACHINE" /DYNAMICBASE /NXCOMPAT /MANIFEST:NO \
  WindowsApp.lib OneCoreUAP.lib d3d11.lib dxgi.lib mfplat.lib mfreadwrite.lib mfuuid.lib d2d1.lib dwrite.lib \
  /DELAYLOAD:oleaut32.dll /DELAYLOAD:xaudio2_9.dll /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll \
  /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll delayimp.lib \
  /NODEFAULTLIB:kernel32.lib /NODEFAULTLIB:user32.lib /NODEFAULTLIB:advapi32.lib /NODEFAULTLIB:ole32.lib \
  /NODEFAULTLIB:oleaut32.lib /NODEFAULTLIB:shell32.lib /NODEFAULTLIB:gdi32.lib /NODEFAULTLIB:uuid.lib \
  /NODEFAULTLIB:runtimeobject.lib
echo "    $(stat -c%s "$BUILD/link/Megler.exe") bytes, ${#OBJS[@]} objects"
# dependents <image> [2]: the DLL names its import table asks the loader for
# when it is loaded -- or, with 2, those of its delay-load table, which are
# only asked for when a function of theirs is first called. (The dumper lists
# the one after the other, each under a heading; told apart by their order,
# not by the heading's wording, which follows the system's language.)
dependents() {
  "$LINK" /dump /dependents "$(w "$1")" | tr -d '\r' | awk -v want="${2:-1}" '
    /^    [A-Za-z0-9._-]+\.[dD][lL][lL]$/ { if (!inside) { block++; inside = 1 } if (block == want) print $1; next }
    /^$/ { next }
    { inside = 0 }'
}
# What the executable asks the loader for at launch: api-sets, d3d11 and dxgi,
# and nothing else -- anything more would have to exist on the phone by that
# name. bcrypt.dll does (the core's random numbers; gecko-w10m's mozglue.dll
# imports it by that name on the same phones). So do ws2_32.dll and
# iphlpapi.dll (the core's sockets, its resolver and the list of interfaces):
# there is no api-set for Winsock or the IP helper, and WindowsApp.lib itself,
# the app partition's library, imports them by these names; gecko-w10m's
# xul.dll imports both too.
mapfile -t DEPENDENTS < <(dependents "$BUILD/link/Megler.exe")
mapfile -t DELAYED < <(dependents "$BUILD/link/Megler.exe" 2)
ODD=()
for dll in "${DEPENDENTS[@]}"; do
  case "${dll,,}" in
    api-ms-win-*|ext-ms-win-*|d3d11.dll|dxgi.dll|bcrypt.dll|ws2_32.dll|iphlpapi.dll) ;;
    *) ODD+=("$dll") ;;
  esac
done
echo "    imports ${#DEPENDENTS[@]} DLLs by name${DELAYED[*]:+ (and, delay-loaded, ${DELAYED[*]})}${ODD[*]:+; WARNING, not known to be on a phone: ${ODD[*]}}"

# MARK: Stage

# A stage left over from the last build must really be gone: just-written
# files can be held open a moment (the antivirus), and rm -rf then leaves
# directories behind without failing.
for try in 1 2 3 4 5; do
  rm -rf "$STAGE" 2> /dev/null || true
  [ -e "$STAGE" ] || break
  sleep 2
done
[ -e "$STAGE" ] && { echo "cannot remove $STAGE" >&2; exit 1; }
mkdir -p "$STAGE/Assets"

# The build number: the version's fourth field. It only ever goes up --
# Windows installs only a strictly higher version over an installed one -- so
# app/Megler/build-number.txt is committed and never reset.
COUNTER="$APP/build-number.txt"
[ -f "$COUNTER" ] || echo 0 > "$COUNTER"
BUILDNO="$(( $(tr -cd '0-9' < "$COUNTER") + 1 ))"
echo "$BUILDNO" > "$COUNTER"
# The first three fields are the manifest's; its first ' Version=' is the
# Identity's.
BASE="$(grep -o ' Version="[0-9]*\.[0-9]*\.[0-9]*' "$APP/Package.appxmanifest" | head -1 | sed 's/.*"//')"
[ -n "$BASE" ] || BASE=0.1.0
VERSION="$BASE.$BUILDNO"
sed -e "/<Identity/,/\/>/ s/Version=\"[0-9.]*\"/Version=\"$VERSION\"/" \
    -e "/<Identity/,/\/>/ s/ProcessorArchitecture=\"[a-z0-9]*\"/ProcessorArchitecture=\"$PKGARCH\"/" \
    "$APP/Package.appxmanifest" > "$STAGE/AppxManifest.xml"
grep -q "Version=\"$VERSION\"" "$STAGE/AppxManifest.xml" || { echo "couldn't set the version in the manifest" >&2; exit 1; }
echo "=== stage: version $VERSION ($PKGARCH) ==="
# The map of this very build, to put names to addresses in a phone's log, and
# what it was built as.
mkdir -p "$ROOT/build/maps"
cp "$BUILD/link/Megler.map" "$ROOT/build/maps/Megler_$VERSION.map"
echo "arch=$PKGARCH auto=${AUTO_MODES:-none} java=$JAVA libcore=$LIBCORE" > "$ROOT/build/maps/Megler_$VERSION.txt"
echo "    map: build/maps/Megler_$VERSION.map"

cp "$BUILD/link/Megler.exe" "$STAGE/"
if [ "$DESKTOP" = 1 ]; then
  cp "$ROOT/build/desktop-$ARCH/app/megler-core.so" "$STAGE/"
  echo "    megler-core.so: $(stat -c %s "$STAGE/megler-core.so") bytes, the core of Android's $ABI ABI"
fi
# The Android root of the package's ABI (tools/dvm-android-root.sh --abi).
ANDROID_ROOT="$ROOT/build/android9/root"
[ "$DESKTOP" = 1 ] && ANDROID_ROOT="$ROOT/build/android9/root-$ABI"
cp "$APP/Assets/"*.png "$STAGE/Assets/"
# Megler's own text is Roboto, Android's face, and its monospace Droid Sans Mono (ui/Theme.h): the Android root's fonts
# (tools/android9-unpack.sh fonts), where they are. Without them the shell's text is Segoe UI.
mkdir -p "$STAGE/fonts"
for f in Roboto-Regular.ttf Roboto-Medium.ttf Roboto-Bold.ttf DroidSansMono.ttf; do
  # (Or from the system image's fonts, which every ABI's root has: a desktop ABI's root may not be made yet.)
  if [ -f "$ANDROID_ROOT/fonts/$f" ]; then cp "$ANDROID_ROOT/fonts/$f" "$STAGE/fonts/"
  elif [ -f "$ROOT/build/android9/system/fonts/$f" ]; then cp "$ROOT/build/android9/system/fonts/$f" "$STAGE/fonts/"; fi
done
echo "    fonts: $(ls "$STAGE/fonts" | tr '\n' ' ')"

# The self-test's guest libraries: megler_selftest loads them from
# <package>/selftest (core/src/mg_diag.c).
GUEST="$ROOT/build/guest/$ABI"
# (The demo is 32-bit ARM code: a desktop package has none, and its library offers the Java test apps instead.)
DEMO="$ROOT/build/guest/demo/megler-demo.apk"
[ "$DESKTOP" = 1 ] && DEMO=""
# (Made here for the phone; a desktop package takes what tools/build-guest.sh made for its ABI, if anything.)
if [ "$DESKTOP" = 0 ] && { ! ls "$GUEST"/*.so > /dev/null 2>&1 || [ ! -f "$DEMO" ] || [ "$ROOT/core/tests/guest/t_demo.c" -nt "$DEMO" ] \
   || [ ! -f "$GUEST/libt_jni.so" ] || [ "$ROOT/core/tests/guest/t_jni.c" -nt "$GUEST/libt_jni.so" ] \
   || [ ! -f "$GUEST/runtime/libt_ruser.so" ] \
   || [ ! -f "$GUEST/libt_svc_arm.so" ] || [ ! -f "$GUEST/libt_svc_diag.so" ] \
   || [ "$ROOT/core/tests/guest/t_svc.c" -nt "$GUEST/libt_svc.so" ] \
   || [ ! -f "$GUEST/libt_go.so" ] || [ "$ROOT/core/tests/guest/t_go.c" -nt "$GUEST/libt_go.so" ]; }; then
  echo "    building the guest test libraries (tools/build-guest.sh)"
  bash "$ROOT/tools/build-guest.sh" > "$BUILD/build-guest.log" 2>&1 || echo "    WARNING: tools/build-guest.sh failed (build/appx-$ARCH/build-guest.log)" >&2
fi
if ls "$GUEST"/*.so > /dev/null 2>&1; then
  mkdir -p "$STAGE/selftest"
  cp "$GUEST"/*.so "$STAGE/selftest/"
  # The runtime's libt_same.so and libt_ruser.so, in a folder of their own as on the PC (test_guest_kinds in core/tests/mg_tests.c).
  if [ -d "$GUEST/runtime" ]; then mkdir -p "$STAGE/selftest/runtime" && cp "$GUEST"/runtime/*.so "$STAGE/selftest/runtime/"; fi
  # Every library tools/build-guest.sh made is where the Self-test looks for it: one left out fails on the phone alone.
  MISSING=""
  for f in $(cd "$GUEST" && find . -name '*.so' | sed 's|^\./||'); do [ -f "$STAGE/selftest/$f" ] || MISSING="$MISSING $f"; done
  [ -z "$MISSING" ] || { echo "the self-test's libraries left out of the package:$MISSING" >&2; exit 1; }
  # The Dalvik runtime's tests read their DEX files from selftest/dvm (tools/build-dvm-fixtures.sh made them).
  if [ -d "$GUEST/dvm" ]; then mkdir -p "$STAGE/selftest/dvm" && cp "$GUEST"/dvm/* "$STAGE/selftest/dvm/"; fi
  ls "$GUEST"/*.apk > /dev/null 2>&1 && cp "$GUEST"/*.apk "$STAGE/selftest/"
  echo "    selftest: $(ls "$STAGE/selftest" | tr '\n' ' ')"
else
  echo "    WARNING: no guest test libraries in build/guest/$ABI; the self-test will fail the tests that load them" >&2
fi
# (The DEX files are every ABI's, and tools/build-dvm-fixtures.sh keeps them beside 32-bit ARM's libraries: a desktop
# package without guest libraries of its own takes them from there, or the runtime's tests fail for want of them.)
if [ ! -d "$STAGE/selftest/dvm" ] && [ -d "$ROOT/build/guest/armeabi-v7a/dvm" ]; then
  mkdir -p "$STAGE/selftest/dvm" && cp "$ROOT/build/guest/armeabi-v7a/dvm/"* "$STAGE/selftest/dvm/"
  echo "    selftest: the Dalvik runtime's DEX files, $(ls "$STAGE/selftest/dvm" | wc -l) files"
fi
if [ "$JAVA" = 1 ]; then
  # The Java framework and the test apps, built where their sources are newer (a JDK and the Android SDK's build tools).
  # (With msys's conversion of paths, which this script turns off for its own tools: build-framework.sh hands Python
  # /c/... paths, and in a fresh checkout, where the framework is not built yet, Python was given C:\c\Users\....)
  { env -u MSYS2_ARG_CONV_EXCL bash "$ROOT/tools/build-framework.sh" && env -u MSYS2_ARG_CONV_EXCL bash "$ROOT/tools/build-test-app.sh"; } > "$BUILD/build-java.log" 2>&1 ||
    { echo "--java: the Java framework or the test apps could not be built (build/appx-$ARCH/build-java.log)" >&2; exit 1; }
fi
if [ "$LIBCORE" = 1 ]; then
  # The Android root, where megler_run looks for it (core/src/drivers/mg_java.c) and the runtime's tests do (core/tests/mg_tests.c).
  bash "$ROOT/tools/dvm-android-root.sh" --abi "$ABI" > /dev/null
  cp -r "$ANDROID_ROOT" "$STAGE/android"
  echo "    android: Android 9's libcore$([ -f "$STAGE/android/framework/megler-framework.dex" ] && echo " and the Java framework"), $(find "$STAGE/android" -type f | wc -l) files, $(du -sk "$STAGE/android" | cut -f1) KB"
fi
# What the auto run `codec` decodes (app/Megler/auto/AutoRun.cpp): the data app's streams, and streams to time the
# decoders by, made here with ffmpeg (they are not in the repository: 4 MB) -- without Python or ffmpeg, only the first.
if [ "$AUTO_CODEC" = 1 ]; then
  mkdir -p "$STAGE/codec"
  cp "$ROOT/core/tests/app/data/assets/"*.mgau "$STAGE/codec/"
  if ! python "$(w "$ROOT/tools/media-test-streams.py")" --speed "$(w "$STAGE/codec")" > "$BUILD/media-speed.log" 2>&1; then
    echo "    WARNING: the streams to time the decoders by could not be made (build/appx-$ARCH/media-speed.log): only the tests run" >&2
  fi
  echo "    codec: $(ls "$STAGE/codec" | tr '\n' ' ')"
fi
# The demo the library offers when it is empty (app/Megler/library/Library.cpp).
if [ -n "$DEMO" ] && [ -f "$DEMO" ]; then
  mkdir -p "$STAGE/demo"
  cp "$DEMO" "$STAGE/demo/"
  echo "    demo: megler-demo.apk"
fi
# The Java test apps beside it, which the library offers likewise and the auto run `java` starts.
if [ "$JAVA" = 1 ]; then
  mkdir -p "$STAGE/demo"
  for app in hello demos gl data network webview; do
    [ -f "$ROOT/build/guest/java/$app.apk" ] || { echo "--java: no build/guest/java/$app.apk" >&2; exit 1; }
    cp "$ROOT/build/guest/java/$app.apk" "$STAGE/demo/"
  done
  echo "    demo: hello.apk, demos.apk, gl.apk, data.apk, network.apk, webview.apk (Java)"
  for need in framework/core-oj.dex framework/megler-framework.dex framework/framework-res.apk lib/libjavacore.so; do
    [ -f "$STAGE/android/$need" ] || echo "    WARNING: no android/$need in the package: a Java app will not start, or will start bare" >&2
  done
fi
if [ -n "$BITGAPPS" ]; then
  python "$(w "$ROOT/tools/bitgapps-extract.py")" "$(w "$BITGAPPS")" "$(w "$STAGE/bitgapps")"
  # The APKs given instead of the archive's, under the names the library offers them by.
  [ -n "$MICROG_APK" ] && { cp "$MICROG_APK" "$STAGE/bitgapps/MicroGGmsCore.apk" && echo "    microG: $(basename "$MICROG_APK"), for apps set to it"; }
  [ -n "$GMS_APK" ] && cp "$GMS_APK" "$STAGE/bitgapps/PrebuiltGmsCore.apk" &&
    echo "    instead: PrebuiltGmsCore.apk is $(basename "$GMS_APK") ($(stat -c %s "$GMS_APK") bytes, sha256 $(sha256sum "$GMS_APK" | cut -d' ' -f1))"
  [ -n "$PLAY_STORE_APK" ] && cp "$PLAY_STORE_APK" "$STAGE/bitgapps/Phonesky.apk" &&
    echo "    instead: Phonesky.apk is $(basename "$PLAY_STORE_APK") ($(stat -c %s "$PLAY_STORE_APK") bytes, sha256 $(sha256sum "$PLAY_STORE_APK" | cut -d' ' -f1))"
fi

# MARK: ANGLE
#
# Android apps draw with OpenGL ES; ANGLE turns that into Direct3D 11, and
# the core opens it with LoadPackagedLibrary("libGLESv2.dll"/"libEGL.dll").
# The two DLLs are the ones gecko-w10m builds for these phones as part of
# Firefox, and they go through what gecko-w10m's tools/build-appx.sh does to
# its engine's DLLs before a phone will load and run them -- its own scripts,
# called by path, in its order:
#
#  1. mozglue.dll beside them: both import their allocator from it (malloc,
#     free, moz_xmalloc, operator delete). It needs nothing else of Gecko's.
#  2. The C runtime, app-local: msvcp140.dll and vcruntime140.dll are the UWP
#     (app container) builds out of the VCLibs framework package, under the
#     plain names the three DLLs import, with msvcp140's own import of
#     vcruntime140_app.dll rewritten to vcruntime140.dll -- a phone resolves
#     the _app names through the framework package, which this package
#     neither declares nor carries.
#  3. gen-apiset-shims.py: for every api-set name a binary in the package
#     imports, a forwarder DLL of that name to the DLL hosting the family on
#     the phone. Where the phone's api-set schema knows the name the file is
#     never opened; where it does not, the forwarder stands in. (Its
#     legacy-name layer -- kernel32.dll, ole32.dll and friends renamed and
#     forwarded, for 1607 phones that lack them -- stays off as in
#     gecko-w10m's own builds: 1709 has the real DLLs. GECKO_W10M_LEGACY_SHIMS=1
#     in the environment turns it on.)
#  4. strip-cfg.py: Control Flow Guard off in the three lld-linked DLLs; its
#     tables lack the Thumb bit and the guarded calls land in ARM state.
#  5. fix-thumb-pointers.py: link-time function pointers lld-link made
#     without the Thumb bit get it back.
#  6. patch-vcall-thunks.py, with ANGLE's linker maps: clang's 32-bit ARM
#     virtual-call thunks destroy r1, the first argument; each is rewritten to
#     use r12. Without it every glUniform* reaches the Direct3D backend with
#     its location replaced.
#
# d3dcompiler_47.dll, which libGLESv2.dll also imports, is the phone's own.
#
# Without any of the pieces the package is built all the same, with no ANGLE:
# the app then says "ANGLE missing" and nothing can draw.
stage_angle() {
  # Megler's own ANGLE where tools/build-angle.py has made one (gecko-w10m's with Megler's changes, angle/*.patch), else
  # gecko-w10m's as its build left it.
  local own="$ROOT/build/angle/dist" angle maps_default
  if [ -z "${MEGLER_ANGLE:-}" ] && [ -f "$own/libGLESv2.dll" ]; then
    angle="$own"
    maps_default="$ROOT/build/angle/maps"
  else
    angle="${MEGLER_ANGLE:-/c/rw-obj/dist/bin}"
    maps_default="$angle/../../third_party/angle"
  fi
  local gecko="${MEGLER_GECKO:-$(dirname "$ROOT")/gecko-w10m}"
  local llvm="${MEGLER_LLVM:-/c/Program Files/LLVM/bin}"
  local maps="${MEGLER_ANGLE_MAPS:-$maps_default}"
  local vclibs="/c/Program Files (x86)/Microsoft SDKs/Windows Kits/10/ExtensionSDKs/Microsoft.VCLibs/14.0/Appx/Retail/ARM/Microsoft.VCLibs.arm.14.00.appx"
  local dll script
  for dll in libEGL.dll libGLESv2.dll mozglue.dll; do
    [ -f "$angle/$dll" ] || { ANGLE_WHY="no $dll in $angle (MEGLER_ANGLE)"; return 1; }
  done
  for script in gen-apiset-shims.py strip-cfg.py fix-thumb-pointers.py patch-vcall-thunks.py; do
    [ -f "$gecko/tools/$script" ] || { ANGLE_WHY="no tools/$script in $gecko (MEGLER_GECKO)"; return 1; }
  done
  command -v python > /dev/null || { ANGLE_WHY="no python for gecko-w10m's scripts"; return 1; }
  [ -f "$llvm/lld-link.exe" ] || { ANGLE_WHY="no lld-link.exe in $llvm (MEGLER_LLVM)"; return 1; }
  [ -f "$vclibs" ] || { ANGLE_WHY="no VCLibs ARM package for the C runtime ($vclibs)"; return 1; }

  cp "$angle/libEGL.dll" "$angle/libGLESv2.dll" "$angle/mozglue.dll" "$STAGE/"
  # (2), as gecko-w10m's tools/build-appx.sh does it.
  python - "$(w "$vclibs")" "$(w "$STAGE")" <<'PY' || { ANGLE_WHY="the C runtime could not be taken out of the VCLibs package"; return 1; }
import re, sys, zipfile
package, stage = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(package) as z:
    names = {n.lower(): n for n in z.namelist()}
    for source, target in (("vcruntime140_app.dll", "vcruntime140.dll"), ("msvcp140_app.dll", "msvcp140.dll")):
        data = z.read(names[source])
        if target == "msvcp140.dll":
            # The name is stored as VCRUNTIME140_APP.dll; the field keeps its
            # length, NUL-padded.
            pattern = re.compile(rb"vcruntime140_app[.]dll", re.IGNORECASE)
            count = len(pattern.findall(data))
            assert count >= 1, "import name not found"
            data = pattern.sub(lambda m: b"vcruntime140.dll" + bytes(4), data)
            print("    msvcp140.dll: %d import name(s) rewritten to vcruntime140.dll" % count)
        open(stage + "\\" + target, "wb").write(data)
PY
  echo "    C runtime from VCLibs 14.00 ARM (UWP, app container), plain names only"
  # (3)
  python "$(w "$gecko/tools/gen-apiset-shims.py")" "$(w "$STAGE")" "$(w "$BUILD/shims")" "$(w "$CL")" "$(w "$llvm/lld-link.exe")" \
    "$(w "$SDK/Lib/$SDKV_LIB/um/arm")" || { ANGLE_WHY="gen-apiset-shims.py failed"; return 1; }
  # (4), (5)
  local targets=("$(w "$STAGE/libEGL.dll")" "$(w "$STAGE/libGLESv2.dll")" "$(w "$STAGE/mozglue.dll")")
  python "$(w "$gecko/tools/strip-cfg.py")" "${targets[@]}" | sed 's/^/    /' || { ANGLE_WHY="strip-cfg.py failed"; return 1; }
  python "$(w "$gecko/tools/fix-thumb-pointers.py")" "${targets[@]}" | sed 's/^/    /' || { ANGLE_WHY="fix-thumb-pointers.py failed"; return 1; }
  # (6)
  local name map
  for name in libGLESv2 libEGL; do
    map="$maps/${name}_gn/${name}.map"
    [ -f "$map" ] || { ANGLE_WHY="no linker map for $name.dll ($map; MEGLER_ANGLE_MAPS) -- its virtual-call thunks would stay broken"; return 1; }
    python "$(w "$gecko/tools/patch-vcall-thunks.py")" "$(w "$STAGE/$name.dll")" "$(w "$map")" | sed 's/^/    /' ||
      { ANGLE_WHY="patch-vcall-thunks.py failed on $name.dll"; return 1; }
  done
  # What the five DLLs ask the loader for must be in the package beside them,
  # or be one of the phone's own DLLs that gecko-w10m's packages load against
  # by these names. Anything else would fail LoadPackagedLibrary with error 126.
  local dll need missing="" system=""
  for dll in libEGL.dll libGLESv2.dll mozglue.dll msvcp140.dll vcruntime140.dll; do
    while IFS= read -r need; do
      [ -n "$need" ] || continue
      # (Case-blind: the import tables say KERNEL32.dll and MSVCP140.dll.)
      if [ -n "$(find "$STAGE" -maxdepth 1 -iname "$need" -print -quit)" ]; then continue; fi
      case "${need,,}" in
        kernel32.dll|ole32.dll|ntdll.dll|bcrypt.dll|d3d11.dll|dxgi.dll|d3dcompiler_47.dll)
          case " $system " in *" ${need,,} "*) ;; *) system="$system ${need,,}" ;; esac ;;
        *) missing="$missing $dll->$need" ;;
      esac
    done < <(dependents "$STAGE/$dll")
  done
  [ -z "$missing" ] || { ANGLE_WHY="imports with nothing to satisfy them:$missing"; return 1; }
  echo "    every import is in the package or the phone's own:$system"
  echo "    ANGLE from $angle: libEGL.dll, libGLESv2.dll, mozglue.dll, the C runtime and $(ls "$STAGE" | grep -c -i -E '^(api|ext)-ms-win-') api-set forwarders"
}
if [ "$ARCH" = arm ]; then
  echo "=== ANGLE ==="
  ANGLE_WHY=""
  if ! stage_angle; then
    # Nothing half-staged: an ANGLE that loads and then misbehaves is worse
    # than none.
    rm -f "$STAGE"/libEGL.dll "$STAGE"/libGLESv2.dll "$STAGE"/mozglue.dll "$STAGE"/msvcp140.dll "$STAGE"/vcruntime140.dll \
          "$STAGE"/api-ms-win-*.dll "$STAGE"/ext-ms-win-*.dll
    echo "    WARNING: ANGLE is NOT in this package: $ANGLE_WHY" >&2
    echo "    WARNING: the app will say \"ANGLE missing\" and no Android app can draw" >&2
    if [ -n "$BITGAPPS" ] && [ "$PACK" = 1 ]; then
      echo "--bitgapps: refusing to pack an unusable app without ANGLE" >&2
      exit 1
    fi
  fi
fi

# resources.pri: every packaged XAML app carries one (gecko-w10m: older XAML
# runtimes aren't known to tolerate its absence). From the manifest and the
# assets alone.
PRITMP="$BUILD/pri"
rm -rf "$PRITMP" && mkdir -p "$PRITMP/Assets"
cp "$STAGE/AppxManifest.xml" "$PRITMP/" && cp "$STAGE/Assets/"* "$PRITMP/Assets/"
"$SDKBIN/makepri.exe" createconfig /cf "$(w "$PRITMP/priconfig.xml")" /dq en-US /pv 10.0.0 /o > /dev/null
"$SDKBIN/makepri.exe" new /pr "$(w "$PRITMP")" /cf "$(w "$PRITMP/priconfig.xml")" \
  /mn "$(w "$PRITMP/AppxManifest.xml")" /of "$(w "$STAGE/resources.pri")" /o > /dev/null
rm -rf "$PRITMP"

# The template of resources.pri for the package of one Android app (core/src/appx/mg_appx_pri.c, shell/AppPackage.cpp):
# the resource map is named after the package, which is not Megler's, and makepri is not on the phone. So one is made for
# a placeholder name of 50 characters (every app package's name has that length), and another for a name one character
# apart, which tells where in the schema checksum's data the name sits (tools/pri-tails.py); the phone puts the name in
# and corrects the checksum. appx-template\resources.txt says the placeholder and the two tails.
PRI_PLACEHOLDER="MeglerApp.00000000000000000000000000000000000000Xy"
PRI_PROBE="MeglerApp.10000000000000000000000000000000000000Xy"
make_pri() {  # <folder> <identity name> <out>
  rm -rf "$1" && mkdir -p "$1/Assets"
  tr -d '\r' < "$STAGE/AppxManifest.xml" | sed -e "0,/Name=\"Megler\"/s//Name=\"$2\"/" > "$1/AppxManifest.xml"
  cp "$STAGE/Assets/"* "$1/Assets/"
  "$SDKBIN/makepri.exe" createconfig /cf "$(w "$1/priconfig.xml")" /dq en-US /pv 10.0.0 /o > /dev/null
  "$SDKBIN/makepri.exe" new /pr "$(w "$1")" /cf "$(w "$1/priconfig.xml")" /mn "$(w "$1/AppxManifest.xml")" /of "$(w "$3")" /o > /dev/null
}
mkdir -p "$STAGE/appx-template"
make_pri "$BUILD/pri-template" "$PRI_PLACEHOLDER" "$STAGE/appx-template/resources.pri"
make_pri "$BUILD/pri-probe" "$PRI_PROBE" "$BUILD/pri-probe.pri"
PRI_TAILS="$(python "$(w "$ROOT/tools/pri-tails.py")" "$(w "$STAGE/appx-template/resources.pri")" "$(w "$BUILD/pri-probe.pri")" "$PRI_PLACEHOLDER" 10)" ||
  { echo "the app packages' resources.pri template: the name's places in its checksum were not found" >&2; exit 1; }
printf 'placeholder=%s\r\ntail1=%s\r\ntail2=%s\r\n' "$PRI_PLACEHOLDER" ${PRI_TAILS} > "$STAGE/appx-template/resources.txt"
rm -rf "$BUILD/pri-template" "$BUILD/pri-probe" "$BUILD/pri-probe.pri"
echo "    appx-template: resources.pri for app packages, the name's tails $PRI_TAILS"
echo "    staged $(find "$STAGE" -type f | wc -l) files, $(du -sk "$STAGE" | cut -f1) KB"

if [ "$PACK" = 1 ]; then
  case "$ARCH" in arm) ARCHNAME=ARM ;; arm64) ARCHNAME=ARM64 ;; *) ARCHNAME="$ARCH" ;; esac
  PKG="$OUT/Megler_${VERSION}_${ARCHNAME}.appx"
  echo "=== pack ==="
  rm -f "$PKG"
  "$SDKBIN/makeappx.exe" pack /d "$(w "$STAGE")" /p "$(w "$PKG")" /o /nv > /dev/null
  echo "=== sign ==="
  # The signer's subject must be the manifest's Publisher (CN=Computershik, as
  # podium-w10m's and gecko-w10m's packages), and a phone installs only what a
  # certificate it trusts signed. So a certificate the user's phones already
  # trust is preferred to a new one: Megler's own if there is one, then
  # Podium's, then Gecko's; only without any of them is a new self-signed one
  # made (tools/sign-appx.ps1), as app/Megler/Megler.pfx.
  SIBLINGS="$(dirname "$ROOT")"
  CERT="${MEGLER_PFX:-}"
  PASSWORD="${MEGLER_PFX_PASSWORD:-}"
  if [ -z "$CERT" ]; then
    if [ -f "$APP/Megler.pfx" ]; then
      CERT="$APP/Megler.pfx"; : "${PASSWORD:=megler}"
    elif [ -f "$SIBLINGS/podium-w10m/app/PodiumW10m/Podium.pfx" ]; then
      CERT="$SIBLINGS/podium-w10m/app/PodiumW10m/Podium.pfx"; : "${PASSWORD:=podium_w10m}"
    elif [ -f "$SIBLINGS/gecko-w10m/app/GeckoW10m/Gecko.pfx" ]; then
      CERT="$SIBLINGS/gecko-w10m/app/GeckoW10m/Gecko.pfx"; : "${PASSWORD:=gecko_w10m}"
    else
      CERT="$APP/Megler.pfx"
    fi
  fi
  : "${PASSWORD:=megler}"
  echo "    certificate: $CERT"
  PUBLISHER="$(tr -d '\r' < "$STAGE/AppxManifest.xml" | sed -n '/<Identity/,/\/>/ s/.*Publisher="\([^"]*\)".*/\1/p' | head -1)"
  # Through PowerShell: msys2 hands its children an empty TEMP, and signtool
  # needs one to repack an appx (tools/sign-appx.ps1 explains).
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(w "$ROOT/tools/sign-appx.ps1")" \
    -Package "$(w "$PKG")" -Certificate "$(w "$CERT")" -Password "$PASSWORD" -Subject "$PUBLISHER" \
    -CerOut "$(w "$OUT/Megler.cer")" -SdkBin "$(w "$SDKBIN")"
  echo
  echo "PACKAGE: $PKG ($(( $(stat -c%s "$PKG") / 1024 )) KB)"
fi
