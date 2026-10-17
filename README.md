# megler-w10m

Android games on **Windows 10 Mobile** (ARM32, 1703 and later): the translation layer
of an earlier project, which does the same on an iPhone, written
for a Windows phone.

An app's own Android code runs directly on the phone's processor. Megler stands in for
Android around it: the dynamic linker, the C library, the NDK, the Java calls the app
makes, and OpenGL ES (through ANGLE, on Direct3D 11). No Android system boots and
nothing is emulated. The original project's second mode, a whole Android under QEMU, is not part of
this port.

**This is a first stage.** The core runs real Android libraries and a complete
NativeActivity app under qemu-arm on a PC and on a phone (a Snapdragon 820 one, where
its own tests pass and its demo draws, sounds and follows fingers), and one real
game, a Unity one, is played there, with its sound and its opening films.
[docs/STATUS.md](docs/STATUS.md) says exactly what has been seen working and where,
and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) how it works and what a phone still
has to answer.

## What can run

- APKs with **32-bit ARM** native code (`armeabi-v7a`, or `armeabi`). Windows 10
  Mobile is a 32-bit system on every phone, so `arm64-v8a` code cannot run, and an
  APK that has only that cannot run here. Older versions of most games still carry
  32-bit libraries.
- Apps built on **NativeActivity** (`android_native_app_glue`), and games made with
  **Unity** (IL2CPP): the second driver is new, and one game has run with it on a
  phone (see the status). SDL, cocos2d-x, Unreal and Godot each need a driver of
  their own, as in the original project; apps written only in Java or Kotlin need a Java runtime.
  One is under way ([docs/JAVA.md](docs/JAVA.md)): a Dalvik interpreter joined to
  the JNI layer, Android 9's own class library under it (libcore, with its
  native libraries built here for ARMv7), the original Java framework
  on top, and drawing by the shell with Direct2D. Small test apps written in
  Java run on a phone with it -- views, lists, dialogs, a second Activity,
  preferences, background tasks, databases, a native library of their own --
  and the first real apps start there (docs/STATUS.md says how far each
  gets), their text in Android's own fonts. HTTPS through Android's HTTP
  client and Conscrypt has also been checked on the HP Elite x3.
- OpenGL ES 2.0 on any phone; 3.0 where the GPU's driver gives Direct3D feature
  level 10 or more. No Vulkan.
- Sound through OpenSL ES buffer queues (what FMOD, OpenAL Soft and SDL sit on).
  Films an engine plays through `libmediandk.so`, when they are something Windows
  decodes (MP4 with H.264 and AAC).
- The network over the phone's own Winsock: TCP and UDP, servers included, names,
  the interfaces, from native code and from Java (`HttpURLConnection` over
  http, `java.net`, NIO). Tested on a PC, under qemu and on the HP Elite x3.
  TLS uses Android 9's Conscrypt and BoringSSL: the phone passes the loopback
  TLS tests and reads external HTTPS responses with normal certificate and
  hostname verification. Individual apps can still require unsupported APIs.

The app's library page tells, for each APK, what it is and why it can or cannot run.

## How Android code runs under UWP

| Need | Solution |
|---|---|
| Executable memory | `rescap:codeGeneration`; a library's pages are committed read-write, filled, relocated and patched, then turned execute-read with `VirtualProtectFromApp`, once (an executable commit and a writable-executable page are both refused). The tricks are [gecko-w10m](../gecko-w10m)'s and [podium-w10m](../podium-w10m)'s. |
| Android's float ABI | Android passes floats in integer registers, Windows in VFP registers. Every function that crosses takes and returns the bits as integers, which is exactly how the Android side passes them; no assembly. |
| The thread pointer | Android code reads `TPIDRURO` inline. Each such instruction is redirected to three that load the guest's thread pointer from a `TlsAlloc` slot in the TEB. |
| ARM-mode PLT entries | Windows on ARM keeps a thread in Thumb state only. Each PLT entry becomes one ARM instruction that is an undefined instruction when read as Thumb, so it works in either state: taken normally, or rescued by the fault handler. |
| ARM-mode functions | Run as they are. Whether a phone keeps them running is what the app's **Device checks** measures. |
| OpenGL ES | ANGLE's `libEGL.dll`/`libGLESv2.dll` as gecko-w10m builds them. The app's window surface is a texture on ANGLE's Direct3D device; the shell copies each frame to its `SwapChainPanel`'s swap chain. |
| bionic's 4-byte mutexes | Implemented in the word itself, waiting on its address (`WaitOnAddress`). |

