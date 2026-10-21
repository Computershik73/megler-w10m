#!/usr/bin/env bash
# Official microG Services release; optional, never downloaded by a build.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/build/microg"
VERSION=v0.3.17.252432
SHA=e5e6cba078bdec812b5450599350e5c1a12c8b7438da5bf77123af170f8c1a35
mkdir -p "$OUT"
if [ -f "$OUT/GmsCore.apk" ] && echo "$SHA  $OUT/GmsCore.apk" | sha256sum -c -; then exit 0; fi
curl -fL --retry 3 "https://github.com/microg/GmsCore/releases/download/$VERSION/com.google.android.gms-252432034.apk" -o "$OUT/GmsCore.apk.download"
echo "$SHA  $OUT/GmsCore.apk.download" | sha256sum -c -
mv "$OUT/GmsCore.apk.download" "$OUT/GmsCore.apk"
echo "microG $VERSION ready; install this APK as a separate application, or use it for the optional integration test."
