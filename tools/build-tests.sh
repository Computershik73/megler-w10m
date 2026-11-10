#!/usr/bin/env bash
# Builds and runs the core's tests.
#
#   tools/build-tests.sh arm [--no-run] [--no-libcore] [--no-java] [-- extra arguments for mg_tests]
#       The 32-bit ARM build, in WSL with arm-linux-gnueabihf-gcc, run under
#       qemu-arm: the one that executes guest code. Thumb-2 and hard-float,
#       like the phone's build. Needs the guest libraries (tools/build-guest.sh).
#   tools/build-tests.sh linux64 [--no-run] [--no-libcore] [-- extra arguments for mg_tests]
#       The x86_64 build, in WSL with its own gcc, run there natively: Linux's
#       x86_64 ABI is Android's (System V, LP64), so this build runs the guest
#       libraries of x86_64 (tools/build-guest.sh x86_64, build/guest/x86_64)
#       as they are, as desktop Windows' core does (docs/DESKTOP.md) -- the
#       guest linker's ELF64 and RELA, the x86 patcher, System V's native calls
#       -- with gdb at hand. Run as the arm build is: the tests, again on
#       Android's libcore (build/android9/root-x86_64, with x86_64's natives),
#       the bundles; not the Java test apps, whose native libraries are
#       armeabi-v7a's. Objects go to ~/megler-build-x64 (MEGLER_WSL_BUILD's
#       folder with -x64 after its name).
#   tools/build-tests.sh arm64 [--no-run] [--no-libcore] [-- extra arguments for mg_tests]
#       ARM64's, with no ARM64 machine: the same core and tests compiled by
#       clang for Android's own aarch64 ABI against the NDK's bionic
#       (MEGLER_NDK, r21e: the static libc.a of API 29 in its sysroot), linked
#       into one static program (build/tests-arm64/mg_tests) and run in WSL
#       under qemu-aarch64 with the POSIX host, on the arm64-v8a guest test
#       libraries (tools/build-guest.sh arm64-v8a): ELF64, the patcher's
#       thread-pointer reads (through the island, bionic's own TPIDR_EL0
#       being the host's here), svc traps, AAPCS64's native calls and faults;
#       then again on Android's libcore with arm64-v8a's natives
#       (build/android9/root-arm64-v8a). Not the bundles, whose tests choose
#       32-bit ARM's splits, nor the Java test apps. The program is copied to
#       ~/megler-build-arm64 (MEGLER_WSL_BUILD's folder with -arm64 after its
#       name). Needs qemu-aarch64 in WSL (qemu-user).
#   tools/build-tests.sh win
#       The x64 build with MSVC, run here: the Win32 host layer and everything
#       that needs no ARM code. And the shell's drawing for a Java app
#       (app/Megler/player/GfxHost.cpp: Direct2D, DirectWrite, WIC), which
#       is built into this test program and run as its --gfx-windows mode:
#       scenes drawn through megler_gfx_host, written to build/gfx-pictures as
#       PNG files (sheet.png has all of them) and checked, twice -- the second
#       time with every canvas copied to a WIC bitmap and back -- and a third
#       time with Android's own fonts where build/android9 has them. Each run ends
#       with its own line of checks and failures. The ARM build has no
#       Direct2D and says so. And where `tools/build-tests.sh arm` has left
#       its records of the Java test apps' drawing (build/gfx-records), each
#       is drawn again here, by the stand-in and by Direct2D:
#       build/gfx-pictures/app-hello-N.png and app-demos-N.png.
#       And the shell's storage host (app/Megler/player/StorageHost.cpp:
#       Windows.Storage), built in likewise and run as --storage-windows: the
#       tests of the phone's folders in an app's storage once more, with
#       Windows.Storage's own objects handing the handles over where the
#       stand-in did, on folders of build/tests-win/scratch-storage. A PC's
#       test program is no app container: what the broker refuses an app is
#       not found out here.
#       And the shell's decoders for a Java app's MediaCodec
#       (app/Megler/player/CodecHost.cpp: Media Foundation's decoder
#       transforms), built in likewise and run as --codec-windows: the core's
#       codec over this PC's own decoders, fed the data app's streams
#       (core/tests/app/data/assets) and checked against what they hold,
#       and timed on 720p and 1080p streams it makes into build/media-speed
#       (tools/media-test-streams.py --speed, where Python and ffmpeg are).
#       And the shell's cameras for a Java app's android.hardware.Camera
#       (app/Megler/player/CameraHost.cpp: Windows.Media.Capture), built in
#       likewise and run as --camera-windows: the PC's first camera opened
#       through the core's camera code and a few pictures taken, or, on a PC
#       without one, a line that says so.
#       And the shell's sensors for an app's SensorManager and ASensorManager
#       (app/Megler/player/SensorHost.cpp: Windows.Devices.Sensors), built in
#       likewise and run as --sensors-windows: the PC's sensors listed through
#       the core's and each read a moment, or, on a PC without them, a line
#       that says so.
#       And the shell's window (app/Megler/player/WindowHost.cpp), built in
#       likewise and run as --window-windows: made with no panel (a test
#       program has no XAML), its swap chains presented to and never shown,
#       on this PC's graphics processor -- each way an app's picture comes
#       counted as it comes, the first kept and said in the log with what its
#       Present said, the player told of it at once (what the player's busy
#       overlay goes by), and a picture whose Present is refused.
#       And the shell's Bluetooth for an app's android.bluetooth
#       (app/Megler/player/BluetoothHost.cpp: Windows.Devices.Bluetooth),
#       built in likewise and run as --bluetooth-windows: the PC's radio
#       and a passive scan of three seconds -- nothing connected to or
#       paired with -- or, on a PC without a radio, a line that says so.
#       And the shell's Wi-Fi for an app's WifiManager (app/Megler/player/
#       WifiHost.cpp: Windows.Devices.WiFi), built in likewise and run as
#       --wifi-windows: the PC's adapter, one scan and its networks, the
#       network it is connected to -- or, on a PC without Wi-Fi, a line that
#       says so.
#       And the Xbox build's client of the console's Device Portal
#       (app/Megler/shell/DevicePortal.cpp), built in likewise and run as
#       --portal-windows against tools/device-portal-mock.py, a portal of the
#       tests' own that answers as an Xbox's does (Basic authentication, the
#       CSRF token, an install as a form of files, its state, the list of the
#       packages, an uninstall), over TLS with a certificate made here by
#       openssl for another name (plain HTTP where there is no openssl), where
#       Python is.
#       And, in a program of its own (mg_shell_tests.exe, from
#       core/tests/mg_shell_windows.cpp), the shell's start and its copies:
#       shell/Files.cpp's copies past the cache and their mending, the log's
#       last words written out at once (ShellLog.cpp), the start's steps and
#       watch (Startup.cpp), the plan and the making of the copies of the
#       package's Google APKs after the window is shown (BundledApks.cpp), and
#       the package volume every package Megler installs goes to, the one
#       Megler itself is on (Placement.cpp: a memory card's or the phone's),
#       and which folders of the apps' APKs in the shared folder an installed
#       package reads and which go (SharedApks.cpp).
#
# Both builds unpack bundles of split APKs as the shell does (mg_tests
# --bundles, core/tests/mg_bundle_tests.c): the test app `splits` as an .apks
# and an .xapk (build/guest/java, made by tools/build-test-app.sh), and the
# real bundles that are in build/bundles, which are not in the repository
# (BiP, MangaDex and Magnit: copied there from where they were downloaded;
# those missing are said and left out).
#
# The tests are run twice where Android 9's boot class path has been unpacked
# (build/android9/boot): once with the Dalvik runtime on its built-in
# java.lang, and once more, the whole of them, with the runtime on Android's
# own libcore from the folder tools/dvm-android-root.sh puts together -- under
# qemu-arm with libcore's native libraries, on the PC without. A process has
# one runtime, so these are two processes; each ends with its own line of
# checks and failures, and the script fails if either does. --no-libcore
# leaves the second run out.
#
# And, under qemu-arm, each of the Java test apps (core/tests/app) is started
# through megler_run on that same folder with the Java framework in it, a process
# for each: `hello` and `demos`, where a JDK and the Android SDK's build tools
# are there to build the framework and the apps (tools/build-framework.sh,
# tools/build-test-app.sh; said and left out where they are not), and
# `natives`, the one with a native library of its own, where there is an NDK
# as well to build that (said and left out where there is none), and `crash`,
# which ends on an exception nothing catches, and `splits`, an app in several
# APKs, twice: given as its .apks and as its .xapk, each unpacked as the shell
# unpacks it and started from its base and splits; and `gl`, OpenGL ES from Java (a
# GLSurfaceView, EGL14, EGL10) on a stand-in that draws -- twice: with a test
# shell that composes the window from the frames the core hands it, and with one
# that refuses them, the core composing. And `data`, a database through
# the framework's android.database.sqlite, twice, the second run on what the
# first left, where the core has SQLite in it (build/sqlite). And `webview`,
# Android's System WebView on pages of the app's own server, where the
# Android root carries the WebView (tools/webview-fetch.sh). And `camera`, the
# old android.hardware.Camera on a stand-in for the shell's cameras, twice: the
# second time with every camera refused. And `screen`, whose surface is resized
# and turned from the host's side (megler_set_surface), as a PC's window or a
# phone turned does it: its activities made again or told, the pictures at the
# new size. And `gms-host`, which hosts a stand-in for Google's own Play
# services (`gms-own`) and starts a service of theirs: their code is told it
# runs in its application's process; twice, the user allowing and then
# refusing their own use of the location, which their package context's
# attribution tag follows.
# And `bluetooth`, Bluetooth LE's central side as MeshCore's app uses it, on a
# stand-in for the shell's radio (core/tests/mg_bluetooth_standin.c). And
# `wifi`, WifiManager's scans as Google Play services' network location uses
# them, on a stand-in for the shell's Wi-Fi (core/tests/mg_wifi_standin.c). Their
# pictures are written to build/java-pictures as BMP files. Each is then run
# once more with its calls of the shell's drawing recorded
# (build/gfx-records/<app>.rec), for the Windows build to draw again.
# --no-java leaves these out.
#
# Objects go to ~/megler-build inside WSL (the Windows drive is slow from
# there) and to build/tests-win here. MEGLER_WSL_BUILD names another folder
# in WSL, as WSL's own shell reads it (quoted here, so that this shell leaves
# the ~ alone: MEGLER_WSL_BUILD='~/megler-build-two'): two checkouts
# building at once must not share one.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WB="${MEGLER_WSL_BUILD:-}"
[ -n "$WB" ] || WB='~/megler-build'
MODE="${1:-arm}"
shift || true
RUN=1
LIBCORE=1
JAVA=1
EXTRA=()
while [ $# -gt 0 ]; do
  case "$1" in
    --no-run) RUN=0 ;;
    --no-libcore) LIBCORE=0 ;;
    --no-java) JAVA=0 ;;
    --) shift; EXTRA=("$@"); break ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done
