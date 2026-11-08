#!/usr/bin/env bash
# Builds and runs the check of the shell's ZIP writer on this PC: tools/zip-writer-test.cpp with app/Megler/shell/ZipWriter.cpp,
# the file Megler.exe is built from, and the core's zlib (core/third_party/zlib but its gz*.c, as Megler.exe has it), for
# x64 with MSVC; then Python's zipfile and Explorer's ZIP folders read the ZIP it wrote and compare it with what was put in
# (tools/zip-writer-check.py).
#
#   tools/zip-writer-test.sh       -> build/zip-writer-test (the program, the ZIP, what Explorer extracted)
#
# A small build: one compiler at a time, no lock needed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/zip-writer-test"
for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe; do
  [ -f "$cl" ] && VC="${cl%/bin/Hostx64/x64/cl.exe}"
done
[ -n "${VC:-}" ] || { echo "no MSVC found" >&2; exit 1; }
SDK="/c/Program Files (x86)/Windows Kits/10"
SDKV="${MEGLER_SDK_VERSION:-10.0.22621.0}"
w() { cygpath -w "$1"; }
# (The shell's precompiled header, read as a plain header here, has C++/WinRT's projection in it.)
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
# (As for an app container, as Megler.exe is compiled: the headers then show what an app may call, and nothing else.)
"$CL" /nologo /c /O2 /MT /W3 /EHsc /GR /permissive- /std:c++20 /Zc:__cplusplus /bigobj /utf-8 /DWINAPI_FAMILY=WINAPI_FAMILY_APP \
  /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS "/I$(w "$ROOT/app/Megler")" "/Fo$(w "$OUT/obj")\\" \
  "$(w "$ROOT/app/Megler/shell/ZipWriter.cpp")" "$(w "$ROOT/tools/zip-writer-test.cpp")" | grep -v -E '^[A-Za-z0-9_.-]+\.cpp$' || true
for obj in ZipWriter zip-writer-test deflate crc32; do
  [ -f "$OUT/obj/$obj.obj" ] || { echo "compile failed: $obj" >&2; exit 1; }
done
"$VC/bin/Hostx64/x64/link.exe" /nologo "/OUT:$(w "$OUT/zip-writer-test.exe")" "$(w "$OUT/obj")\\*.obj" ole32.lib oleaut32.lib
"$OUT/zip-writer-test.exe" "$(w "$OUT/run")"
python "$(w "$ROOT/tools/zip-writer-check.py")" "$(w "$OUT/run")"
