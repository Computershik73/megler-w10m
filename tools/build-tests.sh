#!/usr/bin/env bash
# Builds and runs the core's tests.
#
#   tools/build-tests.sh arm [--no-run] [--no-libcore] [--no-java] [-- extra arguments for hk_tests]
#       The 32-bit ARM build, in WSL with arm-linux-gnueabihf-gcc, run under
#       qemu-arm: the one that executes guest code. Thumb-2 and hard-float,
#       like the phone's build. Needs the guest libraries (tools/build-guest.sh).
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
# which ends on an exception nothing catches. And `data`, a database through
# the framework's android.database.sqlite, twice, the second run on what the
# first left, where the core has SQLite in it (build/sqlite). Their
# pictures are written to build/java-pictures as BMP files. Each is then run
# once more with its calls of the shell's drawing recorded
# (build/gfx-records/<app>.rec), for the Windows build to draw again.
# --no-java leaves these out.
#
# Objects go to ~/megler-w10m-build inside WSL (the Windows drive is slow from
# there) and to build/tests-win here. MEGLER_WSL_BUILD names another folder
# in WSL, as WSL's own shell reads it (quoted here, so that this shell leaves
# the ~ alone: MEGLER_WSL_BUILD='~/megler-w10m-build-two'): two checkouts
# building at once must not share one.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WB="${MEGLER_WSL_BUILD:-}"
[ -n "$WB" ] || WB='~/megler-w10m-build'
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
# SQLite is compiled into the core where its amalgamation is (build/sqlite/sqlite3.c, tools/sqlite-fetch.sh): core/src/sqlite/hk_sqlite3.c
# includes it. Without it the core builds as before and says, when an app asks for a database, that it has none.
SQLITE_GCC=""
SQLITE_CL=()
if [ -f "$ROOT/build/sqlite/sqlite3.c" ] && [ -f "$ROOT/build/sqlite/sqlite3.h" ]; then
  SQLITE_GCC="-DHK_HAVE_SQLITE=1 -Ibuild/sqlite"
  SQLITE_CL=(/DHK_HAVE_SQLITE=1 "/I$(cygpath -w "$ROOT/build/sqlite")")
else
  echo "(no build/sqlite/sqlite3.c: the core is built without SQLite, and the tests that need it are left out)"
fi
# libwebp's decoder likewise, where its sources are (build/webp/src, tools/webp-fetch.sh): core/src/webp/hk_webp_*.c each compile one
# of its files. Without it a WebP picture goes to the shell's decoder like any other, and the tests of the core's own are left out.
WEBP_GCC=""
WEBP_CL=()
if [ -f "$ROOT/build/webp/src/dec/webp_dec.c" ] && [ -f "$ROOT/build/webp/src/webp/decode.h" ]; then
  WEBP_GCC="-DHK_HAVE_WEBP=1 -Ibuild/webp"
  WEBP_CL=(/DHK_HAVE_WEBP=1 "/I$(cygpath -w "$ROOT/build/webp")")
else
  echo "(no build/webp/src: the core is built without its WebP decoder, and the tests that need it are left out)"
fi
AGAIN="the tests again, the Dalvik runtime on Android's libcore (build/android9/root)"
mapfile -t SRCS < <(cd "$ROOT" && find core/src core/tests -maxdepth 3 -name '*.c' ! -path 'core/tests/guest/*' | sort)
mapfile -t ZSRCS < <(cd "$ROOT" && find core/third_party/zlib -maxdepth 1 -name '*.c' ! -name 'gz*.c' | sort)

