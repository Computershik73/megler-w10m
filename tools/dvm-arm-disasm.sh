#!/usr/bin/env bash
# The interpreter as the phone's package compiles it: core/src/dvm/hk_dvm_interp.c through MSVC for 32-bit ARM with the
# flags tools/build-appx.sh gives the core (/O2, an app container's headers), disassembled. For looking at what a
# change does to the code the phone runs, which nothing on this PC can time.
#
#   tools/dvm-arm-disasm.sh [function, hk_dvm_interpret]
#
# Writes build/dvm-arm-disasm/<function>.asm and prints how long the function is and the instructions around the
# switch's jump (the loop's head, which every instruction passes through).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FUNC="${1:-hk_dvm_interpret}"
VC=""
for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/arm/cl.exe; do
  [ -f "$cl" ] && VC="${cl%/bin/Hostx64/arm/cl.exe}"
done
[ -n "$VC" ] || { echo "no MSVC with ARM32 tools" >&2; exit 1; }
SDK="/c/Program Files (x86)/Windows Kits/10"
SDKV="${MEGLET_SDK_VERSION:-10.0.22621.0}"
w() { cygpath -w "$1"; }
export INCLUDE="$(w "$VC/include");$(w "$SDK/Include/$SDKV/ucrt");$(w "$SDK/Include/$SDKV/shared");$(w "$SDK/Include/$SDKV/um")"
export MSYS2_ARG_CONV_EXCL="*" VSLANG=1033
OUT="$ROOT/build/dvm-arm-disasm"
mkdir -p "$OUT"
"$VC/bin/Hostx64/arm/cl.exe" /nologo /c /O2 /MT /utf-8 /DWINAPI_FAMILY=WINAPI_FAMILY_APP /DWIN32 /D_WIN32 /DUNICODE /D_UNICODE /DNOMINMAX \
  /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS /DNDEBUG /D_ARM_ "/Fo$(w "$OUT/hk_dvm_interp.obj")" \
  "$(w "$ROOT/core/src/dvm/hk_dvm_interp.c")" > /dev/null
"$VC/bin/Hostx64/x64/dumpbin.exe" /nologo /disasm "$(w "$OUT/hk_dvm_interp.obj")" > "$OUT/all.asm"
python - "$(w "$OUT/all.asm")" "$FUNC" "$(w "$OUT/$FUNC.asm")" <<'EOF'
import re, sys
text = open(sys.argv[1], encoding="latin-1").read().splitlines()
func, out = sys.argv[2], sys.argv[3]
body, on = [], False
for line in text:
    # a function's name starts a function; the compiler's own labels ($LN...) are inside one
    if re.match(r"^[^\s$][^:]*:$", line):
        on = line[:-1] == func
        continue
    if line.startswith("$"):
        if on:
            body.append(line)
        continue
    if on and line.strip():
        body.append(line)
open(out, "w").write("\n".join(body) + "\n")
print("%s: %d instructions" % (func, len(body)))
for i, line in enumerate(body):
    if re.search(r"\btbb\b|\btbh\b|ldr\s+pc|add\s+pc|mov\s+pc", line):
        print("-- the jump at line %d, with the 24 instructions before it:" % i)
        for l in body[max(0, i - 24):i + 1]:
            print(l)
        break
EOF
