#!/bin/bash
# What the Java side takes from Android 9, out of Google's emulator image
#
#   system-images;android-28;default;arm64-v8a, revision 2
#   https://dl.google.com/android/repository/sys-img/android/arm64-v8a-28_r02.zip
#   426760297 bytes, SHA-1 e209114dd0dfc2f4e0d328f5fd7367fec39ee1bd
#
# into build/android9/system: the boot class path's bytecode (as the .vdex files
# the image keeps it in; tools/vdex-dex.py takes the DEX files out), the
# framework's resources, ICU's data, the time zones, the root certificates, and
# the fonts of Android's named families (Roboto and seven more: 30 files, 8 MB
# of the image's 68 MB of fonts; what they lack the phone's own fonts supply).
#
# The image is for 64-bit ARM only (ro.product.cpu.abilist=arm64-v8a): it has no
# /system/lib, so the native halves of libcore for a 32-bit phone are not in it.
#
#   tools/android9-unpack.sh            (in WSL, or from Git Bash: it calls WSL itself)
#   tools/android9-unpack.sh fonts      the fonts alone, into a build/android9/system that is there
#
# The zip is looked for in build/android9; system.img (2.7 GB) is unpacked next
# to it and removed again. Needs unzip and e2fsprogs' debugfs (both part of an
# Ubuntu WSL).
set -euo pipefail

if [ -z "${WSL_DISTRO_NAME:-}" ]; then
  HERE="$(cd "$(dirname "$0")" && pwd)"
  exec env MSYS_NO_PATHCONV=1 wsl.exe -- bash "$(echo "$HERE" | sed -E 's|^/([a-zA-Z])/|/mnt/\L\1/|')/android9-unpack.sh" "$@"
fi

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/build/android9"
ZIP="$DIR/arm64-v8a-28_r02.zip"
OUT="$DIR/system"
DEBUGFS="$(command -v debugfs || echo /sbin/debugfs)"
[ -f "$ZIP" ] || { echo "no $ZIP: download it first (see the top of this script)" >&2; exit 1; }
echo "e209114dd0dfc2f4e0d328f5fd7367fec39ee1bd  $ZIP" | sha1sum -c --quiet

cd "$DIR"
[ -f arm64-v8a/system.img ] || unzip -o -q "$ZIP" arm64-v8a/system.img arm64-v8a/build.prop
# The image is a disk with one partition, an ext4 file system a megabyte in.
IMG="arm64-v8a/system.img?offset=1048576"
ONLY="${1:-}"
[ "$ONLY" = fonts ] || rm -rf "$OUT"
mkdir -p "$OUT/framework" "$OUT/usr/icu" "$OUT/usr/share/zoneinfo" "$OUT/etc/security" "$OUT/lib64-for-reference" "$OUT/fonts"

take() { "$DEBUGFS" -R "dump $1 $2" "$IMG" 2> /dev/null; [ -s "$2" ] || { echo "not in the image: $1" >&2; exit 1; }; }
names() { "$DEBUGFS" -R "ls -l $1" "$IMG" 2> /dev/null | awk '$2 ~ /^10/ {print $NF}'; }

# The families /system/etc/fonts.xml gives a name to: sans-serif, sans-serif-condensed, serif, monospace, serif-monospace,
# casual, cursive, sans-serif-smallcaps. (app/Megler/player/GfxHost.cpp has the same list.)
for f in $(names /system/fonts); do
  case "$f" in
    Roboto-*.ttf|RobotoCondensed-*.ttf|NotoSerif-Regular.ttf|NotoSerif-Bold.ttf|NotoSerif-Italic.ttf|NotoSerif-BoldItalic.ttf|DroidSansMono.ttf|CutiveMono.ttf|ComingSoon.ttf|DancingScript-*.ttf|CarroisGothicSC-Regular.ttf)
      take "/system/fonts/$f" "$OUT/fonts/$f" ;;
  esac
done
take /system/etc/fonts.xml "$OUT/etc/fonts.xml"
if [ "$ONLY" = fonts ]; then
  rm -f arm64-v8a/system.img
  echo "fonts: $(ls "$OUT/fonts" | wc -l) files, $(du -sk "$OUT/fonts" | cut -f1) KB"
  exit 0
fi

for f in $(names /system/framework); do
  case "$f" in
    boot*.vdex|framework-res.apk|core-oj.jar|core-libart.jar|ext.jar|org.apache.http.legacy.boot.jar) take "/system/framework/$f" "$OUT/framework/$f" ;;
  esac
done
# Android's legacy Apache HTTP client, which an app gets on its class path when it asks for it (drivers/mg_java.c): not on
# the boot class path since Android 9, so its bytecode is in a .vdex of its own; named as the boot jars' are, for vdex-dex.py.
take /system/framework/oat/arm64/org.apache.http.legacy.boot.vdex "$OUT/framework/boot-org.apache.http.legacy.vdex"
for f in $(names /system/usr/icu); do take "/system/usr/icu/$f" "$OUT/usr/icu/$f"; done
for f in $(names /system/usr/share/zoneinfo); do take "/system/usr/share/zoneinfo/$f" "$OUT/usr/share/zoneinfo/$f"; done
"$DEBUGFS" -R "rdump /system/etc/security/cacerts $OUT/etc/security" "$IMG" 2> /dev/null
take /system/build.prop "$OUT/build.prop"
# The 64-bit libraries are of no use on the phone; their import lists say what the 32-bit ones will ask of the layer.
for f in libjavacore.so libopenjdk.so libopenjdkjvm.so libnativehelper.so libicuuc.so libicui18n.so libexpat.so libjavacrypto.so; do
  take "/system/lib64/$f" "$OUT/lib64-for-reference/$f"
done

rm -f arm64-v8a/system.img
du -sh "$OUT" | sed 's/^/unpacked: /'
ls -la "$OUT/framework" | awk '{print "  " $5, $NF}' | tail -n +4