## Layout

```
core/             the layer, portable C11
  include/megler.h  what the shell sees
  src/host/       the OS (Win32 app container; POSIX for the tests)
  src/ld/ arm/    the dynamic linker; what is rewritten in guest code
  src/bionic/     libc, libm, libdl, liblog, pthreads, files
  src/ndk/ jni/   libandroid, EGL/GLES over ANGLE; JNI over a Java world in C
  src/drivers/    the Java side of each kind of app
  src/dvm/        a Dalvik interpreter for an app's own bytecode (docs/JAVA.md)
  src/apk/ zip/   APKs, manifests, resources
  tests/          the tests, and guest/: Android libraries and an app for them
app/Megler/     the C++/WinRT shell
tools/            build-appx.sh, build-tests.sh, build-guest.sh, phone.py;
                  android9-*.sh and vdex-dex.py: Android 9's class library, from
                  Google's image and AOSP's source, into build/android9
docs/             ARCHITECTURE.md, STATUS.md, JAVA.md
```

## Build

Requirements: Visual Studio 2022 17.14 (MSVC 14.44 with the ARM build tools),
Windows SDK 10.0.22621 (headers, tools) and 10.0.15063 (ARM libraries), LLVM (clang
and ld.lld, for the guest test libraries), Git Bash, Python 3. For the tests that
execute Android code: WSL Ubuntu with `gcc-arm-linux-gnueabihf` and `qemu-user`.
ANGLE comes from a gecko-w10m build (`C:\rw-obj\dist\bin`, or `MEGLER_ANGLE`).

```bash
bash tools/build-guest.sh          # the Android test libraries and native-test.apk (clang, no NDK)
bash tools/build-tests.sh arm      # the core's tests under qemu-arm: guest code really runs
bash tools/build-tests.sh win      # the Win32 host layer and the APK reader, on this PC
bash tools/build-appx.sh arm       # → app/Megler/AppPackages/Megler_<ver>_ARM.appx
```

`build/tests-win/hk_tests.exe --apk some.apk` prints what the layer makes of an APK;
`hk_tests --load <dir> <lib>...` (the ARM build, under qemu) loads real Android
libraries from a folder and says what each needed; `hk_tests --run <apk> <data dir>`
starts an APK there with a stand-in for OpenGL ES, touches it and presses Back.

## On the phone

The phone must be interop-unlocked (for `codeGeneration`), in Developer mode, and
trust the package's certificate (`AppPackages/Megler.cer`; the package is signed with
podium-w10m's certificate unless `MEGLER_PFX` names another).

Install with the Device Portal, or `python tools/phone.py install` (it pins the
target phone by name; `python tools/phone.py logs` pulls `megler.log` and the check
logs from LocalState). Then, in the app:

1. **Device › Probe** first. Its lines are the answers this port is waiting
   for (ARCHITECTURE.md lists them); `LocalState/megler-probe.log` keeps them. It
   ends with a second of a 440 Hz tone, played straight through XAudio2: if that is
   not heard, look at the phone's Media + Apps volume before anything else.
2. **Device › Self-test**: the core's tests on the phone itself. It runs a small app
   of its own, which uses up the one app a start of Megler can run: close and reopen
   Megler before playing.
3. **Add the Demo** (offered while the library is empty), then **Play**: the
   package's own small app, whose picture says what is right. An F in the top left
   corner (the picture is the right way up), red, green and blue squares left to
   right at the bottom (the colours are), a triangle turning clockwise once in four
   seconds, a bar along the top that is full width at 60 frames a second, a square
   and a cross under each finger, and a tone for the first second and then while a finger is down. Back ends it.
4. **Add APK**, then **Play**.

Android's shared storage (`/sdcard`) is one folder for all apps, and the phone's
music, pictures and videos are in it as `Music`, `Pictures` and `Movies` (the
camera's as `DCIM`). **Settings › Android Storage** lists them and adds any
other folder under a name of your choosing -- the phone's Documents or
Downloads, a folder of a memory card -- and **Check the Folders** there tries
in each what an app will do to files, in a folder of its own that it removes
again; every step is in the Log. The four folders and the check have run on a
phone (102 steps, all passed); the Settings section and adding a folder have
not ([docs/STATUS.md](docs/STATUS.md)).

One app runs per start of Megler: Android libraries cannot be unloaded, so the app has
to be closed and opened again to play another.

## License

Megler is licensed under the GNU General Public License version 3 (LICENSE). The vendored zlib is under the zlib
license; ANGLE is BSD.
