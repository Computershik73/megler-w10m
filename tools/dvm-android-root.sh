#!/usr/bin/env bash
# Puts together the folder the Dalvik runtime starts Android's libcore from
# (mg_dvm_start_android): laid out like Android's /system, out of what
# tools/android9-*.sh left in build/android9.
#
#   tools/dvm-android-root.sh [--no-res | --full-res] [folder]        (build/android9/root when no folder is named)
#
#   framework/<name>.dex    the boot class path's bytecode, one DEX file each:
#                           core-oj, core-libart, conscrypt, okhttp,
#                           bouncycastle, apache-xml
#   framework/org.apache.http.legacy.dex
#                           Android's legacy Apache HTTP client, not on the
#                           boot class path: an app that asks for it in its
#                           manifest, or targets an Android before 9, has it
#                           on its class path (drivers/mg_java.c)
#   framework/core-oj.jar, core-libart.jar
#                           the jars as the device has them: no bytecode in
#                           them, but what else libcore opens them for (ICU4J
#                           reads its configuration out of core-libart.jar)
#   lib/*.so                libcore's native libraries, 32-bit ARM, where they
#                           have been built (build/android9/lib); without them
#                           the runtime still starts, and libcore is its
#                           bytecode alone. With libjavacrypto.so among them
#                           the security providers are conscrypt's, TLS
#                           included (mg_dvm_art.c, "the security providers")
#   etc/security/cacerts/*  the certificate authorities Android 9 trusts, which
#                           conscrypt's trust manager reads; without them no
#                           server's certificate is trusted
#   framework/megler-framework.dex
#                           the Java framework an app with an Activity runs
#                           on, where tools/build-framework.sh has built it
#   framework/framework-res.apk
#                           the platform's own resources, Android 9's: the
#                           themes, layouts and pictures an app names as
#                           android.R.*. Cut down to the languages and screen
#                           densities a phone needs, by the SDK's aapt2
#                           (`optimize`): Android's own file is 43 MB, with a
#                           table of 29 MB that an app holds in memory whole,
#                           for every language it speaks; with English and
#                           Russian and the two densities round a phone's it
#                           is 12 MB and its table under 2. --full-res takes
#                           Android's own as it is, and so does a PC with no
#                           aapt2 (it says so). --no-res leaves the file out:
#                           an app then starts bare -- no action bar, no
#                           button shapes -- and fails where it asks for a
#                           platform resource by id.
#   fonts/*.ttf             the fonts of Android's named families (Roboto and
#                           seven more), for the shell's drawing: a Java
#                           app's text is then as wide and as tall as its
#                           layout was made for. Without them the phone's own
#                           fonts stand in.
#   etc/fonts.xml           Android's list of those families and their
#                           files, which native code that draws its own text
#                           reads (Chromium's Skia, in the WebView)
#   usr/icu/icudt60l.dat    ICU's data
#   usr/share/zoneinfo/*    the time zones
#   app/webview/webview.apk Android's System WebView, AOSP's prebuilt of
#                           Chromium for 32-bit ARM, where Android's image has
#                           it (/system/app/webview): android.webkit.WebView
#                           loads it into the app (WebViewFactory). Where
#                           tools/webview-fetch.sh has fetched it
#                           (build/webview/webview.apk); without it a WebView
#                           cannot be made, as on a phone without one
#   lib/libwebviewchromium_plat_support.so
#                           the name of the platform's WebView support, which
#                           the WebView loads by name: a file that says so, as
#                           its natives are the core's (mg_java_webview.c)
#   lib/libc.so, libz.so ...
#                           the names of the system's libraries the core
#                           answers itself (mg_bionic_is_system_lib), each a
#                           file that says so: code that looks for them in
#                           /system/lib before loading (Facebook's SoLoader)
#                           finds them there
#
# Files are copied only when they are missing or older than their source, and
# the script says what the folder holds and how large it is.
#
#   MEGLER_RES_LANGUAGES  the languages kept in the platform's resources
#                            besides the default, as aapt2 names them (en,ru)
#   MEGLER_RES_DENSITIES  the screen densities kept (xhdpi,xxhdpi: 320 and
#                            480 dpi; a resource for another is scaled)
#   MEGLER_ANDROID_SDK    an Android SDK with build-tools 29.0.3, for aapt2
#                            (the user's Documents folder)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A9="$ROOT/build/android9"
RES=cut
if [ "${1:-}" = --no-res ]; then RES=none; shift; elif [ "${1:-}" = --full-res ]; then RES=full; shift; fi
OUT="${1:-$A9/root}"
[ -f "$A9/boot/core-oj.dex" ] || { echo "no build/android9/boot/core-oj.dex: Android 9's boot class path has not been unpacked (tools/android9-*.sh)" >&2; exit 1; }
mkdir -p "$OUT/framework" "$OUT/lib" "$OUT/usr/icu" "$OUT/usr/share/zoneinfo"
put() { # source, destination folder
  if [ -f "$1" ]; then cp -u "$1" "$2/"; else echo "  (not there: ${1#$ROOT/})"; fi
}
for name in core-oj core-libart conscrypt okhttp bouncycastle apache-xml; do put "$A9/boot/$name.dex" "$OUT/framework"; done
# Android's legacy Apache HTTP client, for the apps that get it on their class path (drivers/mg_java.c, add_shared_libraries).
put "$A9/boot/org.apache.http.legacy.dex" "$OUT/framework"
bash "$ROOT/tools/build-libcore-overlay.sh"
put "$ROOT/build/libcore-overlay/megler-libcore.dex" "$OUT/framework"
for name in core-oj core-libart; do put "$A9/system/framework/$name.jar" "$OUT/framework"; done
RES_SRC="$A9/system/framework/framework-res.apk"
RES_SAID="no platform resources"
if [ "$RES" = cut ]; then
  AAPT2="${MEGLER_ANDROID_SDK:-$USERPROFILE/Documents}/build-tools/29.0.3/aapt2.exe"
  LANGS="${MEGLER_RES_LANGUAGES:-en,ru}"
  DENS="${MEGLER_RES_DENSITIES:-xhdpi,xxhdpi}"
  CUT="$A9/res-cut/$LANGS-$DENS/framework-res.apk"
  if [ ! -f "$AAPT2" ]; then
    echo "  (no aapt2 at $AAPT2: the platform's resources are taken whole)"
    RES=full
  elif [ -f "$RES_SRC" ]; then
    if [ ! -f "$CUT" ] || [ "$RES_SRC" -nt "$CUT" ]; then
      mkdir -p "$(dirname "$CUT")"
      "$AAPT2" optimize -o "$(cygpath -m "$CUT")" -c "$LANGS" --target-densities "$DENS" "$(cygpath -m "$RES_SRC")" \
        || { echo "aapt2 could not cut the platform's resources down" >&2; exit 1; }
    fi
    cmp -s "$CUT" "$OUT/framework/framework-res.apk" || cp "$CUT" "$OUT/framework/framework-res.apk"
    RES_SAID="the platform's resources for $LANGS at $DENS"
  else
    echo "  (not there: ${RES_SRC#$ROOT/})"
  fi