# MG_TESTS_SKIP (core/tests/mg_tests.c: guest functions left uncalled) reaches what WSL runs too.
if [ -n "${MG_TESTS_SKIP:-}" ]; then export WSLENV="${WSLENV:+$WSLENV:}MG_TESTS_SKIP"; fi

# The Dalvik runtime's generated sources and its tests' DEX files (build/guest/armeabi-v7a/dvm).
bash "$ROOT/tools/build-dvm-fixtures.sh"
[ -f "$ROOT/build/android9/boot/core-oj.dex" ] || LIBCORE=0
[ "$LIBCORE" = 1 ] || JAVA=0
if [ "$JAVA" = 1 ] && [ "$MODE" = arm ] && [ "$RUN" = 1 ]; then
  # The framework and the test apps: built only where the tools to build them are, and only when their sources are newer.
  if ! { bash "$ROOT/tools/build-framework.sh" && bash "$ROOT/tools/build-test-app.sh"; }; then
    echo "(the Java framework or the test apps could not be built here: the Java apps are not run)"
    JAVA=0
  fi
fi
STATUS=0
# SQLite is compiled into the core where its amalgamation is (build/sqlite/sqlite3.c, tools/sqlite-fetch.sh): core/src/sqlite/mg_sqlite3.c
# includes it. Without it the core builds as before and says, when an app asks for a database, that it has none.
SQLITE_GCC=""
SQLITE_CL=()
if [ -f "$ROOT/build/sqlite/sqlite3.c" ] && [ -f "$ROOT/build/sqlite/sqlite3.h" ]; then
  SQLITE_GCC="-DMG_HAVE_SQLITE=1 -Ibuild/sqlite"
  SQLITE_CL=(/DMG_HAVE_SQLITE=1 "/I$(cygpath -w "$ROOT/build/sqlite")")
else
  echo "(no build/sqlite/sqlite3.c: the core is built without SQLite, and the tests that need it are left out)"
fi
# libwebp's decoder likewise, where its sources are (build/webp/src, tools/webp-fetch.sh): core/src/webp/mg_webp_*.c each compile one
# of its files. Without it a WebP picture goes to the shell's decoder like any other, and the tests of the core's own are left out.
WEBP_GCC=""
WEBP_CL=()
if [ -f "$ROOT/build/webp/src/dec/webp_dec.c" ] && [ -f "$ROOT/build/webp/src/webp/decode.h" ]; then
  WEBP_GCC="-DMG_HAVE_WEBP=1 -Ibuild/webp"
  WEBP_CL=(/DMG_HAVE_WEBP=1 "/I$(cygpath -w "$ROOT/build/webp")")
else
  echo "(no build/webp/src: the core is built without its WebP decoder, and the tests that need it are left out)"
fi
AGAIN="the tests again, the Dalvik runtime on Android's libcore (build/android9/root)"
mapfile -t SRCS < <(cd "$ROOT" && find core/src core/tests -maxdepth 3 -name '*.c' ! -path 'core/tests/guest/*' | sort)
mapfile -t ZSRCS < <(cd "$ROOT" && find core/third_party/zlib -maxdepth 1 -name '*.c' ! -name 'gz*.c' | sort)

if [ "$MODE" = arm ]; then
  # The guest libraries again when any of their sources is newer than the first of them build-guest.sh makes.
  { [ -f "$ROOT/build/guest/armeabi-v7a/libt_basic.so" ] \
    && [ -z "$(find "$ROOT/core/tests/guest" -type f -newer "$ROOT/build/guest/armeabi-v7a/libt_basic.so" -print -quit)" ]; } || bash "$ROOT/tools/build-guest.sh" > /dev/null
  WROOT="/mnt/${ROOT:1:1}${ROOT:2}"
  cat > "$ROOT/build/tests-arm.sh" <<EOF
