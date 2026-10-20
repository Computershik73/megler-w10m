#!/usr/bin/env bash
# Builds the publisher cache folder probe (app/PubCacheProbe) twice, as the packages MeglerProbeA and MeglerProbeB: one
# program, two manifests that differ in name, phone product id and display name, signed as Megler's packages are.
#
#   tools/build-pubcache-probe.sh       -> build/pubcache-probe/MeglerProbeA.appx, MeglerProbeB.appx
#
# tools/pubcache-probe.py installs them on the phone, starts A and then B, and says what each found.
# The toolchain is found as tools/build-appx.sh finds it (MEGLER_VC, MEGLER_SDK, MEGLER_SDK_VERSION,
# MEGLER_SDK_LIB).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/app/PubCacheProbe"
OUT="$ROOT/build/pubcache-probe"

if [ -z "${MEGLER_VC:-}" ]; then
  for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/arm/cl.exe; do
    [ -f "$cl" ] && MEGLER_VC="${cl%/bin/Hostx64/arm/cl.exe}"
  done
fi
VC="${MEGLER_VC:?no MSVC toolset with ARM32 tools; set MEGLER_VC}"
SDK="${MEGLER_SDK:-/c/Program Files (x86)/Windows Kits/10}"
SDKV="${MEGLER_SDK_VERSION:-10.0.22621.0}"
SDKV_LIB="${MEGLER_SDK_LIB:-10.0.15063.0}"
SDKBIN="$SDK/bin/$SDKV/x64"
w() { cygpath -w "$1"; }
export INCLUDE="$(w "$VC/include");$(w "$SDK/Include/$SDKV/ucrt");$(w "$SDK/Include/$SDKV/shared");$(w "$SDK/Include/$SDKV/um");$(w "$SDK/Include/$SDKV/winrt");$(w "$SDK/Include/$SDKV/cppwinrt")"
export LIB="$(w "$VC/lib/arm");$(w "$SDK/Lib/$SDKV_LIB/ucrt/arm");$(w "$SDK/Lib/$SDKV_LIB/um/arm")"
export PATH="$VC/bin/Hostx64/x64:$PATH"
export VSLANG=1033
export MSYS2_ARG_CONV_EXCL="*"

rm -rf "$OUT" && mkdir -p "$OUT/obj"
"$VC/bin/Hostx64/arm/cl.exe" /nologo /c /O2 /MT /utf-8 /EHsc /GR /permissive- /std:c++20 /W3 \
  /DWINAPI_FAMILY=WINAPI_FAMILY_APP /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /D_ARM_ \
  "/Fo$(w "$OUT/obj/App.obj")" "$(w "$SRC/App.cpp")"
"$VC/bin/Hostx64/arm/link.exe" /nologo "$(w "$OUT/obj/App.obj")" "/OUT:$(w "$OUT/Probe.exe")" /OPT:REF /INCREMENTAL:NO \
  /APPCONTAINER /SUBSYSTEM:WINDOWS /MACHINE:ARM /DYNAMICBASE /NXCOMPAT /MANIFEST:NO WindowsApp.lib OneCoreUAP.lib \
  /NODEFAULTLIB:kernel32.lib /NODEFAULTLIB:user32.lib /NODEFAULTLIB:advapi32.lib /NODEFAULTLIB:ole32.lib \
  /NODEFAULTLIB:oleaut32.lib /NODEFAULTLIB:shell32.lib /NODEFAULTLIB:gdi32.lib /NODEFAULTLIB:uuid.lib /NODEFAULTLIB:runtimeobject.lib

# A version that grows from build to build, so that the portal takes each as an update.
BUILD=$(( $(date +%s) / 60 % 65000 ))
CERT="${MEGLER_PFX:-$ROOT/app/Megler/Megler.pfx}"
PASSWORD="${MEGLER_PFX_PASSWORD:-megler}"
for spec in "A 6a1f3b2e-5c4d-4e7f-9a8b-1c2d3e4f5a01" "B 6a1f3b2e-5c4d-4e7f-9a8b-1c2d3e4f5a02"; do
  set -- $spec
  NAME="MeglerProbe$1"
  STAGE="$OUT/stage-$1"
  mkdir -p "$STAGE/Assets"
  cp "$OUT/Probe.exe" "$STAGE/"
  cp "$ROOT/app/Megler/Assets/"*.png "$STAGE/Assets/"
  sed -e "s/@NAME@/$NAME/g" -e "s/@PRODUCT@/$2/" -e "s/@DISPLAY@/Probe $1/g" -e "s/@BUILD@/$BUILD/" \
    "$SRC/AppxManifest.xml.in" > "$STAGE/AppxManifest.xml"
  "$SDKBIN/makeappx.exe" pack /d "$(w "$STAGE")" /p "$(w "$OUT/$NAME.appx")" /o /nv > /dev/null
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(w "$ROOT/tools/sign-appx.ps1")" \
    -Package "$(w "$OUT/$NAME.appx")" -Certificate "$(w "$CERT")" -Password "$PASSWORD" -Subject "CN=Computershik" \
    -CerOut "$(w "$OUT/probe.cer")" -SdkBin "$(w "$SDKBIN")" > /dev/null
  echo "PACKAGE: $OUT/$NAME.appx ($(( $(stat -c%s "$OUT/$NAME.appx") / 1024 )) KB), version 1.0.0.$BUILD"
done
