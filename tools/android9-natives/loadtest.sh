#!/bin/bash
# Has Meglet's own linker load what tools/android9-natives.sh built, under qemu-arm,
# and has a small guest library call into it.
#
#   tools/android9-natives/loadtest.sh        (from Git Bash)
#
# The core's test program already does what is wanted: `mg_tests --load <dir>
# <library>...` maps each library and everything it needs from <dir>, applies
# every relocation, says which imports nothing provided, rewrites the PLT
# entries and thread-pointer reads, and runs the libraries' constructors
# (DT_INIT_ARRAY). It does not call JNI_OnLoad: there is no Java runtime in
# that mode. Nothing under core/ is touched or built here: the program is the
# one tools/build-tests.sh arm last made (~/meglet-build/mg_tests in WSL),
# copied before it is run so that a build going on meanwhile cannot change it
# under qemu, and the output says when it was built. Run tools/build-tests.sh
# arm --no-run first to try the layer as it is now.
#
# Five libraries are named to it:
#
#   libcrypto.so          mg_tests knows OpenSSL's names and tries a hash, a
#                         cipher and a big-number product when it finds them
#   libjavacrypto.so      conscrypt's, with libssl.so under it
#   libjavacore.so        with all it needs: ICU, expat, libnativehelper
#   libopenjdk.so         (its libopenjdkjvm.so does not exist; that is said)
#   libnatives_probe.so   built here from probe.c, with ICU's data file in it:
#                         its constructor gives ICU the data and asks ICU,
#                         expat and libopenjdk's fdlibm questions that have one
#                         right answer
#
# Output: build/android9/lib/report/loadtest.txt (all of it) and a summary
# here; `report.py loadtest` holds it against what the files themselves say.
#
#   MEGLET_NDK, MEGLET_NDK_HOST   as for tools/android9-natives.sh
#   MEGLET_WSL_BUILD       the WSL folder tools/build-tests.sh arm built mg_tests in (~/meglet-build); the
#                             libraries are copied next to it, into <that folder>-natives
#   MEGLET_QEMU_CPU             the processor qemu-arm is to be (cortex-a7: ARMv7-A with NEON, VFPv4 and integer
#                             divide, what the ARMv7 Windows phones have; cortex-a8 has neither VFPv4 nor divide).
#                             An instruction the processor does not have ends the run with an illegal instruction.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
A=build/android9/aosp
OUT=build/android9/lib/report
WORK=build/android9/obj/loadtest
STUBS=build/android9/obj/stubs
UNSTRIPPED=build/android9/obj/unstripped
SHIM=tools/android9-natives
NDK="${MEGLET_NDK:-/c/Users/User/Documents/android-ndk-r21e}"
HOST="${MEGLET_NDK_HOST:-windows-x86_64}"
EXE=""; case "$HOST" in windows-*) EXE=".exe" ;; esac
CC="$NDK/toolchains/llvm/prebuilt/$HOST/bin/clang$EXE"
LIBS=(libcrypto.so libjavacore.so libopenjdk.so libjavacrypto.so libnatives_probe.so)
WB="${MEGLET_WSL_BUILD:-}"
[ -n "$WB" ] || WB='~/meglet-build'
WB="\$HOME${WB#\~}"
CPU="${MEGLET_QEMU_CPU:-cortex-a7}"
mkdir -p "$OUT" "$WORK"
WROOT="/mnt/${ROOT:1:1}${ROOT:2}"
for l in libjavacore.so libopenjdk.so libicuuc.so libicui18n.so libexpat.so libjavacrypto.so; do
  [ -f "$UNSTRIPPED/$l" ] || { echo "no $UNSTRIPPED/$l: run tools/android9-natives.sh first" >&2; exit 1; }
done
DAT=build/android9/system/usr/icu/icudt60l.dat
[ -f "$DAT" ] || { echo "no $DAT: run tools/android9-unpack.sh first" >&2; exit 1; }

# The probe: probe.c, and ICU's data as 24 MB of read-only bytes behind one symbol.
ARCH=(--target=armv7a-linux-androideabi28 -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=softfp -mthumb)
cat > "$WORK/probe_icudata.S" <<EOF
	.section .rodata
	.globl probe_icudata
	.type probe_icudata,%object
	.balign 16
probe_icudata:
	.incbin "$DAT"
EOF
"$CC" "${ARCH[@]}" -Os -fPIC -Wall -I$A/external/icu/icu4c/source/common -I$A/external/icu/icu4c/source/i18n -I$A/external/expat/lib \
  -c "$SHIM/probe.c" -o "$WORK/probe.o"
"$CC" "${ARCH[@]}" -c "$WORK/probe_icudata.S" -o "$WORK/probe_icudata.o"
"$CC" "${ARCH[@]}" -shared -fuse-ld=lld -nodefaultlibs -Wl,--no-undefined -Wl,-z,relro -Wl,-z,now -Wl,-z,max-page-size=4096 -Wl,--hash-style=both \
  -Wl,--exclude-libs,libunwind.a -Wl,--exclude-libs,libcompiler_rt-extras.a -Wl,--exclude-libs,libgcc_real.a \
  -Wl,-soname,libnatives_probe.so -o "$WORK/libnatives_probe.so" "$WORK/probe.o" "$WORK/probe_icudata.o" \
  -lunwind -lcompiler_rt-extras -lgcc_real "-L$STUBS" "-L$UNSTRIPPED" -licuuc -licui18n -lexpat -lopenjdk -llog -lc -lm -ldl

# The script WSL runs. Written to a file: quoting it through wsl.exe's command line is not worth trying.
cat > "$WORK/run.sh" <<EOF
OUT=$WB-natives
TESTS=$WB/mg_tests
[ -x \$TESTS ] || { echo "no \$TESTS: run tools/build-tests.sh arm --no-run first"; exit 1; }
rm -rf \$OUT && mkdir -p \$OUT/lib
cp \$TESTS \$OUT/mg_tests
echo "mg_tests: the one tools/build-tests.sh built at \$(date -r \$TESTS '+%Y-%m-%d %H:%M')"
# The libraries are read from a copy inside WSL: the same bytes, without the Windows drive in between.
cp "$WROOT"/build/android9/lib/*.so "$WROOT/$WORK/libnatives_probe.so" \$OUT/lib/
cd \$OUT
echo "--- mg_tests --load lib ${LIBS[*]}   (qemu-arm -cpu $CPU)"
timeout 600 qemu-arm -cpu $CPU -L /usr/arm-linux-gnueabihf \$OUT/mg_tests --load \$OUT/lib ${LIBS[*]} 2>&1
echo "--- exit status \$?"
EOF

MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash "$WROOT/$WORK/run.sh" > "$OUT/loadtest.txt" 2>&1 || true
tr -d '\r' < "$OUT/loadtest.txt" > "$OUT/loadtest.tmp" && mv "$OUT/loadtest.tmp" "$OUT/loadtest.txt"
grep -E "^mg_tests:|^no |^--- |unresolved imports in all|Thumb, .* ARM functions|^  [A-Z]|probe: " "$OUT/loadtest.txt" || true
echo "different unresolved names: $(grep -oE "unresolved (function|symbol|data symbol) [^ ]+" "$OUT/loadtest.txt" | awk '{print $NF}' | sort -u | wc -l) (all of the output: $OUT/loadtest.txt)"