set -e
OUT=$WB
mkdir -p \$OUT/obj
cd "$WROOT"
CFLAGS="-std=gnu11 -O1 -g -mthumb -march=armv7-a -mfpu=neon -mfloat-abi=hard -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation -Icore/third_party/zlib -D_GNU_SOURCE $SQLITE_GCC $WEBP_GCC"
compile() {
  src="\$1"; obj="\$OUT/obj/\$(echo "\$src" | tr '/' '_').o"
  if [ ! -f "\$obj" ] || [ "\$src" -nt "\$obj" ] || [ -n "\$(find core/include core/src core/tests \( -name '*.h' -o -name '*.inc' \) -newer "\$obj" -print -quit)" ] \\
     || { [ "\$src" = core/src/sqlite/mg_sqlite3.c ] && [ -f build/sqlite/sqlite3.c ] && [ build/sqlite/sqlite3.c -nt "\$obj" ]; }; then
    arm-linux-gnueabihf-gcc \$CFLAGS \$2 -c "\$src" -o "\$obj" || exit 1
  fi
}
# (With SQLite or without, with the WebP decoder or without, is a matter of flags, which no object file remembers: when they change, everything is compiled again.)
[ "\$(cat \$OUT/sqlite.flags 2>/dev/null)" = "$SQLITE_GCC $WEBP_GCC" ] || rm -f \$OUT/obj/*.o
echo "$SQLITE_GCC $WEBP_GCC" > \$OUT/sqlite.flags
export -f compile
export OUT CFLAGS
printf '%s\n' ${SRCS[*]} | xargs -P 8 -I{} bash -c 'compile {} ""'
printf '%s\n' ${ZSRCS[*]} | xargs -P 8 -I{} bash -c 'compile {} "-w"'
arm-linux-gnueabihf-gcc -mthumb -o \$OUT/mg_tests \$OUT/obj/*.o -lm -lpthread -ldl
echo "built \$OUT/mg_tests"
EOF
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash "$WROOT/build/tests-arm.sh"
  if [ "$RUN" = 1 ]; then
    # (The layer's log is the program's standard error; the two are joined inside WSL, where lines stay whole.)
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "rm -rf $WB/scratch && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests '$WROOT/build/guest/armeabi-v7a' $WB/scratch \$([ -f $WB/mg.apk ] && echo --dvm-apk $WB/mg.apk) ${EXTRA[*]:-} 2>&1" || STATUS=1
    if [ "$LIBCORE" = 1 ]; then
      bash "$ROOT/tools/dvm-android-root.sh"
      echo "$AGAIN"
      # The folder is copied into WSL's own disk: the Windows drive is slow from there, and ICU maps 24 MB of it.
      MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "mkdir -p $WB/android-root && cp -ru '$WROOT/build/android9/root/.' $WB/android-root/ && rm -rf $WB/scratch && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests '$WROOT/build/guest/armeabi-v7a' $WB/scratch --dvm-android $WB/android-root \$([ -f $WB/mg.apk ] && echo --dvm-apk $WB/mg.apk) ${EXTRA[*]:-} 2>&1" || STATUS=1
    fi
    # Bundles of split APKs, unpacked as the shell unpacks them.
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "rm -rf $WB/scratch-bundles && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --bundles '$WROOT/build/guest/java' $WB/scratch-bundles '$WROOT/build/bundles' 2>&1; STATUS=\$?; rm -rf $WB/scratch-bundles; exit \$STATUS" || STATUS=1
    if [ "$JAVA" = 1 ]; then
      for app in hello demos natives crash gc gl screen bluetooth wifi; do
        if [ ! -f "$ROOT/build/guest/java/$app.apk" ]; then
          echo "(the Java test app $app is not run: build/guest/java/$app.apk was not built -- its native library needs the NDK, see tools/build-test-app.sh)"
          continue
        fi
        echo "the Java test app $app (build/guest/java/$app.apk), through megler_run"
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp -u '$WROOT/build/guest/java/$app.apk' $WB/$app.apk && rm -rf $WB/java-data-$app && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --java $app $WB/$app.apk $WB/java-data-$app $WB/android-root '$WROOT/build/java-pictures' 2>&1" || STATUS=1
      done
      # Optional real microG integration, including a conflicting client GMS class.
      # Fetch the pinned upstream APK with tools/microg-fetch.sh first.
      if [ -f "$ROOT/build/microg/GmsCore.apk" ] && [ -f "$ROOT/build/guest/java/microg.apk" ]; then
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp '$WROOT/build/guest/java/microg.apk' $WB/microg.apk && rm -rf $WB/java-data-microg && cp '$WROOT/build/microg/GmsCore.apk' $WB/GmsCore.apk && MG_JAVA_GMS_APK=$WB/GmsCore.apk qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --java microg $WB/microg.apk $WB/java-data-microg $WB/android-root - 2>&1" || STATUS=1
      fi
      # Google's own Play services as an app hosts them, in a stand-in of theirs (gms-own: no microG in it): their code is told
      # it runs in its application's process, its main one; and their own location requests get an attribution tag once the
      # user allows them it, and none where the user refuses it (`deny`), the user asked once (mg_java_tests.c, run_gms_host).
      if [ -f "$ROOT/build/guest/java/gms-own.apk" ] && [ -f "$ROOT/build/guest/java/gms-host.apk" ]; then
        for answer in allow deny; do
          echo "the Java test app gms-host (build/guest/java/gms-host.apk), hosting the stand-in for Google's own Play services (gms-own.apk): their own location, the user's answer $answer"
          MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp -u '$WROOT/build/guest/java/gms-host.apk' $WB/gms-host.apk && cp -u '$WROOT/build/guest/java/gms-own.apk' $WB/gms-own.apk && rm -rf $WB/java-data-gms-host && MG_JAVA_GMS_APK=$WB/gms-own.apk qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --java gms-host $WB/gms-host.apk $WB/java-data-gms-host $WB/android-root - 0 480x800@160 $answer 2>&1" || STATUS=1
        done
      fi
      # The app in several APKs, from each of its bundles: unpacked as the shell unpacks one, for a screen of 320 dpi, and run.
      for bundle in apks xapk; do
        if [ ! -f "$ROOT/build/guest/java/splits.$bundle" ]; then
          echo "(the Java test app splits is not run from its .$bundle: build/guest/java/splits.$bundle was not built -- its native library needs the NDK)"
          continue
        fi
        echo "the Java test app splits (build/guest/java/splits.$bundle), unpacked and run through megler_run"
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp -u '$WROOT/build/guest/java/splits.$bundle' $WB/splits.$bundle && rm -rf $WB/java-data-splits && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --java splits $WB/splits.$bundle $WB/java-data-splits $WB/android-root '$WROOT/build/java-pictures' 0 720x1280@320 2>&1" || STATUS=1
      done
      # The database app, twice: the second run on the data folder the first left, to find what it wrote. Only where the core has SQLite in it.
      if [ -n "$SQLITE_GCC" ] && [ -f "$ROOT/build/guest/java/data.apk" ]; then
        for run in 1 2; do
          echo "the Java test app data (build/guest/java/data.apk), through megler_run: run $run on its database"
          CLEAN=""
          [ "$run" = 1 ] && CLEAN="rm -rf $WB/java-data-data &&"
          # The second run with the flags trace-net, trace-exceptions, trace-stacks, trace-cpu, trace-native-stacks, trace-profile and a trace-calls and a trace-calls-within- on, which mg_java_tests.c finds the lines of.
          [ "$run" = 2 ] && CLEAN="mkdir -p $WB/java-data-data/flags && touch $WB/java-data-data/flags/trace-net $WB/java-data-data/flags/trace-exceptions $WB/java-data-data/flags/trace-stacks $WB/java-data-data/flags/trace-cpu $WB/java-data-data/flags/trace-native-stacks $WB/java-data-data/flags/trace-profile $WB/java-data-data/flags/trace-calls-android.media.session.PlaybackState $WB/java-data-data/flags/trace-calls-within-android.media.session.PlaybackState &&"
          MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp -u '$WROOT/build/guest/java/data.apk' $WB/data.apk && $CLEAN qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --java data $WB/data.apk $WB/java-data-data $WB/android-root '$WROOT/build/java-pictures' 0 480x800@160 run:$run 2>&1" || STATUS=1
        done
      else
        echo "(the Java test app data is not run: the core was built without SQLite)"
      fi
      # The WebView app: Android's System WebView, Chromium, from the Android root (tools/webview-fetch.sh brings it), on pages
      # of the app's own server on loopback. Only where the root carries it.
      if [ -f "$ROOT/build/guest/java/webview.apk" ] && [ -f "$ROOT/build/android9/root/app/webview/webview.apk" ]; then
        echo "the Java test app webview (build/guest/java/webview.apk), through megler_run: Android's System WebView from the Android root"
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp -u '$WROOT/build/guest/java/webview.apk' $WB/webview.apk && rm -rf $WB/java-data-webview && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --java webview $WB/webview.apk $WB/java-data-webview $WB/android-root '$WROOT/build/java-pictures' 0 480x800@160 2>&1" || STATUS=1
      else
        echo "(the Java test app webview is not run: no WebView in the Android root -- tools/webview-fetch.sh fetches it)"
      fi
      # The camera app: the old android.hardware.Camera on a stand-in for the shell's cameras (core/tests/mg_camera_standin.c),
      # its previews' pictures checked in the window; and once more with every camera refused, as Windows refuses Megler the
      # camera when the user has said no.
      if [ -f "$ROOT/build/guest/java/camera.apk" ]; then
        for run in 1 2; do
          SCRIPT=""
          [ "$run" = 2 ] && SCRIPT="denied"
          echo "the Java test app camera (build/guest/java/camera.apk), through megler_run${SCRIPT:+, every camera refused}"
          MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp -u '$WROOT/build/guest/java/camera.apk' $WB/camera.apk && rm -rf $WB/java-data-camera && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --java camera $WB/camera.apk $WB/java-data-camera $WB/android-root '$WROOT/build/java-pictures' 0 480x800@160 $SCRIPT 2>&1" || STATUS=1
        done
      fi
      # The GL app again with a shell that does not compose the window (gl_frame says no): the core reads the frames back and
      # composes the window itself, the way taken where EGL shares no textures; the same checks of the picture.
      if [ -f "$ROOT/build/guest/java/gl.apk" ]; then
        echo "the Java test app gl again, the window composed by the core (the shell takes no frames)"
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "rm -rf $WB/java-data-gl && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --java gl $WB/gl.apk $WB/java-data-gl $WB/android-root '$WROOT/build/java-pictures-readback' 0 480x800@160 readback 2>&1" || STATUS=1
      fi
      # And natives likewise: the EGL surfaces its native code makes on a SurfaceView, in a context of its own, read back.
      if [ -f "$ROOT/build/guest/java/natives.apk" ]; then
        echo "the Java test app natives again, the window composed by the core (the shell takes no frames)"
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "rm -rf $WB/java-data-natives && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --java natives $WB/natives.apk $WB/java-data-natives $WB/android-root '$WROOT/build/java-pictures-readback' 0 480x800@160 readback 2>&1" || STATUS=1
      fi
      # And an upright surface, as a phone held upright gives an app that does not ask for landscape: the same app, the same checks.
      echo "the Java test app hello again, on an upright surface (720 x 1280 at 320 dpi)"
      MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp -u '$WROOT/build/guest/java/hello.apk' $WB/hello.apk && rm -rf $WB/java-data-hello && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --java hello $WB/hello.apk $WB/java-data-hello $WB/android-root '$WROOT/build/java-pictures-upright' 0 720x1280@320 2>&1" || STATUS=1
      # Each of them once more, with every call it makes of the shell's drawing written to build/gfx-records/<app>.rec
      # (core/tests/mg_gfx_record.c): what `tools/build-tests.sh win` draws again with Direct2D and DirectWrite.
      mkdir -p "$ROOT/build/gfx-records"
      for app in hello demos; do
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "rm -rf $WB/java-data-$app && qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --gfx-record $app $WB/$app.apk $WB/java-data-$app $WB/android-root $WB/$app.rec 2>/dev/null && cp $WB/$app.rec '$WROOT/build/gfx-records/$app.rec'" || STATUS=1
      done
    fi
    # The shell's drawing (Direct2D, DirectWrite, WIC) is only in the Windows build: this one has the mode and says so.
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "qemu-arm -L /usr/arm-linux-gnueabihf $WB/mg_tests --gfx-windows - 2>&1" || STATUS=1
  fi
elif [ "$MODE" = linux64 ]; then
  # x86_64's guest libraries again when any of their sources is newer than the first of them build-guest.sh makes.
  { [ -f "$ROOT/build/guest/x86_64/libt_basic.so" ] \
    && [ -z "$(find "$ROOT/core/tests/guest" -type f -newer "$ROOT/build/guest/x86_64/libt_basic.so" -print -quit)" ]; } || bash "$ROOT/tools/build-guest.sh" x86_64 > /dev/null
  WROOT="/mnt/${ROOT:1:1}${ROOT:2}"
  WB="$WB-x64"
  cat > "$ROOT/build/tests-linux64.sh" <<EOF
set -e
OUT=$WB
mkdir -p \$OUT/obj
cd "$WROOT"
CFLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation -Icore/third_party/zlib -D_GNU_SOURCE $SQLITE_GCC $WEBP_GCC"
compile() {
  src="\$1"; obj="\$OUT/obj/\$(echo "\$src" | tr '/' '_').o"
  if [ ! -f "\$obj" ] || [ "\$src" -nt "\$obj" ] || [ -n "\$(find core/include core/src core/tests \( -name '*.h' -o -name '*.inc' \) -newer "\$obj" -print -quit)" ] \\
     || { [ "\$src" = core/src/sqlite/mg_sqlite3.c ] && [ -f build/sqlite/sqlite3.c ] && [ build/sqlite/sqlite3.c -nt "\$obj" ]; }; then
    gcc \$CFLAGS \$2 -c "\$src" -o "\$obj" || exit 1
  fi
}
# (With SQLite or without, with the WebP decoder or without, is a matter of flags, which no object file remembers: when they change, everything is compiled again.)
[ "\$(cat \$OUT/sqlite.flags 2>/dev/null)" = "$SQLITE_GCC $WEBP_GCC" ] || rm -f \$OUT/obj/*.o
echo "$SQLITE_GCC $WEBP_GCC" > \$OUT/sqlite.flags
export -f compile
export OUT CFLAGS
printf '%s\n' ${SRCS[*]} | xargs -P 8 -I{} bash -c 'compile {} ""'
printf '%s\n' ${ZSRCS[*]} | xargs -P 8 -I{} bash -c 'compile {} "-w"'
gcc -o \$OUT/mg_tests \$OUT/obj/*.o -lm -lpthread -ldl
echo "built \$OUT/mg_tests"
EOF
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash "$WROOT/build/tests-linux64.sh"
  if [ "$RUN" = 1 ]; then
    # (The layer's log is the program's standard error; the two are joined inside WSL, where lines stay whole. The Dalvik
    # runtime's DEX fixtures are every ABI's: build-dvm-fixtures.sh keeps them beside armeabi-v7a's libraries.)
    DVM="--dvm '$WROOT/build/guest/armeabi-v7a/dvm'"
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "rm -rf $WB/scratch && $WB/mg_tests '$WROOT/build/guest/x86_64' $WB/scratch $DVM ${EXTRA[*]:-} 2>&1" || STATUS=1
    if [ "$LIBCORE" = 1 ]; then
      bash "$ROOT/tools/dvm-android-root.sh" --abi x86_64
      echo "$AGAIN, with x86_64's natives (build/android9/root-x86_64)"
      # The folder is copied into WSL's own disk: the Windows drive is slow from there, and ICU maps 24 MB of it.
      MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "mkdir -p $WB/android-root && cp -ru '$WROOT/build/android9/root-x86_64/.' $WB/android-root/ && rm -rf $WB/scratch && $WB/mg_tests '$WROOT/build/guest/x86_64' $WB/scratch $DVM --dvm-android $WB/android-root ${EXTRA[*]:-} 2>&1" || STATUS=1
    fi
    # Bundles of split APKs, unpacked as the shell unpacks them.
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "rm -rf $WB/scratch-bundles && $WB/mg_tests --bundles '$WROOT/build/guest/java' $WB/scratch-bundles '$WROOT/build/bundles' 2>&1; STATUS=\$?; rm -rf $WB/scratch-bundles; exit \$STATUS" || STATUS=1
  fi
elif [ "$MODE" = arm64 ]; then
  NDK="${MEGLER_NDK:-/c/Users/User/Documents/android-ndk-r21e}"
  LLVM="${MEGLER_LLVM:-/c/Program Files/LLVM/bin}"
  SYSROOT="$NDK/toolchains/llvm/prebuilt/windows-x86_64/sysroot"
  LIBDIR="$SYSROOT/usr/lib/aarch64-linux-android/29"
  BUILTINS="$(ls "$NDK"/toolchains/llvm/prebuilt/windows-x86_64/lib64/clang/*/lib/linux/libclang_rt.builtins-aarch64-android.a 2>/dev/null | head -1)"
  { [ -f "$LIBDIR/libc.a" ] && [ -f "$BUILTINS" ]; } || { echo "no static bionic or compiler-rt for aarch64 in $NDK (set MEGLER_NDK)" >&2; exit 1; }
  [ -x "$LLVM/clang.exe" ] || { echo "no clang in $LLVM (set MEGLER_LLVM)" >&2; exit 1; }
  { [ -f "$ROOT/build/guest/arm64-v8a/libt_basic.so" ] \
    && [ -z "$(find "$ROOT/core/tests/guest" -type f -newer "$ROOT/build/guest/arm64-v8a/libt_basic.so" -print -quit)" ]; } || bash "$ROOT/tools/build-guest.sh" arm64-v8a > /dev/null
  OUT="$ROOT/build/tests-arm64"
  mkdir -p "$OUT/obj"
  cd "$ROOT"
  # The linux64 build's flags, for Android's ABI; atomics inline (NDK r21e's compiler-rt has no __aarch64_ldadd4_acq_rel
  # and its kin, which clang's outline atomics call).
  FLAGS=(--target=aarch64-linux-android29 "--sysroot=$SYSROOT" -std=gnu11 -O1 -g -mno-outline-atomics -Wall -Wextra -Wno-unused-parameter
         -Wno-sign-compare -Wno-format-truncation -Icore/third_party/zlib -D_GNU_SOURCE $SQLITE_GCC $WEBP_GCC)
  [ "$(cat "$OUT/obj/flags" 2>/dev/null)" = "${FLAGS[*]}" ] || rm -f "$OUT/obj/"*.o
  echo "${FLAGS[*]}" > "$OUT/obj/flags"
  newest_header="$(find core/include core/src core/tests \( -name '*.h' -o -name '*.inc' \) | xargs ls -t | head -1)"
  compile_arm64() {
    local src="$1" extra="$2" obj="$OUT/obj/$(echo "$1" | tr '/' '_').o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ "$newest_header" -nt "$obj" ] \
       || { [ "$src" = core/src/sqlite/mg_sqlite3.c ] && [ -f build/sqlite/sqlite3.c ] && [ build/sqlite/sqlite3.c -nt "$obj" ]; }; then
      "$LLVM/clang.exe" "${FLAGS[@]}" $extra -c "$src" -o "$obj" 2> "$obj.log" || { cat "$obj.log" >&2; rm -f "$obj"; echo "FAILED: $src" >&2; return 1; }
    fi
  }
  for s in "${SRCS[@]}"; do
    compile_arm64 "$s" "" &
    while [ "$(jobs -r | wc -l)" -ge "${MEGLER_JOBS:-12}" ]; do sleep 0.1; done
  done
  for s in "${ZSRCS[@]}"; do
    compile_arm64 "$s" "-w" &
    while [ "$(jobs -r | wc -l)" -ge "${MEGLER_JOBS:-12}" ]; do sleep 0.1; done
  done
  wait
  for s in "${SRCS[@]}" "${ZSRCS[@]}"; do
    [ -f "$OUT/obj/$(echo "$s" | tr '/' '_').o" ] || { echo "not compiled: $s" >&2; exit 1; }
  done
  # One static program: bionic's static libc of API 29 (static bionic's dlopen loads nothing, and nothing here needs it:
  # the guest libraries are the core's own loader's). -nostdlib, as clang's own runtime libraries are not the NDK's.
  "$LLVM/clang.exe" --target=aarch64-linux-android29 "--sysroot=$SYSROOT" -static -nostdlib -fuse-ld=lld -Wl,-z,max-page-size=4096 \
    -o "$OUT/mg_tests" "$LIBDIR/crtbegin_static.o" "$OUT/obj/"*.o \
    -Wl,--start-group "$LIBDIR/libm.a" "$LIBDIR/libdl.a" "$BUILTINS" "$LIBDIR/libc.a" -Wl,--end-group "$LIBDIR/crtend_android.o"
  echo "built $OUT/mg_tests"
  WROOT="/mnt/${ROOT:1:1}${ROOT:2}"
  WB="$WB-arm64"
  if [ "$RUN" = 1 ]; then
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "command -v qemu-aarch64 > /dev/null" \
      || { echo "no qemu-aarch64 in WSL (sudo apt install qemu-user)" >&2; exit 1; }
    DVM="--dvm '$WROOT/build/guest/armeabi-v7a/dvm'"
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "mkdir -p $WB && cp '$WROOT/build/tests-arm64/mg_tests' $WB/mg_tests && rm -rf $WB/scratch && qemu-aarch64 $WB/mg_tests '$WROOT/build/guest/arm64-v8a' $WB/scratch $DVM ${EXTRA[*]:-} 2>&1" || STATUS=1
    if [ "$LIBCORE" = 1 ]; then
      bash "$ROOT/tools/dvm-android-root.sh" --abi arm64-v8a
      echo "$AGAIN, with arm64-v8a's natives (build/android9/root-arm64-v8a)"
      # The folder is copied into WSL's own disk: the Windows drive is slow from there, and ICU maps 24 MB of it.
      MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "mkdir -p $WB/android-root && cp -ru '$WROOT/build/android9/root-arm64-v8a/.' $WB/android-root/ && rm -rf $WB/scratch && qemu-aarch64 $WB/mg_tests '$WROOT/build/guest/arm64-v8a' $WB/scratch $DVM --dvm-android $WB/android-root ${EXTRA[*]:-} 2>&1" || STATUS=1
    fi
  fi