fi
if [ "$RES" = full ]; then
  if [ -f "$RES_SRC" ]; then
    cmp -s "$RES_SRC" "$OUT/framework/framework-res.apk" || cp "$RES_SRC" "$OUT/framework/framework-res.apk"
    RES_SAID="the platform's resources, whole"
  else
    echo "  (not there: ${RES_SRC#$ROOT/})"
  fi
elif [ "$RES" = none ]; then
  rm -f "$OUT/framework/framework-res.apk"
fi
put "$A9/system/usr/icu/icudt60l.dat" "$OUT/usr/icu"
for f in "$A9/system/usr/share/zoneinfo/"*; do put "$f" "$OUT/usr/share/zoneinfo"; done
LIBS=0
for f in "$A9/lib/"*.so; do
  [ -f "$f" ] || continue
  cp -u "$f" "$OUT/lib/"
  LIBS=$((LIBS + 1))
done
[ "$LIBS" -gt 0 ] || echo "  (no native libraries in build/android9/lib: libcore will be its bytecode alone)"
CERTS=0
for f in "$A9/system/etc/security/cacerts/"*; do
  [ -f "$f" ] || continue
  mkdir -p "$OUT/etc/security/cacerts"
  cp -u "$f" "$OUT/etc/security/cacerts/"
  CERTS=$((CERTS + 1))