if [ "$MODE" = arm ]; then
  { [ -f "$ROOT/build/guest/armeabi-v7a/libt_basic.so" ] && [ "$ROOT/build/guest/armeabi-v7a/libt_jni.so" -nt "$ROOT/core/tests/guest/t_jni.c" ] \
    && [ "$ROOT/build/guest/armeabi-v7a/runtime/libt_ruser.so" -nt "$ROOT/core/tests/guest/t_user.c" ]; } || bash "$ROOT/tools/build-guest.sh" > /dev/null
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
     || { [ "\$src" = core/src/sqlite/hk_sqlite3.c ] && [ -f build/sqlite/sqlite3.c ] && [ build/sqlite/sqlite3.c -nt "\$obj" ]; }; then
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
arm-linux-gnueabihf-gcc -mthumb -o \$OUT/hk_tests \$OUT/obj/*.o -lm -lpthread -ldl
echo "built \$OUT/hk_tests"
EOF
  MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash "$WROOT/build/tests-arm.sh"
  if [ "$RUN" = 1 ]; then
    # (The layer's log is the program's standard error; the two are joined inside WSL, where lines stay whole.)
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "rm -rf $WB/scratch && qemu-arm -L /usr/arm-linux-gnueabihf $WB/hk_tests '$WROOT/build/guest/armeabi-v7a' $WB/scratch \$([ -f $WB/hk.apk ] && echo --dvm-apk $WB/hk.apk) ${EXTRA[*]:-} 2>&1" || STATUS=1
    if [ "$LIBCORE" = 1 ]; then
      bash "$ROOT/tools/dvm-android-root.sh"
      echo "$AGAIN"
      # The folder is copied into WSL's own disk: the Windows drive is slow from there, and ICU maps 24 MB of it.
      MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "mkdir -p $WB/android-root && cp -ru '$WROOT/build/android9/root/.' $WB/android-root/ && rm -rf $WB/scratch && qemu-arm -L /usr/arm-linux-gnueabihf $WB/hk_tests '$WROOT/build/guest/armeabi-v7a' $WB/scratch --dvm-android $WB/android-root \$([ -f $WB/hk.apk ] && echo --dvm-apk $WB/hk.apk) ${EXTRA[*]:-} 2>&1" || STATUS=1
    fi
    if [ "$JAVA" = 1 ]; then
      for app in hello demos natives crash; do
        if [ ! -f "$ROOT/build/guest/java/$app.apk" ]; then
          echo "(the Java test app $app is not run: build/guest/java/$app.apk was not built -- its native library needs the NDK, see tools/build-test-app.sh)"
          continue
        fi
        echo "the Java test app $app (build/guest/java/$app.apk), through megler_run"
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp -u '$WROOT/build/guest/java/$app.apk' $WB/$app.apk && rm -rf $WB/java-data-$app && qemu-arm -L /usr/arm-linux-gnueabihf $WB/hk_tests --java $app $WB/$app.apk $WB/java-data-$app $WB/android-root '$WROOT/build/java-pictures' 2>&1" || STATUS=1
      done
      # The database app, twice: the second run on the data folder the first left, to find what it wrote. Only where the core has SQLite in it.
      if [ -n "$SQLITE_GCC" ] && [ -f "$ROOT/build/guest/java/data.apk" ]; then
        for run in 1 2; do
          echo "the Java test app data (build/guest/java/data.apk), through megler_run: run $run on its database"
          CLEAN=""
          [ "$run" = 1 ] && CLEAN="rm -rf $WB/java-data-data &&"
          MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp -u '$WROOT/build/guest/java/data.apk' $WB/data.apk && $CLEAN qemu-arm -L /usr/arm-linux-gnueabihf $WB/hk_tests --java data $WB/data.apk $WB/java-data-data $WB/android-root '$WROOT/build/java-pictures' 0 480x800@160 run:$run 2>&1" || STATUS=1
        done
      else
        echo "(the Java test app data is not run: the core was built without SQLite)"
      fi
      # And an upright surface, as a phone held upright gives an app that does not ask for landscape: the same app, the same checks.
      echo "the Java test app hello again, on an upright surface (720 x 1280 at 320 dpi)"
      MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "cp -u '$WROOT/build/guest/java/hello.apk' $WB/hello.apk && rm -rf $WB/java-data-hello && qemu-arm -L /usr/arm-linux-gnueabihf $WB/hk_tests --java hello $WB/hello.apk $WB/java-data-hello $WB/android-root '$WROOT/build/java-pictures-upright' 0 720x1280@320 2>&1" || STATUS=1
      # Each of them once more, with every call it makes of the shell's drawing written to build/gfx-records/<app>.rec
      # (core/tests/hk_gfx_record.c): what `tools/build-tests.sh win` draws again with Direct2D and DirectWrite.
      mkdir -p "$ROOT/build/gfx-records"
      for app in hello demos; do
        MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "rm -rf $WB/java-data-$app && qemu-arm -L /usr/arm-linux-gnueabihf $WB/hk_tests --gfx-record $app $WB/$app.apk $WB/java-data-$app $WB/android-root $WB/$app.rec 2>/dev/null && cp $WB/$app.rec '$WROOT/build/gfx-records/$app.rec'" || STATUS=1
      done
    fi
    # The shell's drawing (Direct2D, DirectWrite, WIC) is only in the Windows build: this one has the mode and says so.
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL="*" wsl.exe -- bash -c "qemu-arm -L /usr/arm-linux-gnueabihf $WB/hk_tests --gfx-windows - 2>&1" || STATUS=1
  fi
elif [ "$MODE" = win ]; then
  for cl in "/c/Program Files/Microsoft Visual Studio/2022/"*/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe; do
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
  OUT="$ROOT/build/tests-win"
  mkdir -p "$OUT/obj" "$OUT/zobj"
  ALL=()
  for s in "${SRCS[@]}"; do ALL+=("$(w "$ROOT/$s")"); done
  ZALL=()
  for s in "${ZSRCS[@]}"; do ZALL+=("$(w "$ROOT/$s")"); done
  # (HK_GFX_WINDOWS, HK_STORAGE_WINDOWS: this build has the shell's drawing and its storage host in it; hk_tests_main.c's
  # --gfx-windows and --storage-windows modes.)
  "$VC/bin/Hostx64/x64/cl.exe" /nologo /c /O1 /MT /W3 /std:c11 /utf-8 /MP /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS /DHK_GFX_WINDOWS /DHK_STORAGE_WINDOWS "${SQLITE_CL[@]}" "${WEBP_CL[@]}" \
    "/I$(w "$ROOT/core/third_party/zlib")" "/Fo$(w "$OUT/obj")\\" "${ALL[@]}" | grep -v -E '^[A-Za-z0-9_]+\.c$' || true
  "$VC/bin/Hostx64/x64/cl.exe" /nologo /c /O1 /MT /w /MP /D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_WARNINGS \
    "/Fo$(w "$OUT/zobj")\\" "${ZALL[@]}" | grep -v -E '^[A-Za-z0-9_]+\.c$' || true
  # The shell's drawing itself, the very file the phone's package is built from (it is plain COM, no WinRT), and the few
  # lines that hand it to the tests: C++, with the shell's own flags.
  GFX_SRC=("$(w "$ROOT/app/Megler/player/GfxHost.cpp")" "$(w "$ROOT/core/tests/hk_gfx_windows.cpp")")
  GFX_FLAGS=(/nologo /c /MT /W3 /EHsc /GR /permissive- /std:c++20 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
             "/I$(w "$ROOT/app/Megler")" "/I$(w "$ROOT/core/include")")
  mkdir -p "$OUT/gfxobj"
  rm -f "$OUT/gfxobj/"*.obj
  "$VC/bin/Hostx64/x64/cl.exe" "${GFX_FLAGS[@]}" /O2 /MP "/Fo$(w "$OUT/gfxobj")\\" "${GFX_SRC[@]}" | grep -v -E '^[A-Za-z0-9_]+\.cpp$' || true
  for obj in GfxHost hk_gfx_windows; do
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
  # in for the rest of the shell (core/tests/hk_storage_windows.cpp). The shell's precompiled header is read as a plain
  # header here; the projection's headers are the SDK's own. Compiled as for an app container (WINAPI_FAMILY_APP), which
  # is what the phone's package is compiled as: the headers then show what an app may call and nothing else.
  STORAGE_SRC=("$(w "$ROOT/app/Megler/player/StorageHost.cpp")" "$(w "$ROOT/app/Megler/shell/Strings.cpp")" "$(w "$ROOT/core/tests/hk_storage_windows.cpp")")
  mkdir -p "$OUT/storageobj"
  rm -f "$OUT/storageobj/"*.obj
  INCLUDE="$INCLUDE;$(w "$SDK/Include/$SDKV/winrt");$(w "$SDK/Include/$SDKV/cppwinrt")" \
    "$VC/bin/Hostx64/x64/cl.exe" "${GFX_FLAGS[@]}" /Zc:__cplusplus /bigobj /DWINAPI_FAMILY=WINAPI_FAMILY_APP /O1 /MP "/Fo$(w "$OUT/storageobj")\\" "${STORAGE_SRC[@]}" \
    | grep -v -E '^[A-Za-z0-9_]+\.cpp$' || true
  for obj in StorageHost Strings hk_storage_windows; do
    [ -f "$OUT/storageobj/$obj.obj" ] || { echo "compile failed: $obj.cpp" >&2; exit 1; }
  done
  "$VC/bin/Hostx64/x64/link.exe" /nologo "/OUT:$(w "$OUT/hk_tests.exe")" "$(w "$OUT/obj")\\*.obj" "$(w "$OUT/zobj")\\*.obj" "$(w "$OUT/gfxobj")\\*.obj" \
    "$(w "$OUT/storageobj")\\*.obj" bcrypt.lib synchronization.lib d2d1.lib dwrite.lib ole32.lib oleaut32.lib ws2_32.lib iphlpapi.lib
  echo "built $OUT/hk_tests.exe"
  if [ "$RUN" = 1 ]; then
    rm -rf "$OUT/scratch"
    APK="${MEGLER_TEST_APK:-$USERPROFILE/Downloads/Hollow_Knight_22.10.2022_LQ.apk}"
    DVM=(--dvm "$(w "$ROOT/build/guest/armeabi-v7a/dvm")")
    [ -f "$APK" ] && DVM+=(--dvm-apk "$APK")
    "$OUT/hk_tests.exe" - "$(w "$OUT/scratch")" "${DVM[@]}" "${EXTRA[@]}" || STATUS=1
    if [ "$LIBCORE" = 1 ]; then
      bash "$ROOT/tools/dvm-android-root.sh"
      echo "$AGAIN"
      rm -rf "$OUT/scratch"
      "$OUT/hk_tests.exe" - "$(w "$OUT/scratch")" "${DVM[@]}" --dvm-android "$(w "$ROOT/build/android9/root")" "${EXTRA[@]}" || STATUS=1
    fi
    # The shell's drawing: its scenes as PNG files in build/gfx-pictures, and once more, without the files, the way it
    # would draw if Direct2D refused the app's memory.
    rm -rf "$ROOT/build/gfx-pictures"
    "$OUT/hk_tests.exe" --gfx-windows "$(w "$ROOT/build/gfx-pictures")" || STATUS=1
    "$OUT/hk_tests.exe" --gfx-windows - copy || STATUS=1
    # And with Android's own fonts in place of the system's, as the phone's package has them, where they have been taken
    # out of the image (tools/android9-unpack.sh fonts).
    if [ -f "$ROOT/build/android9/root/fonts/Roboto-Regular.ttf" ]; then
      echo "(with Android's own fonts)"
      MEGLER_GFX_FONTS="$(w "$ROOT/build/android9/root/fonts")" "$OUT/hk_tests.exe" --gfx-windows - || STATUS=1
    fi
    # The shell's storage host: the storage's tests once more, with Windows.Storage's own objects handing the handles over
    # instead of the stand-in, on folders under a scratch folder of its own.
    rm -rf "$OUT/scratch-storage"
    "$OUT/hk_tests.exe" --storage-windows "$(w "$OUT/scratch-storage")" || STATUS=1
    # The Java test apps' own drawing, as `tools/build-tests.sh arm` recorded it under qemu: drawn again by the stand-in,
    # which must give the recorded pictures back pixel for pixel, and then by the shell's drawing, into
    # build/gfx-pictures/app-<app>-N.png -- the app's views as a phone's Direct2D and DirectWrite would draw them, laid
    # out as the stand-in's measures of text had them laid out.
    for app in hello demos; do
      if [ -f "$ROOT/build/gfx-records/$app.rec" ]; then
        "$OUT/hk_tests.exe" --gfx-replay "$(w "$ROOT/build/gfx-records/$app.rec")" - "app-$app" standin || STATUS=1
        "$OUT/hk_tests.exe" --gfx-replay "$(w "$ROOT/build/gfx-records/$app.rec")" "$(w "$ROOT/build/gfx-pictures")" "app-$app" || STATUS=1
      else
        echo "(no record of the Java test app $app's drawing in build/gfx-records: tools/build-tests.sh arm makes one)"
      fi
    done
  fi
else
  echo "usage: tools/build-tests.sh arm|win" >&2
  exit 2
fi
exit $STATUS
