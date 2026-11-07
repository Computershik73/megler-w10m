#!/usr/bin/env bash
# Builds and runs the check of Save All Logs on this PC (tools/log-archive-test.cpp): app/Megler/shell/LogArchive.cpp, the file
# Megler.exe is built from, with LogMirror.cpp, ZipWriter.cpp, Files.cpp, Strings.cpp and the core's zlib, and stand-ins for
# the platform, for x64 with MSVC, compiled as for an app container; run on folders of its own, the ZIP it saves read by
# Python's zipfile (tools/log-archive-check.py). The stand-in is an Xbox: the ZIP stays in the test's LocalState\logs, and
# nothing is put in this PC's Pictures.
#
#   tools/log-archive-test.sh      -> build/log-archive-test
#
# A small build: one compiler at a time, no lock needed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/log-archive-test"
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
CL="$VC/bin/Hostx64/x64/cl.exe"

rm -rf "$OUT"
mkdir -p "$OUT/obj"
mapfile -t ZSRC < <(find "$ROOT/core/third_party/zlib" -maxdepth 1 -name '*.c' ! -name 'gz*.c' | sort)
ZW=()
for s in "${ZSRC[@]}"; do ZW+=("$(w "$s")"); done
"$CL" /nologo /c /O2 /MT /w "/Fo$(w "$OUT/obj")\\" "${ZW[@]}" | grep -v -E '^[A-Za-z0-9_]+\.c$' || true
SRC=()
for s in app/Megler/shell/LogArchive.cpp app/Megler/shell/LogMirror.cpp app/Megler/shell/ZipWriter.cpp app/Megler/shell/Files.cpp \
         app/Megler/shell/Strings.cpp tools/log-archive-test.cpp; do
  SRC+=("$(w "$ROOT/$s")")
done
"$CL" /nologo /c /O2 /MT /W3 /EHsc /GR /permissive- /std:c++20 /Zc:__cplusplus /bigobj /utf-8 \
  /DWINAPI_FAMILY=WINAPI_FAMILY_APP /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS \
  "/I$(w "$ROOT/app/Megler")" "/I$(w "$ROOT/core/include")" "/Fo$(w "$OUT/obj")\\" "${SRC[@]}" | grep -v -E '^[A-Za-z0-9_.-]+\.cpp$' || true
for obj in LogArchive LogMirror ZipWriter Files Strings log-archive-test deflate; do
  [ -f "$OUT/obj/$obj.obj" ] || { echo "compile failed: $obj" >&2; exit 1; }
done
"$VC/bin/Hostx64/x64/link.exe" /nologo "/OUT:$(w "$OUT/log-archive-test.exe")" "$(w "$OUT/obj")\\*.obj" ole32.lib oleaut32.lib \
  runtimeobject.lib WindowsApp.lib
"$OUT/log-archive-test.exe" "$(w "$OUT/run")" | tee "$OUT/run.txt"
python "$(w "$ROOT/tools/log-archive-check.py")" "$(tail -1 "$OUT/run.txt" | tr -d '\r')" "$(w "$OUT/run/LocalState")"