done
[ "$CERTS" -gt 0 ] || echo "  (no certificates in build/android9/system/etc/security/cacerts: tools/android9-unpack.sh takes them out of the image)"
FONTS=0
for f in "$A9/system/fonts/"*.ttf; do
  [ -f "$f" ] || continue
  mkdir -p "$OUT/fonts"
  cp -u "$f" "$OUT/fonts/"
  FONTS=$((FONTS + 1))
done
[ "$FONTS" -gt 0 ] || echo "  (no fonts in build/android9/system/fonts: tools/android9-unpack.sh fonts takes them out of the image)"
# The list of the families and their files, which native code reads as Android's own is read: Chromium's Skia (the WebView)
# has no fonts at all without it, and ends on its first text.
if [ -f "$A9/system/etc/fonts.xml" ]; then
  mkdir -p "$OUT/etc"
  cp -u "$A9/system/etc/fonts.xml" "$OUT/etc/"
fi
size() { du -sk "$1" | cut -f1; }
# Android's System WebView, where tools/webview-fetch.sh has put it, and the name of its platform support library.
WEBVIEW="no WebView"
if [ -f "$ROOT/build/webview/webview.apk" ]; then
  mkdir -p "$OUT/app/webview"
  cmp -s "$ROOT/build/webview/webview.apk" "$OUT/app/webview/webview.apk" || cp "$ROOT/build/webview/webview.apk" "$OUT/app/webview/webview.apk"
  echo "The platform support of the WebView: its natives are answered by the core (core/src/drivers/mg_java_webview.c)." > "$OUT/lib/libwebviewchromium_plat_support.so"
  WEBVIEW="the WebView $(size "$OUT/app/webview") KB"
else
  echo "  (not there: build/webview/webview.apk: tools/webview-fetch.sh fetches Android's System WebView)"
  rm -rf "$OUT/app/webview" "$OUT/lib/libwebviewchromium_plat_support.so"
fi
# The system's libraries whose functions the core answers itself, each named in lib by a file that says so, as Android has
# them as files in /system/lib: Facebook's SoLoader looks for an app library's system dependencies there before it loads
# it, and Instagram stopped at "couldn't find DSO to load: libz.so". The names are the core's own list (mg_bionic.c,
# mg_bionic_is_system_lib); a library of that name is loaded by its name, whatever its file holds. A file already there
# (libcore's, the WebView's support above) is left as it is.
SYSLIBS=0
for name in $(sed -n '/^bool mg_bionic_is_system_lib/,/^}/p' "$ROOT/core/src/bionic/mg_bionic.c" | grep -o '"lib[^"]*\.so"' | tr -d '"'); do
  [ "$name" = libwebviewchromium_plat_support.so ] && continue
  [ -f "$OUT/lib/$name" ] || echo "A library of the system's whose functions the core answers itself (core/src/bionic/mg_bionic.c, mg_bionic_is_system_lib): it is loaded by its name." > "$OUT/lib/$name"
  SYSLIBS=$((SYSLIBS + 1))
done
[ "$SYSLIBS" -gt 0 ] || { echo "no system libraries named in core/src/bionic/mg_bionic.c (mg_bionic_is_system_lib)" >&2; exit 1; }
# The Java framework an app with an Activity runs on, where it has been built (tools/build-framework.sh).
FW="no Java framework"
if [ -f "$ROOT/build/framework/megler-framework.dex" ]; then
  cp -u "$ROOT/build/framework/megler-framework.dex" "$OUT/framework/"
  FW="the Java framework"
fi
echo "${OUT#$ROOT/}: $(size "$OUT") KB in all: framework $(size "$OUT/framework") KB ($FW, $RES_SAID), lib $(size "$OUT/lib") KB ($LIBS libraries and $SYSLIBS of the system's named), $CERTS certificates, $FONTS fonts, usr $(size "$OUT/usr") KB, $WEBVIEW"
