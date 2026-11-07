#!/usr/bin/env bash
# Builds and runs the check of the logs' mirror on this PC (tools/log-mirror-test.cpp): app/Megler/shell/LogMirror.cpp, the
# file Megler.exe is built from, with the shell's Files.cpp and Strings.cpp and stand-ins for the shared folder, LocalState
# and the shell's log, for x64 with MSVC, compiled as for an app container; then run on folders of its own.
#
#   tools/log-mirror-test.sh       -> build/log-mirror-test (the program; the folders it played a package's runs in)
#
# A small build: one compiler at a time, no lock needed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/log-mirror-test"
for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe; do
  [ -f "$cl" ] && VC="${cl%/bin/Hostx64/x64/cl.exe}"
done
[ -n "${VC:-}" ] || { echo "no MSVC found" >&2; exit 1; }
SDK="/c/Program Files (x86)/Windows Kits/10"
SDKV="${MEGLER_SDK_VERSION:-10.0.22621.0}"
w() { cygpath -w "$1"; }
export INCLUDE="$(w "$VC/include");$(w "$SDK/Include/$SDKV/ucrt");$(w "$SDK/Include/$SDKV/shared");$(w "$SDK/Include/$SDKV/um");$(w "$SDK/Include/$SDKV/winrt");$(w "$SDK/Include/$SDKV/cppwinrt")"
export LIB="$(w "$VC/lib/x64");$(w "$SDK/Lib/$SDKV/ucrt/x64");$(w "$SDK/Lib/$SDKV/um/x64")"
export PATH="$VC/bin/Hostx64/x64:$PATH"
export VSLANG=1033 MSYS2_ARG_CONV_EXCL="*"

rm -rf "$OUT"
mkdir -p "$OUT/obj"
SRC=()
for s in app/Megler/shell/LogMirror.cpp app/Megler/shell/Files.cpp app/Megler/shell/Strings.cpp tools/log-mirror-test.cpp; do SRC+=("$(w "$ROOT/$s")"); done
"$VC/bin/Hostx64/x64/cl.exe" /nologo /c /O2 /MT /W3 /EHsc /GR /permissive- /std:c++20 /Zc:__cplusplus /bigobj /utf-8 \
  /DWINAPI_FAMILY=WINAPI_FAMILY_APP /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS \
  "/I$(w "$ROOT/app/Megler")" "/I$(w "$ROOT/core/include")" "/Fo$(w "$OUT/obj")\\" "${SRC[@]}" | grep -v -E '^[A-Za-z0-9_.-]+\.cpp$' || true
for obj in LogMirror Files Strings log-mirror-test; do
  [ -f "$OUT/obj/$obj.obj" ] || { echo "compile failed: $obj" >&2; exit 1; }
done
"$VC/bin/Hostx64/x64/link.exe" /nologo "/OUT:$(w "$OUT/log-mirror-test.exe")" "$(w "$OUT/obj")\\*.obj" ole32.lib oleaut32.lib runtimeobject.lib WindowsApp.lib
"$OUT/log-mirror-test.exe" "$(w "$OUT/run")"
