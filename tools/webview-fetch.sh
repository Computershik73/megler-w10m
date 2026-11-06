#!/usr/bin/env bash
# Android's System WebView for the Android root, from AOSP's own server, into build/webview (not part of the repository):
#
#   tools/webview-fetch.sh [abi]      (from Git Bash or WSL; needs curl, tar, sha1sum and git)
#
#   abi: armeabi-v7a (the default, as below); arm64-v8a, x86 or x86_64 for desktop Windows' packages (docs/DESKTOP.md):
#   the same Chromium release's prebuilt for that processor, from the same commit's folder of its name, into
#   build/webview/<abi>/webview.apk.
#
#   build/webview/webview.apk         AOSP's prebuilt WebView for 32-bit ARM, Chromium 128.0.6613.88: the APK Android 15
#                                     (android-15.0.0_r36, 25Q1) builds into its image as /system/app/webview/webview.apk.
#                                     platform/external/chromium-webview at commit 27ac2bdb9a1a (AOSP's main, March 2025),
#                                     folder 128.0.6613.88/arm: 86,760,206 bytes, SHA-1 7698153247712ffb3fbc230db993b7dc67759428
#                                     (git blob 54e5b6f82a2c5677264c2d10b496fd90c2afb923). com.android.webview, versionCode
#                                     661308800, minSdkVersion 26, targetSdkVersion 35; lib/armeabi-v7a/libwebviewchromium.so
#                                     (73.6 MB) stored in it, its classes.dex, its resources a shared library's.
#   build/webview/aosp/webkit/        frameworks/base/core/java/android/webkit at android-15.0.0_r36, the release that ships
#                                     that WebView: what core/framework-overlay/android/webkit was taken from, to compare.
#
# tools/dvm-android-root.sh puts the APK in the Android root as app/webview/webview.apk, which an app sees as
# /system/app/webview/webview.apk (docs/JAVA.md, "The WebView"). Why this WebView and not the Android 9 image's own (Chromium
# 66, built for 64-bit ARM alone, its bytecode only in the image's .vdex) is said there.
#
# What is there already is not fetched again (remove it to fetch it anew).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/webview"
BASE="https://android.googlesource.com/platform"
COMMIT=27ac2bdb9a1a4e55dc65bb6cdc752dbfdea3b9f4
VERSION=128.0.6613.88
SHA1=7698153247712ffb3fbc230db993b7dc67759428
TAG=android-15.0.0_r36
mkdir -p "$OUT"

fetch() { # url, file
  local try
  for try in 1 2 3 4; do
    if curl -s -S -L -f -m 1800 -o "$2" "$1"; then return 0; fi
    echo "    try $try failed, waiting" >&2
    sleep $((try * 20))
  done
  echo "cannot fetch $1" >&2
  exit 1
}

ABI="${1:-armeabi-v7a}"
if [ "$ABI" != armeabi-v7a ]; then
  # The same release built for the desktop's processors (docs/DESKTOP.md), each the one file of its folder there, into
  # build/webview/<abi>/webview.apk: checked against its git blob in that commit.
  case "$ABI" in
    arm64-v8a) FOLDER=arm64; BLOB=258dc814a9dc02ce953889440255d6787fca7b57 ;;
    x86) FOLDER=x86; BLOB=dd8fd435da6dcfd3964db8761155958a478a0de1 ;;
    x86_64) FOLDER=x86_64; BLOB=fcd919744aaba4ff5f9d30701e0f1e135cbf0ad6 ;;
    *) echo "usage: tools/webview-fetch.sh [armeabi-v7a|arm64-v8a|x86|x86_64]" >&2; exit 2 ;;
  esac
  DEST="$OUT/$ABI/webview.apk"
  mkdir -p "$OUT/$ABI"
  if [ -f "$DEST" ] && [ "$(git hash-object "$DEST")" = "$BLOB" ]; then
    echo "    have build/webview/$ABI/webview.apk (Chromium $VERSION)"
  else
    fetch "$BASE/external/chromium-webview/+archive/$COMMIT/$VERSION/$FOLDER.tar.gz" "$OUT/.fetch.tar.gz"
    rm -rf "$OUT/.unpack"
    mkdir -p "$OUT/.unpack"
    tar -xzf "$OUT/.fetch.tar.gz" -C "$OUT/.unpack"
    [ "$(git hash-object "$OUT/.unpack/webview.apk")" = "$BLOB" ] || { echo "the WebView APK fetched for $ABI is not the one expected" >&2; exit 1; }
    mv "$OUT/.unpack/webview.apk" "$DEST"
    rm -rf "$OUT/.unpack" "$OUT/.fetch.tar.gz"
    echo "    build/webview/$ABI/webview.apk: Chromium $VERSION, $(stat -c%s "$DEST") bytes"
  fi
  exit 0
fi

if [ -f "$OUT/webview.apk" ] && echo "$SHA1  $OUT/webview.apk" | sha1sum -c --quiet 2> /dev/null; then
  echo "    have build/webview/webview.apk (Chromium $VERSION)"
else
  # gitiles hands a folder over as a tar archive; the APK is the folder's one file.
  fetch "$BASE/external/chromium-webview/+archive/$COMMIT/$VERSION/arm.tar.gz" "$OUT/.fetch.tar.gz"
  rm -rf "$OUT/.unpack"
  mkdir -p "$OUT/.unpack"
  tar -xzf "$OUT/.fetch.tar.gz" -C "$OUT/.unpack"
  echo "$SHA1  $OUT/.unpack/webview.apk" | sha1sum -c --quiet || { echo "the WebView APK fetched is not the one expected" >&2; exit 1; }
  mv "$OUT/.unpack/webview.apk" "$OUT/webview.apk"
  rm -rf "$OUT/.unpack" "$OUT/.fetch.tar.gz"
  echo "    build/webview/webview.apk: Chromium $VERSION, $(stat -c%s "$OUT/webview.apk") bytes"
fi

if [ -f "$OUT/aosp/webkit/WebViewFactory.java" ]; then
  echo "    have build/webview/aosp/webkit"
else
  mkdir -p "$OUT/aosp/webkit"
  fetch "$BASE/frameworks/base/+archive/refs/tags/$TAG/core/java/android/webkit.tar.gz" "$OUT/.fetch.tar.gz"
  tar -xzf "$OUT/.fetch.tar.gz" -C "$OUT/aosp/webkit"
  rm -f "$OUT/.fetch.tar.gz"
  echo "    build/webview/aosp/webkit: android.webkit at $TAG, $(ls "$OUT/aosp/webkit" | wc -l) files"
fi
