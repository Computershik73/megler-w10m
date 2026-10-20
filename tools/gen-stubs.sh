#!/usr/bin/env bash
# Assembles core/src/arm/mg_stubs.S and writes its bytes and symbol offsets
# into core/src/arm/mg_stubs_blob.h (committed, so building the core needs no
# assembler). Run it after editing mg_stubs.S.
#
# The assembler is WSL's arm-linux-gnueabihf binutils (the same toolchain the
# qemu-arm tests are built with).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/core/src/arm/mg_stubs.S"
OUT="$ROOT/core/src/arm/mg_stubs_blob.h"
WORK="$ROOT/build/stubs"
mkdir -p "$WORK"
cp "$SRC" "$WORK/mg_stubs.S"
# /c/Users/... here is /mnt/c/Users/... there; msys must not rewrite that path on the way.
WWORK="/mnt/${WORK:1:1}${WORK:2}"
MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cd '$WWORK' && arm-linux-gnueabihf-as -o mg_stubs.o mg_stubs.S && arm-linux-gnueabihf-objcopy -O binary -j .text mg_stubs.o mg_stubs.bin && arm-linux-gnueabihf-nm mg_stubs.o > mg_stubs.nm && arm-linux-gnueabihf-objdump -d mg_stubs.o > mg_stubs.lst"
python "$ROOT/tools/gen-stubs.py" "$WORK/mg_stubs.bin" "$WORK/mg_stubs.nm" "$OUT"
echo "wrote $OUT ($(stat -c%s "$WORK/mg_stubs.bin") bytes of code); listing: build/stubs/mg_stubs.lst"
