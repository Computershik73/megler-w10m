#!/usr/bin/env bash
# Builds tools/appx-tool (the core's package maker, core/src/appx/hk_appx.c, on a PC: x64) into build/appx-tool/appx-tool.exe.
# The toolchain is found as tools/build-appx.sh finds it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/appx-tool"
if [ -z "${MEGLER_VC:-}" ]; then
  for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe; do
    [ -f "$cl" ] && MEGLER_VC="${cl%/bin/Hostx64/x64/cl.exe}"
  done
fi
VC="${MEGLER_VC:?no MSVC toolset; set MEGLER_VC}"
SDK="${MEGLER_SDK:-/c/Program Files (x86)/Windows Kits/10}"
SDKV="${MEGLER_SDK_VERSION:-10.0.22621.0}"
w() { cygpath -w "$1"; }
export INCLUDE="$(w "$VC/include");$(w "$SDK/Include/$SDKV/ucrt");$(w "$SDK/Include/$SDKV/shared");$(w "$SDK/Include/$SDKV/um")"
export LIB="$(w "$VC/lib/x64");$(w "$SDK/Lib/$SDKV/ucrt/x64");$(w "$SDK/Lib/$SDKV/um/x64")"
export VSLANG=1033
export MSYS2_ARG_CONV_EXCL="*"
mkdir -p "$OUT"
"$VC/bin/Hostx64/x64/cl.exe" /nologo /O2 /W3 /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS \
  "/I$(w "$ROOT/core/src")" "/I$(w "$ROOT/core/third_party/zlib")" "/Fo$(w "$OUT")\\" "/Fe$(w "$OUT/appx-tool.exe")" \
  "$(w "$ROOT/tools/appx-tool/appx-tool.c")" "$(w "$ROOT/core/src/appx/hk_appx.c")" "$(w "$ROOT/core/third_party/zlib/crc32.c")" \
  bcrypt.lib
echo "built $OUT/appx-tool.exe"