elif [ "$MODE" = win ]; then
  for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe "/c/Program Files (x86)/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe; do
    [ -f "$cl" ] && VC="${cl%/bin/Hostx64/x64/cl.exe}"
  done
  [ -n "${VC:-}" ] || { echo "no MSVC found" >&2; exit 1; }
  SDK="/c/Program Files (x86)/Windows Kits/10"
  SDKV="${MEGLER_SDK_VERSION:-10.0.22621.0}"
  w() { cygpath -w "$1"; }
  export INCLUDE="$(w "$VC/include");$(w "$SDK/Include/$SDKV/ucrt");$(w "$SDK/Include/$SDKV/shared");$(w "$SDK/Include/$SDKV/um")"
  export LIB="$(w "$VC/lib/x64");$(w "$SDK/Lib/$SDKV/ucrt/x64");$(w "$SDK/Lib/$SDKV/um/x64")"
  export PATH="$VC/bin/Hostx64/x64:$PATH"
  export VSLANG=1033 MSYS2_ARG_CONV_EXCL="*"
  # (MEGLER_TESTS_OPT: the C compiler's optimisation, /O1 unless told; tools/dvm-bench.sh builds with /O2, as the app is
  # built, into a folder of its own named by MEGLER_TESTS_OUT.)
  OPT="${MEGLER_TESTS_OPT:-/O1}"
  # (MEGLER_JOBS: the compilers at once, as the other builds take it; a bare /MP is one for every processor, and this PC
  # turns itself off with all sixteen of its busy.)
  MP="/MP${MEGLER_JOBS:-}"
  OUT="${MEGLER_TESTS_OUT:-$ROOT/build/tests-win}"
  mkdir -p "$OUT/obj" "$OUT/zobj"
  # Every file is compiled on every run; the objects of the last go first, so that a file that no longer compiles fails
  # the link instead of leaving its old object to be linked with the others (whose structures may have changed since).
  rm -f "$OUT/obj/"*.obj
  ALL=()
  for s in "${SRCS[@]}"; do ALL+=("$(w "$ROOT/$s")"); done
  ZALL=()
  for s in "${ZSRCS[@]}"; do ZALL+=("$(w "$ROOT/$s")"); done
  # (MG_GFX_WINDOWS, MG_STORAGE_WINDOWS, MG_CODEC_WINDOWS, MG_CAMERA_WINDOWS, MG_SENSORS_WINDOWS, MG_BLUETOOTH_WINDOWS,
  # MG_WIFI_WINDOWS, MG_PORTAL_WINDOWS, MG_WINDOW_WINDOWS: this build has the shell's drawing, its storage host, its
  # decoders, its cameras, its sensors, its Bluetooth, its Wi-Fi, the Xbox build's Device Portal client and the shell's
  # window in it; mg_tests_main.c's --gfx-windows, --storage-windows, --codec-windows, --camera-windows, --sensors-windows,
  # --bluetooth-windows, --wifi-windows, --portal-windows and --window-windows modes.)
  "$VC/bin/Hostx64/x64/cl.exe" /nologo /c "$OPT" /MT /W3 /std:c11 /utf-8 "$MP" /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS /DMG_GFX_WINDOWS /DMG_STORAGE_WINDOWS /DMG_CODEC_WINDOWS /DMG_CAMERA_WINDOWS /DMG_SENSORS_WINDOWS /DMG_BLUETOOTH_WINDOWS /DMG_WIFI_WINDOWS /DMG_PORTAL_WINDOWS /DMG_WINDOW_WINDOWS /DMG_DL_TESTS "${SQLITE_CL[@]}" "${WEBP_CL[@]}" \
    "/I$(w "$ROOT/core/third_party/zlib")" "/Fo$(w "$OUT/obj")\\" "${ALL[@]}" | grep -v -E '^[A-Za-z0-9_]+\.c$' || true
  "$VC/bin/Hostx64/x64/cl.exe" /nologo /c /O1 /MT /w "$MP" /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS \
    "/Fo$(w "$OUT/zobj")\\" "${ZALL[@]}" | grep -v -E '^[A-Za-z0-9_]+\.c$' || true
  # The shell's drawing itself, the very file the phone's package is built from (it is plain COM, no WinRT), and the few
  # lines that hand it to the tests: C++, with the shell's own flags.
  GFX_SRC=("$(w "$ROOT/app/Megler/player/GfxHost.cpp")" "$(w "$ROOT/core/tests/mg_gfx_windows.cpp")")
  GFX_FLAGS=(/nologo /c /MT /W3 /EHsc /GR /permissive- /std:c++20 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
             "/I$(w "$ROOT/app/Megler")" "/I$(w "$ROOT/core/include")")
  mkdir -p "$OUT/gfxobj"
  rm -f "$OUT/gfxobj/"*.obj
  "$VC/bin/Hostx64/x64/cl.exe" "${GFX_FLAGS[@]}" /O2 "$MP" "/Fo$(w "$OUT/gfxobj")\\" "${GFX_SRC[@]}" | grep -v -E '^[A-Za-z0-9_]+\.cpp$' || true
  for obj in GfxHost mg_gfx_windows; do
    [ -f "$OUT/gfxobj/$obj.obj" ] || { echo "compile failed: $obj.cpp" >&2; exit 1; }
  done
  # The same file against the headers of the SDK whose libraries the phone's package links (10.0.15063, Windows 10
  # Mobile's last): an interface that only a later Windows has would compile above and fail on the phone.
  OLD_SDK="${MEGLER_SDK_LIB:-10.0.15063.0}"
  if [ -f "$SDK/Include/$OLD_SDK/um/d2d1_1.h" ]; then
    mkdir -p "$OUT/gfxobj-old"
    rm -f "$OUT/gfxobj-old/GfxHost.obj"
    INCLUDE="$(w "$VC/include");$(w "$SDK/Include/$OLD_SDK/ucrt");$(w "$SDK/Include/$OLD_SDK/shared");$(w "$SDK/Include/$OLD_SDK/um")" \
      "$VC/bin/Hostx64/x64/cl.exe" "${GFX_FLAGS[@]}" /Od "/Fo$(w "$OUT/gfxobj-old")\\" "${GFX_SRC[0]}" | grep -v -E '^[A-Za-z0-9_]+\.cpp$' || true
    [ -f "$OUT/gfxobj-old/GfxHost.obj" ] || { echo "GfxHost.cpp does not compile against the SDK $OLD_SDK headers" >&2; exit 1; }
    echo "GfxHost.cpp also compiles against the SDK $OLD_SDK headers"
  else
    echo "(no SDK $OLD_SDK headers here: GfxHost.cpp was not compiled against them)"
  fi
  # The shell's storage host too (app/Megler/player/StorageHost.cpp: Windows.Storage, through C++/WinRT), the very file
  # the phone's package is built from, with the shell's Strings.cpp and the few lines that hand it to the tests and stand
  # in for the rest of the shell (core/tests/mg_storage_windows.cpp). The shell's precompiled header is read as a plain
  # header here; the projection's headers are the SDK's own. Compiled as for an app container (WINAPI_FAMILY_APP), which
  # is what the phone's package is compiled as: the headers then show what an app may call and nothing else.
  # And the shell's decoders for a Java app's MediaCodec (app/Megler/player/CodecHost.cpp: Media Foundation's decoder
  # transforms), likewise, with the lines that hand them to the tests (core/tests/mg_codec_windows.cpp), and its reader of
  # whole files (player/MediaHost.cpp), for the files of sound alone a Java app's MediaPlayer and SoundPool hand it; the
  # shell's log they write to is mg_storage_windows.cpp's. And the shell's cameras (player/CameraHost.cpp:
  # Windows.Media.Capture), with the lines that hand them to the tests (core/tests/mg_camera_windows.cpp). And the shell's
  # sensors (player/SensorHost.cpp: Windows.Devices.Sensors), likewise (core/tests/mg_sensors_windows.cpp). And its
  # Bluetooth (player/BluetoothHost.cpp: Windows.Devices.Bluetooth), likewise (core/tests/mg_bluetooth_windows.cpp), and its
  # Wi-Fi (player/WifiHost.cpp: Windows.Devices.WiFi), likewise (core/tests/mg_wifi_windows.cpp). And the Xbox
  # build's Device Portal client (shell/DevicePortal.cpp: Windows.Web.Http), with the tests that drive it against the mock
  # portal (core/tests/mg_portal_windows.cpp); no package but the Xbox's has the client (tools/build-appx.sh --xbox).
  STORAGE_SRC=("$(w "$ROOT/app/Megler/player/StorageHost.cpp")" "$(w "$ROOT/app/Megler/shell/Strings.cpp")" "$(w "$ROOT/core/tests/mg_storage_windows.cpp")"
               "$(w "$ROOT/app/Megler/player/CodecHost.cpp")" "$(w "$ROOT/core/tests/mg_codec_windows.cpp")"
               "$(w "$ROOT/app/Megler/player/MediaHost.cpp")"
               "$(w "$ROOT/app/Megler/player/CameraHost.cpp")" "$(w "$ROOT/core/tests/mg_camera_windows.cpp")"
               "$(w "$ROOT/app/Megler/player/SensorHost.cpp")" "$(w "$ROOT/core/tests/mg_sensors_windows.cpp")"
               "$(w "$ROOT/app/Megler/player/BluetoothHost.cpp")" "$(w "$ROOT/core/tests/mg_bluetooth_windows.cpp")"
               "$(w "$ROOT/app/Megler/player/WifiHost.cpp")" "$(w "$ROOT/core/tests/mg_wifi_windows.cpp")"
               "$(w "$ROOT/app/Megler/shell/DevicePortal.cpp")" "$(w "$ROOT/core/tests/mg_portal_windows.cpp")"
               "$(w "$ROOT/app/Megler/player/WindowHost.cpp")" "$(w "$ROOT/core/tests/mg_window_windows.cpp")")
  mkdir -p "$OUT/storageobj"
  rm -f "$OUT/storageobj/"*.obj
  INCLUDE="$INCLUDE;$(w "$SDK/Include/$SDKV/winrt");$(w "$SDK/Include/$SDKV/cppwinrt")" \
    "$VC/bin/Hostx64/x64/cl.exe" "${GFX_FLAGS[@]}" /Zc:__cplusplus /bigobj /DWINAPI_FAMILY=WINAPI_FAMILY_APP /O1 "$MP" "/Fo$(w "$OUT/storageobj")\\" "${STORAGE_SRC[@]}" \
    | grep -v -E '^[A-Za-z0-9_]+\.cpp$' || true
  for obj in StorageHost Strings mg_storage_windows CodecHost mg_codec_windows MediaHost CameraHost mg_camera_windows SensorHost mg_sensors_windows BluetoothHost mg_bluetooth_windows WifiHost mg_wifi_windows DevicePortal mg_portal_windows WindowHost mg_window_windows; do
    [ -f "$OUT/storageobj/$obj.obj" ] || { echo "compile failed: $obj.cpp" >&2; exit 1; }
  done
  # (MEGLER_TESTS_MAP=1: a linker map beside it, mg_tests.map, which tools/dvm-bench.sh profile reads.)
  MAPOPT=()
  [ "${MEGLER_TESTS_MAP:-0}" = 1 ] && MAPOPT=("/MAP:$(w "$OUT/mg_tests.map")")
  # (MEGLER_TESTS_ORDER=<file>: the functions it names first in the program's code, in that order (link /ORDER). The
  # interpreter's speed moves by half when unrelated code before it shifts its loop's alignment; tools/dvm-bench.sh puts
  # `mg_dvm_interpret` first, so that what it measures is the change and not where the loop fell.)
  [ -n "${MEGLER_TESTS_ORDER:-}" ] && MAPOPT+=("/ORDER:@$(w "$MEGLER_TESTS_ORDER")")
  "$VC/bin/Hostx64/x64/link.exe" /nologo ${MAPOPT[@]+"${MAPOPT[@]}"} "/OUT:$(w "$OUT/mg_tests.exe")" "$(w "$OUT/obj")\\*.obj" "$(w "$OUT/zobj")\\*.obj" "$(w "$OUT/gfxobj")\\*.obj" \
    "$(w "$OUT/storageobj")\\*.obj" bcrypt.lib synchronization.lib d2d1.lib d3d11.lib dwrite.lib ole32.lib oleaut32.lib ws2_32.lib iphlpapi.lib mfplat.lib mfreadwrite.lib mfuuid.lib
  echo "built $OUT/mg_tests.exe"
  # Two DLLs beside mg_tests.exe that do not load, for its test of what mg_dl_open says then (MG_DL_TESTS,
  # test_dl_open_why): mg-dl-t1.dll imports a function of mg-dl-nowhere.dll, which is nowhere, and mg-dl-t2.dll one that
  # kernel32.dll has not got. Each imports that function alone (no C runtime, no entry point), through an import library
  # made from a .def of its own.
  DLT="$OUT/dltest"
  mkdir -p "$DLT"
  rm -f "$OUT/mg-dl-t1.dll" "$OUT/mg-dl-t2.dll"
  printf 'LIBRARY mg-dl-nowhere.dll\nEXPORTS\n  MeglerNowhere\n' > "$DLT/nowhere.def"
  printf 'LIBRARY KERNEL32.dll\nEXPORTS\n  MeglerNoSuchFunction\n' > "$DLT/k32.def"
  printf '__declspec(dllimport) int MeglerNowhere(void);\n__declspec(dllexport) int mg_dl_t1(void) { return MeglerNowhere(); }\n' > "$DLT/t1.c"
  printf '__declspec(dllimport) int MeglerNoSuchFunction(void);\n__declspec(dllexport) int mg_dl_t2(void) { return MeglerNoSuchFunction(); }\n' > "$DLT/t2.c"
  for t in t1:nowhere t2:k32; do
    src="${t%%:*}" imp="${t##*:}"
    "$VC/bin/Hostx64/x64/lib.exe" /nologo /machine:x64 "/def:$(w "$DLT/$imp.def")" "/out:$(w "$DLT/$imp.lib")" > /dev/null
    "$VC/bin/Hostx64/x64/cl.exe" /nologo /c /GS- "/Fo$(w "$DLT/$src.obj")" "$(w "$DLT/$src.c")" > /dev/null
    "$VC/bin/Hostx64/x64/link.exe" /nologo /DLL /NOENTRY /NODEFAULTLIB "/OUT:$(w "$OUT/mg-dl-$src.dll")" "$(w "$DLT/$src.obj")" \
      "$(w "$DLT/$imp.lib")" > /dev/null
    [ -f "$OUT/mg-dl-$src.dll" ] || { echo "mg-dl-$src.dll was not made" >&2; exit 1; }
  done
  # The shell's start and its copies (shell/Files.cpp, ShellLog.cpp, Startup.cpp, BundledApks.cpp and Strings.cpp), the
  # package volume Megler installs onto (shell/Placement.cpp), the apps' APKs in the shared folder (shell/SharedApks.cpp), and
  # an app's permissions as the shell keeps and decides them (player/PermissionRules.cpp, shell/Settings.cpp), the very
  # files the phone's package is built from, in a program of their own with the tests that drive them
  # (core/tests/mg_shell_windows.cpp): mg_tests.exe stands in for the shell's files and log, as its storage, codec and camera
  # tests want, and these want the real ones. Compiled as for an app container, as the storage host is (onecore.lib: an app's
  # VirtualAlloc is VirtualAllocFromApp).
  SHELL_SRC=("$(w "$ROOT/app/Megler/shell/Files.cpp")" "$(w "$ROOT/app/Megler/shell/Strings.cpp")" "$(w "$ROOT/app/Megler/shell/ShellLog.cpp")"
             "$(w "$ROOT/app/Megler/shell/Startup.cpp")" "$(w "$ROOT/app/Megler/shell/BundledApks.cpp")" "$(w "$ROOT/app/Megler/shell/Placement.cpp")"
             "$(w "$ROOT/app/Megler/shell/SharedApks.cpp")" "$(w "$ROOT/app/Megler/shell/Settings.cpp")" "$(w "$ROOT/app/Megler/player/PermissionRules.cpp")"
             "$(w "$ROOT/core/tests/mg_shell_windows.cpp")")
  mkdir -p "$OUT/shellobj"
  rm -f "$OUT/shellobj/"*.obj
  INCLUDE="$INCLUDE;$(w "$SDK/Include/$SDKV/winrt");$(w "$SDK/Include/$SDKV/cppwinrt")" \
    "$VC/bin/Hostx64/x64/cl.exe" "${GFX_FLAGS[@]}" /Zc:__cplusplus /bigobj /DWINAPI_FAMILY=WINAPI_FAMILY_APP /O1 "$MP" "/Fo$(w "$OUT/shellobj")\\" "${SHELL_SRC[@]}" \
    | grep -v -E '^[A-Za-z0-9_]+\.cpp$' || true
  for obj in Files Strings ShellLog Startup BundledApks Placement SharedApks Settings PermissionRules mg_shell_windows; do
    [ -f "$OUT/shellobj/$obj.obj" ] || { echo "compile failed: $obj.cpp" >&2; exit 1; }
  done
  "$VC/bin/Hostx64/x64/link.exe" /nologo "/OUT:$(w "$OUT/mg_shell_tests.exe")" "$(w "$OUT/shellobj")\\*.obj" bcrypt.lib ole32.lib oleaut32.lib onecore.lib
  echo "built $OUT/mg_shell_tests.exe"
  if [ "$RUN" = 1 ]; then
    rm -rf "$OUT/scratch"
    APK="${MEGLER_TEST_APK:-$USERPROFILE/Downloads/Hollow_Knight_22.10.2022_LQ.apk}"
    DVM=(--dvm "$(w "$ROOT/build/guest/armeabi-v7a/dvm")")
    [ -f "$APK" ] && DVM+=(--dvm-apk "$APK")
    "$OUT/mg_tests.exe" - "$(w "$OUT/scratch")" "${DVM[@]}" "${EXTRA[@]}" || STATUS=1
    if [ "$LIBCORE" = 1 ]; then
      bash "$ROOT/tools/dvm-android-root.sh"
      echo "$AGAIN"
      rm -rf "$OUT/scratch"
      "$OUT/mg_tests.exe" - "$(w "$OUT/scratch")" "${DVM[@]}" --dvm-android "$(w "$ROOT/build/android9/root")" "${EXTRA[@]}" || STATUS=1
    fi
    # The shell's drawing: its scenes on this machine's graphics processor, as PNG files in build/gfx-pictures (and in
    # memory without one, which its first line says); then, without the files, in memory by Direct2D's software
    # rasterizer, which is what a phone without a device for it draws with; in memory the way it would draw if Direct2D
    # refused the app's memory; and on WARP, Direct3D's software device, a second rasterizer of the graphics processor's
    # kind.
    rm -rf "$ROOT/build/gfx-pictures"
    "$OUT/mg_tests.exe" --gfx-windows "$(w "$ROOT/build/gfx-pictures")" || STATUS=1
    "$OUT/mg_tests.exe" --gfx-windows - memory || STATUS=1
    "$OUT/mg_tests.exe" --gfx-windows - copy || STATUS=1
    "$OUT/mg_tests.exe" --gfx-windows - warp || STATUS=1
    # And with Android's own fonts in place of the system's, as the phone's package has them, where they have been taken
    # out of the image (tools/android9-unpack.sh fonts).
    if [ -f "$ROOT/build/android9/root/fonts/Roboto-Regular.ttf" ]; then
      echo "(with Android's own fonts)"
      MEGLER_GFX_FONTS="$(w "$ROOT/build/android9/root/fonts")" "$OUT/mg_tests.exe" --gfx-windows - || STATUS=1
    fi
    # The shell's window (player/WindowHost.cpp) with no panel, on this PC's graphics processor: each way a picture of an
    # app's comes -- drawn by the shell's drawing, in memory, a game's frame in ANGLE's texture -- counted as it comes, the
    # first said in the log with what its Present said and the player told of it at once, which its busy overlay goes by;
    # and a picture whose Present is refused (the flag fail-present).
    "$OUT/mg_tests.exe" --window-windows || STATUS=1
    # The shell's storage host: the storage's tests once more, with Windows.Storage's own objects handing the handles over
    # instead of the stand-in, on folders under a scratch folder of its own.
    rm -rf "$OUT/scratch-storage"
    "$OUT/mg_tests.exe" --storage-windows "$(w "$OUT/scratch-storage")" || STATUS=1
    # The shell's start and its copies: files copied past the cache and mended, the log's last words written out at once,
    # the start's steps and watch, the plan of the copies of the package's Google APKs and their making after the start.
    rm -rf "$OUT/scratch-shell"
    "$OUT/mg_shell_tests.exe" "$(w "$OUT/scratch-shell")" || STATUS=1
    # The shell's decoders: the core's codec for MediaCodec over Media Foundation's own decoder transforms, fed the data
    # app's streams (H.264 and H.265 with B-pictures, AAC, MP3) and checked against what they hold; and timed on longer
    # streams of 720p and 1080p, made once into build/media-speed where Python and ffmpeg are (they are not kept in the
    # repository).
    rm -rf "$OUT/scratch-codec"
    SPEED=()
    if [ -f "$ROOT/build/media-speed/speed-avc-1080p.mgau" ] || python "$(w "$ROOT/tools/media-test-streams.py")" --speed "$(w "$ROOT/build/media-speed")" > /dev/null 2>&1; then
      SPEED=("$(w "$ROOT/build/media-speed")")
    else
      echo "(the streams to time the decoders by could not be made: tools/media-test-streams.py --speed needs Python and ffmpeg)"
    fi
    "$OUT/mg_tests.exe" --codec-windows "$(w "$ROOT/core/tests/app/data/assets")" "$(w "$OUT/scratch-codec")" ${SPEED[@]+"${SPEED[@]}"} || STATUS=1
    # Bundles of split APKs, unpacked as the shell unpacks them: the test app's and the real ones in build/bundles.
    rm -rf "$OUT/scratch-bundles"
    "$OUT/mg_tests.exe" --bundles "$(w "$ROOT/build/guest/java")" "$(w "$OUT/scratch-bundles")" "$(w "$ROOT/build/bundles")" || STATUS=1
    rm -rf "$OUT/scratch-bundles"
    # The shell's cameras: the PC's first camera through the core's camera code, a few pictures taken; a PC without one says so.
    rm -rf "$OUT/scratch-camera"
    "$OUT/mg_tests.exe" --camera-windows "$(w "$OUT/scratch-camera")" || STATUS=1
    # The shell's sensors: what this PC has through the core's sensors, each read a moment; a PC without them says so.
    rm -rf "$OUT/scratch-sensors"
    "$OUT/mg_tests.exe" --sensors-windows "$(w "$OUT/scratch-sensors")" || STATUS=1
    # The shell's Bluetooth: this PC's radio through the core's, and a passive scan of three seconds (nothing connected to or
    # paired with); a PC without one says so.
    rm -rf "$OUT/scratch-bluetooth"
    "$OUT/mg_tests.exe" --bluetooth-windows "$(w "$OUT/scratch-bluetooth")" || STATUS=1
    # The shell's Wi-Fi: this PC's Wi-Fi through the core's, one scan and the network it is connected to; a PC without one says so.
    rm -rf "$OUT/scratch-wifi"
    "$OUT/mg_tests.exe" --wifi-windows "$(w "$OUT/scratch-wifi")" || STATUS=1
    # The Xbox build's Device Portal client, against a portal of the tests' own (tools/device-portal-mock.py) on a port of
    # its choosing on 127.0.0.1: over TLS with a certificate openssl makes here for another name, nothing vouching for it, as
    # the console's own is; the first two questions of the install's state answered 204, as another client's install
    # would be. Without Python it is said and left out.
    PORTAL="$OUT/scratch-portal"
    rm -rf "$PORTAL" && mkdir -p "$PORTAL"
    if command -v python > /dev/null; then
      TLS=()
      if command -v openssl > /dev/null &&
         openssl req -x509 -newkey rsa:2048 -nodes -keyout "$(w "$PORTAL/key.pem")" -out "$(w "$PORTAL/cert.pem")" -days 2 \
           -subj "/CN=xbox-mock.invalid" > /dev/null 2>&1; then
        TLS=(--cert "$(w "$PORTAL/cert.pem")" --key "$(w "$PORTAL/key.pem")")
      else
        echo "(no openssl: the mock Device Portal answers over plain HTTP)"
      fi
      python -I "$(w "$ROOT/tools/device-portal-mock.py")" --ready "$(w "$PORTAL/ready")" --user portal-test --password "mock-pw-1" \
        --received "$(w "$PORTAL/received")" --busy-first 2 --log "$(w "$PORTAL/mock.log")" ${TLS[@]+"${TLS[@]}"} &
      MOCK=$!
      for _ in $(seq 100); do [ -f "$PORTAL/ready" ] && break; sleep 0.1; done
      if [ -f "$PORTAL/ready" ]; then
        "$OUT/mg_tests.exe" --portal-windows "$(tr -d '\r\n' < "$PORTAL/ready")" portal-test "mock-pw-1" "$(w "$PORTAL")" "$(w "$PORTAL/received")" \
          "$(w "$PORTAL/mock.log")" || STATUS=1
      else
        echo "the mock Device Portal did not start (tools/device-portal-mock.py)" >&2
        STATUS=1
      fi
      kill "$MOCK" 2> /dev/null || true
      wait "$MOCK" 2> /dev/null || true
    else
      echo "(no Python: the Xbox build's Device Portal client was not run against the mock portal)"
    fi
    # The Java test apps' own drawing, as `tools/build-tests.sh arm` recorded it under qemu: drawn again by the stand-in,
    # which must give the recorded pictures back pixel for pixel, and then by the shell's drawing, into
    # build/gfx-pictures/app-<app>-N.png -- the app's views as a phone's Direct2D and DirectWrite would draw them, laid
    # out as the stand-in's measures of text had them laid out.
    for app in hello demos; do
      if [ -f "$ROOT/build/gfx-records/$app.rec" ]; then
        "$OUT/mg_tests.exe" --gfx-replay "$(w "$ROOT/build/gfx-records/$app.rec")" - "app-$app" standin || STATUS=1
        "$OUT/mg_tests.exe" --gfx-replay "$(w "$ROOT/build/gfx-records/$app.rec")" "$(w "$ROOT/build/gfx-pictures")" "app-$app" || STATUS=1
        "$OUT/mg_tests.exe" --gfx-replay "$(w "$ROOT/build/gfx-records/$app.rec")" - "app-$app" memory || STATUS=1
      else
        echo "(no record of the Java test app $app's drawing in build/gfx-records: tools/build-tests.sh arm makes one)"
      fi
    done
  fi
else
  echo "usage: tools/build-tests.sh arm|linux64|arm64|win" >&2
  exit 2
fi
exit $STATUS
