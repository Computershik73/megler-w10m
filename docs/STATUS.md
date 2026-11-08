# Status

What has been seen working, and where. "qemu" is the ARM32 build of the core under
qemu-arm on Linux (`tools/build-tests.sh arm`): it executes guest code, on a
hard-float Thumb-2 host like the phone, but its kernel is Linux. "PC" is the x64
Windows build (`tools/build-tests.sh win`): the real Win32 host layer, no guest
code. "Phone" is a Snapdragon 820 phone (Adreno 530) on Windows 10 Mobile
10.0.15254.603, where the Probe and the Self-test of build 0.1.0.18 ran to the end
on 2026-10-12, and the demo app (builds 0.1.0.19 and 0.1.0.21) put its picture on
the screen at 59 frames a second, followed five fingers, was heard, and ended on
Back. No real app has been tried.

| | State |
|---|---|
| Host layer, Win32 (memory, threads, wait-on-address, files) | **passes on the phone** and on PC (52 checks) |
| Executable memory in the app container (`codeGeneration`, write/execute flips) | **works on the phone**: a flip pair of one page takes 56–82 µs, an instruction-cache flush 8–15 µs a page |
| The guest thread pointer in a TEB slot | **works on the phone**: TEB+0xEA4, checked against `TlsGetValue` |
| An undefined instruction reaching the fault handler and the thread resumed elsewhere | **works on the phone**, in Thumb and in ARM state; 5 µs a time |
| A thread staying in ARM state | **it does on the phone**: 1.5 billion iterations (0.8 s) undisturbed |
| APK reader, manifest and resources | **passes on the phone**; read CPU-Z's and a Java app's real APKs correctly on PC (label, version, ABIs, icon) |
| Apps in several APKs: a bundle of split APKs (.apks, .xapk) unpacked into its base and the splits this phone can use (32-bit ARM, the densities nearest the screen's, English and the phone's languages, feature splits, asset packs), an .xapk's OBB put in the app's own `Android/obb`; the report of the app as one; the app run with its splits (docs/JAVA.md, "Split APKs and bundles") | **passes under qemu and on PC**: `mg_tests --bundles`, 125 checks -- the test app `splits` as an .apks and an .xapk, and three real bundles: BiP 4.2.42 and MangaDex 1.0.0.0 accepted (package, version, ABI), Magnit 8.107.1 refused for having native code for arm64-v8a only; and the test app from each bundle through `megler_run` under qemu, 32 checks each (its library loaded from `config.armeabi_v7a`, a picture and strings found in its density and language splits, an asset pack, a feature split's code, `splitNames` and `splitSourceDirs` as Android gives them, the OBB). **Not tried on a phone**: the library's picker for .apks and .xapk and its unpacking are the shell's |
| Loader: REL, packed (APS2) and RELR relocations, GNU and SysV hash, constructors, scope | **passes on the phone** |
| Thumb-2 guest library: data, calls both ways, floats, variadics, 64-bit division, heap, `qsort` callbacks, `setjmp`, threads, TLS, files, pipes | **passes on the phone** |
| The same library built in ARM mode | **passes on the phone** |
| PLT entries rewritten; the rescue of an entry entered in Thumb state | **passes on the phone** |
| A library that checks its own code (ByteDance's `libmetasec_ov.so` in TikTok Lite) left as the file has it: no PLT rewritten, its `svc`s and thread-pointer reads stopped at the processor's hardware breakpoints, every guest thread given them (docs/ARCHITECTURE.md, "Libraries left as the file has them") | **works on the phone** (HP Elite x3, 0.1.0.606 to 0.1.0.609): TikTok Lite starts and plays its feed, video after video, with unbroken sound |
| A library of Go's: its runtime's ARM `svc 0` led out of its code to the core, the calls made on a stack of the thread's for them where the caller is on a goroutine's stack, never a trap whose handler runs below it (docs/ARCHITECTURE.md, "Direct system calls") | **passes under qemu** (`test_go_stack`: 1305 calls by number from a Go-like library, 501 of them on goroutines' stacks of 2 KB with no memory below, registers, flags and VFP registers kept, here and on four threads; MCO Advanced's `libgojni.so`, Go 1.27.2, started and called through its own gomobile binding, `status`, `storage_info`, `network`, `refresh` and `start` answered). The Self-test runs the same checks on a phone. **Not tried on a phone**: there, as traps, MCO Advanced ended as its runtime started (package 669) |
| Real libraries: OpenSSL `libcrypto.so`/`libssl.so` (ARM mode), CPU-Z's `libcpuid.so` | **load under qemu**; SHA-256, AES and bignum results correct. Not tried on the phone |
| A NativeActivity app through `megler_run`: manifest, JNI, assets, looper, EGL, frames, touch, focus, pause, Back, teardown | **passes on the phone**, with a stand-in for OpenGL ES |
| Sound: an OpenSL ES buffer-queue player, resampled and mixed to 44.1 kHz stereo for the shell | **passes on the phone** as far as the mixer's output (sample counts and levels) |
| Sound: AAudio output streams (`libaaudio.so`), fed by a data callback or written to, on the same mixer | **passes under qemu**; not yet heard on the phone |
| Sound from Java: `AudioTrack`, `MediaPlayer` and `SoundPool` (`megler.Audio`), with Ogg Vorbis (stb_vorbis), WAV, and the shell's decoder for MP3 and AAC | **passes under qemu** (Vorbis and WAV) and **on PC** (MP3 and M4A through the shell's reader, which on the phone refused files with no picture until 0.1.0.250); not yet heard on the phone |
| Decoders for a Java app's `MediaCodec` (`megler.MediaCodecNative`, H.264, H.265, AAC, MP3) over a call of the shell's that decodes samples (`megler_codec_host`: Media Foundation's decoder transforms, `player/CodecHost.cpp`; docs/JAVA.md, "Decoders for MediaCodec") | **passes under qemu and on PC** with a stand-in for the shell's decoders (the core's tests; the data app's eight steps under qemu: pictures in display order on a `Surface`, a flush, the asynchronous mode, AAC raw and with ADTS, MP3, and a decoder that fills while its outputs wait, as Android's do and as ExoPlayer needs: the framework's own, as it came in `core/framework`, never filled), and **on PC with Windows' own decoders** through the shell's host (`mg_tests --codec-windows`: H.264 with B-pictures in display order in every form of parameter sets, a flush, a change of size, H.265, AAC, MP3, two decoders at once). **On the phone** (`--auto codec`, packages 245 to 248), all of that passes with Microsoft's decoders for H.264, H.265, AAC and MP3, except after a flush in H.265, where the first picture was black: mayconus's fix, the transform made again on a flush, passed all 77 codec checks on a Lumia 950 XL (docs/LUMIA-950XL.md) and has not been tried on the Elite x3 yet. HEVCVideoExtension, the phone's first H.265 decoder, never returned from a call; it is given up on after 8 s and remembered in `LocalState\codec-hung.txt`. Speed: H.264 720p 29 to 39 pictures a second, 1080p 15 to 17 (in software); H.265 1080p 31 to 40 (docs/JAVA.md, "On the phone") |
| Films: `libmediandk.so` (extractor, decoders, formats) over the shell's Media Foundation reader; pictures onto a `SurfaceTexture` | **works on the phone** with Hollow Knight's two opening films (H.264 360x202, AAC): every picture decoded and shown full screen, the sound in step with them; watched and heard by the phone's owner, played through and skipped |
| Video from Java: `MediaPlayer` and `VideoView` draw a file's pictures on the app's `Surface` (`megler.Audio.decVideo` over the shell's Media Foundation reader), fitted or cropped, timed by the sound or by a clock of their own (docs/JAVA.md, "Sound from Java"; mayconus's work, docs/LUMIA-950XL.md) | **passes on a JVM** with stand-ins for the host (`tools/test-media-player.py`, 24 checks: sound alone, pictures alone, both, pause, seek while paused, looping, a cancelled preparation, HTTP headers, VideoView's deferred start) and **on a Lumia 950 XL** (the test app `core/tests/app/video`: 8 checks, an H.264/AAC MP4 and one with no sound) and **on the HP Elite x3** (0.1.0.583, the same app as an app of the phone's own: the 8 checks pass, but at the full 1440x2560 render size the screen shows about 5 of the films' 30 pictures a second, each picture's canvas flushed to memory). A source is read whole into memory (256 MiB at most); `MediaExtractor` is still a stub, so ExoPlayer gets nothing from it |
| Cameras for a Java app's `android.hardware.Camera` (the overlay's class over `megler.CameraNative`, `drivers/mg_java_camera.c`, and the shell's `megler_camera_host` over Windows.Media.Capture, `player/CameraHost.cpp`; docs/JAVA.md, "The cameras") | **passes under qemu** with a stand-in for the shell's cameras (the core's 44 checks, on the PC as well; the test app `camera`, 38 checks: the parameters as Android keeps them, a preview on a `SurfaceView` and one on a `TextureView` checked in the window, turned and mirrored as Android shows them, frames in the app's buffers byte for byte and in their order, autofocus, the torch, the threads the callbacks come on; and 13 more with every camera refused, as when the user says no). The shell's host is built for the phone and into the PC's test program, whose `--camera-windows` opens this PC's one camera (DroidCam's virtual one) through it and takes ten pictures (9 checks). **On the phone** (package 267, VK mp3 mod's QR sign-in): the back camera opens and streams 640 x 640 at 30 a second, and every picture is drawn and read by ZXing, but the scanner showed white -- its frame's inverse path, now drawn as Android draws it (the camera test app frames its `TextureView` the same way); not yet tried again |
| The phone's sensors for a Java app's `SensorManager` and native code's `ASensorManager` (`ndk/mg_sensors.c`, `drivers/mg_java_sensors.c`, the overlay's `SensorManager` and `Sensor`; the shell's `megler_sensor_host` over Windows.Devices.Sensors, `player/SensorHost.cpp`; ARCHITECTURE.md, "Sensors"): the accelerometer, gravity, linear acceleration, gyroscope, magnetic field, rotation vectors, the old orientation, light, proximity and pressure, in Android's units and signs | **passes under qemu** with a stand-in for the shell's sensors (the core's tests of the list, every conversion, the times, rates, pause, flush and libandroid's functions, on the PC as well; the data app's step `sensors` in Java and the natives app's in native code on the main thread's looper). The shell's host is built for the phone and into the PC's test program, whose `--sensors-windows` finds no sensors on this PC. **Not tried on a phone**: the signs are Windows' documented ones (the hardware lab kit's tests) and Chromium's mapping of the orientation sensor and the inclinometer |
| Bluetooth LE's central side for a Java app's `android.bluetooth` (`run/mg_bluetooth.c`, `drivers/mg_java_bluetooth.c`, the overlay's `BluetoothAdapter`, `BluetoothDevice`, `BluetoothGatt` and `android.bluetooth.le`, `megler.Bluetooth`; the shell's `megler_bluetooth_host` over Windows.Devices.Bluetooth, `player/BluetoothHost.cpp`; the `bluetooth` capability in Megler's and the apps' packages; ARCHITECTURE.md, "Bluetooth LE"): the adapter and its state's broadcasts, scans with filters and the old one, GATT connections, services, reads, writes with and without response, notifications, the MTU, the RSSI, pairing with a PIN (the app's or the user's), the request to turn the radio on -- what MeshCore's app (flutter_blue_plus) calls | **passes under qemu** with a stand-in for the shell's radio (a MeshCore radio of the Nordic UART service that echoes writes as notifications and wants a PIN for one characteristic, a beacon): the core's tests (306 checks, on the PC as well) and the test app `bluetooth` (22 steps, 51 checks), which goes through what MeshCore's app does, Android 13's calls and callbacks. The shell's host is built for the phone and the PC and into the PC's test program, whose `--bluetooth-windows` listens passively (this PC has no Bluetooth adapter, which it says). **Not tried with a real device**: a MeshCore radio in range of a phone (or a PC) is what shows the connection, the database, the notifications, the MTU Windows negotiates and pairing with the radio's PIN |
| Android's runtime permissions (`megler.Permissions`: normal ones with the install, dangerous ones asked when the app requests them, by Windows for the camera, the microphone and the location and by Megler's own question for the storage, the media, the messages, the phone and the calls, kept for each app; docs/JAVA.md, "Permissions"), and an app's own package declaring only the capabilities its permissions need (docs/PER-APP-PACKAGES.md, "What a package declares") | **passes under qemu** (the data app's step `permissions` and the answers after it, two runs on one data folder, with a stand-in for the shell's answers; the report's permissions; the manifest made with the capabilities, `test_appx_manifest`). The shell's side (`player/PermissionHost.cpp`: Windows' consent read and asked, Megler's dialog, App Settings' Permissions) is built, **not tried on a phone** |
| The shared storage: one `/sdcard` for every app, the app's own `Android/data` and `obb` in it, and links to folders of the phone's own through handles the shell hands over | **passes under qemu and on PC** with a stand-in for the shell (`core/tests/mg_storage_tests.c`, 451 checks), and on PC with the shell's own host over Windows.Storage (`player/StorageHost.cpp`, the same checks; a test program is no app container). **On the phone** (HP Elite x3, 0.1.0.126, `tools/build-appx.sh arm --auto storage`): Music, Pictures, Movies and DCIM are found and answer, and the check's 102 steps pass -- 24 in each link, in a folder of its own: files made, written, read back, appended to, emptied, renamed, moved, removed, folders likewise, and a file moved from one link to another. One thing the PC did not show: an open straight after a close is at times refused with a sharing violation, something of Windows' own having the new file for a moment (2 steps of 102, different ones each run); the host asks again for up to a second. A folder is not moved into another folder by renaming (the app is told EXDEV, and copies). **Not tried**: the Settings section (the list, Add a Folder with the folder picker, the folders Windows remembers across starts), Documents, Downloads, a memory card |
| libc's POSIX regular expressions (`regcomp`, `regexec`, `regerror`, `regfree`: basic and extended, the leftmost longest, back references, GNU's word escapes), zlib's gzip files on the guest's files (`gzopen`, `gzdopen`, `gzread`, `gzgets`, `gzwrite`, `gzputs`, `gzrewind`, `gzclose` and the rest), `j0` and `j1`, `eventfd_read` and `eventfd_write`, `fchmodat`: what Zalo's libraries import that was not there | **passes under qemu and on PC** (`test_libc_gaps`, 44 checks) |
| The network: TCP and UDP sockets, servers included, blocking and not; the app's own AF_UNIX sockets; names (`getaddrinfo`, `getnameinfo`, `gethostbyname`); the interfaces; poll, select and epoll over sockets with pipes, eventfds and files (docs/ARCHITECTURE.md, "The network") | **passes under qemu and on PC** over loopback (`core/tests/mg_net_tests.c`: 234 checks under qemu, 225 on the PC, which cannot run the test of signal 34 ending a blocked call), and from Java under qemu (the test app `data`: `InetAddress`, `HttpURLConnection` against a `ServerSocket` of the app's own, `DatagramSocket`, a `ServerSocket` closed under the thread in `accept`, a `java.nio` selector, `NetworkInterface`). The check a phone is to run (`megler_net_check`: a public name, `HEAD /generate_204` on its port 80, a server of its own on 127.0.0.1, UDP on 127.0.0.1) passes on both with the internet. **Passed on the HP Elite x3, 2026-10-16**: 16 native network checks, Java loopback TLS including rejection of an untrusted certificate, external HTTP/HTTPS, and HTTP/2 ALPN through the Android 10 public API. See [PHONE-NETWORK.md](PHONE-NETWORK.md) for the app failures and fixes. |
| OpenGL ES from an app's Java: the 832 natives of `GLES10` to `GLES32` (793 generated, 39 by hand), EGL14 and EGL10 over `megler.EGLNative`, a `GLSurfaceView`'s renderer on a GL thread of the layer's own, the window composed in the core of the views over the frames read back (docs/ARCHITECTURE.md, "OpenGL ES from an app's Java") | **passes under qemu and on PC** on a stand-in for OpenGL ES that draws (`core/tests/mg_gl_tests.c`: 86 checks on libcore, 67 on the built-in java.lang; the test app `gl`: 49 checks under qemu -- a renderer's calls in Android's order, sixty frames a second at most, `RENDERMODE_WHEN_DIRTY`, `queueEvent`, a fixed size, pause and resume, EGL14 on a pbuffer, EGL10 and GL10 on a `SurfaceView`, and the picture). AIDA64 and Device Info read the GPU's strings and show them under qemu. A swap of a 720 x 1182 surface costs 1.45 ms on the PC with the stand-in's read-back (a copy of memory); ANGLE's waits for the GPU and **has not been measured: nothing of it has run on a phone** (the log's `java: OpenGL frames` line will say it there) |
| A `SurfaceView` drawn by an app's native code: `ANativeWindow_fromSurface` on its `Surface`, EGL window surfaces on that window from the app's own threads, composed under the views as those of EGL from Java are, and `ANativeWindow_lock`; a `SurfaceTexture`'s `Surface` likewise, its frames handed to the texture (docs/JAVA.md, "A SurfaceView drawn by native code") | **passes under qemu** (the test app `natives`, 31 checks, with the shell composing the window and with the core: the window's size and format, an EGL surface on it from a thread never attached to Java, its frames in the view's place the right way up, the surface resized by `setFixedSize` and by the geometry at the next swap, the window no longer valid and the swap refused after `surfaceDestroyed`, a new window when the surface comes back, a software buffer in RGB 565 scaled into its view; a `TextureView`'s texture drawn in software and with EGL). **Not tried on a phone**; Asphalt 9 is the app it was made for (its log: `java: ANativeWindow_fromSurface: a window of`, `egl: window surface ... on a view's window`, `java: OpenGL frames`) |
| Sound out of the phone (XAudio2) | **heard on the phone** with build 0.1.0.21, the Probe's tone and the demo's; XAudio2 played every frame it was given. With 0.1.0.19 nothing was heard and the log could not say why: the engine was built on the app's own thread then, and nothing was counted |
| A Java app's sound with Megler minimised, and the phone's media controls (lock screen, volume flyout) for its media session (docs/JAVA.md, "Sound in the background") | **passes under qemu** as far as the core (the data app's media session: its state and metadata told, the media keys to its callback). **Not yet heard on the phone** in the background |
| Android's accent colours from the phone's accent, and its light or dark theme; Material You's system colours in HCT (docs/JAVA.md, "The phone's accent and theme") | **passes under qemu** (the data app: DeviceDefault's `colorAccent` and `system_accent1_600` from a cobalt accent) and **on PC** (the palettes against Material 3's baseline colours). **Not seen on a phone** in an app |
| APKs, .apks and .xapk opened in Megler from the phone's file manager; an Android app's install intent or `PackageInstaller` session handed to Megler's installer (docs/JAVA.md, "Installing, and the phone's other apps") | **passes under qemu** (the data app: an APK by `ACTION_VIEW`; a session of one file, handed over by its package's name, which the user says no to, and its `STATUS_FAILURE_ABORTED`; a session of a base and a split handed over as one `.apks` of both, and its `STATUS_SUCCESS` once the phone's apps have it); the shell's side is built. On the phone (0.1.0.457) Google Play's download reached Megler, but under a name Megler's link refused, and Google Play, told of success at once, reported a failed install; both are fixed since, **not yet tried again** |
| Jobs (`JobScheduler`): run once their minimum latency has passed, by their service's `onStartJob`, ended by `jobFinished` or the end of their work, again after a backoff or an interval (docs/JAVA.md, "Jobs") | **passes under qemu** (the data app: a job with extras and a latency, and one given work twice, which it takes in order). **Not yet seen on a phone** |
| An app's other processes (`android:process`: Play Store's `:download_service`) as class scopes of the app's own APK, each with its Application and its process's name (docs/JAVA.md, "Jobs") | **passes under qemu** (the data app: a service in `:elsewhere` with statics of its own and that name, reached through `transact`). **Not yet seen on a phone** |
| Methods an app hooks through Xposed's XposedBridge, as Aliucord's Aliuhook declares its natives (whose LSPlant needs ART): the runtime's own hooks, every call of a hooked method -- interpreted, compiled, through reflection or JNI -- going to the app's callback and a backup running the original; the library loaded without its `JNI_OnLoad` (docs/JAVA.md, "Methods an app hooks") | **passes under qemu and on PC** (`test_hooks` on libcore: instance, static and constructor hooks, a default and a native method, from bytecode, from loops compiled before the first hook and after it, through reflection and JNI, arguments and results changed, exceptions, unhooking, two threads, a hook only the runtime keeps across collections; under qemu a stand-in for libaliuhook.so loaded with none of its functions called). **Not tried on a phone**: Aliucord is the app it was made for |
| Launchers: the apps of Megler's library listed to a Java app (`PackageManager`, `LauncherApps`, icons) and started by it, through their own packages or Megler; every activity of theirs, with its label, icon and filters (`megler_apk_describe_activities`), for a shortcut maker | **passes under qemu** (the data app: one other app listed with its label and icon, and started by its launch intent; Nova Launcher 8.9.4 starts to its welcome screen at the phone's size). **On the phone** Nova ended at its start with package 373 (docs/JAVA.md, "Real apps"); fixed under qemu, not yet tried there again |
| The screen resized and turned (`megler_set_surface`, `megler_display_host`): a Java app's configuration change -- `onConfigurationChanged` for an activity whose `android:configChanges` takes it, a relaunch with its saved state (its views' too) and retained object for the others, none for a size that crosses no size its resources are made for -- its windows laid out at the new size, `Display.getRotation`, the activity's wish of orientation to the shell; a game's window made again at the new size; the player turning the phone as the app asks and resizing the surface with a PC's window (docs/JAVA.md, "The screen resized and turned") | **passes under qemu** (the test app `screen`, 63 checks: turned, narrowed within and across a size of its resources, an activity that takes the change, one relaunched as it comes back, the orientations asked for). **Not tried on a phone or a PC yet** |
| ANGLE: its DLLs, a display, a context | **works on the phone**: Direct3D 11 at feature level 11_1, EGL 1.5, OpenGL ES 3.0; 16384-pixel textures, DXT, depth textures, NPOT; no ETC1, no ASTC |
| A window surface over the shell's texture, the copy to the swap chain, the picture on screen | **works on the phone**: the right way up, the right colours, 1280x720 scaled to a 2560x1440 screen, 59 frames a second paced by `Present(1)` |
| Touch | **works on the phone**: five fingers at once, about a hundred events a second for each |
| App shell (library, app page, player with swap chain and XAudio2 output, device checks, log) | **runs on the phone**: adding an app, its page, the player in landscape, the checks |
| Package | **installs and starts on the phone**: `Megler.exe` imports only api-sets of 1703 and `bcrypt.dll`; ANGLE with its runtime and forwarders staged by gecko-w10m's own scripts |

208 checks under qemu and on the phone (build 0.1.0.28), none failing.

## What the phone taught

- When a vectored exception handler changes `Pc`, the thread's state on resume is
  taken from **bit 0 of `Pc`**, not from the T bit in `Cpsr`. Resuming Thumb code at
  an even address runs it as ARM code. (Linux wants the opposite: an even `pc` and
  the T bit.) `mg_fault_resume_at` is the one place that knows; both crashes of
  the first build on a phone were this.
- The `Pc` an exception reports for Thumb code has bit 0 set as well
  (`0x05420091`, `Cpsr` `0x80000030`).
- `TPIDRURO` holds the processor's number and `TPIDRURW` the TEB.
- `LoadPackagedLibrary` takes ANGLE's DLLs with no forwarders for
  `kernel32.dll`/`ole32.dll`.
- Touches come from another thread on another core, and a race that qemu never
  showed was hit within seconds: the input queue counted an event in its
  descriptor after queueing it, an app thread already taking events took it in
  between, and the descriptor stayed readable with nothing behind it. The demo,
  which polls without waiting like any game, then went round its event loop for
  ever: no frames, one core burning, touches still answered. The queue and its
  descriptor now change under one lock, moves that have not been taken are brought
  up to date instead of piling up, and a test with two threads (`input queue`)
  fails on the old code on this PC.
- A sleep of one millisecond took sixteen: Windows ends a wait on the next tick of
  its timer, 15.6 ms apart unless a process asks for finer, and Android code sleeps
  a millisecond at a time everywhere. `NtSetTimerResolution` (what winmm's
  `timeBeginPeriod` calls; winmm is not there for an app, ntdll is) is asked for
  1 ms at the first sleep, and the phone grants it.
- ANGLE does not list ETC1 there, the format most older Android games ship their
  textures in, so it would refuse them. Nothing is done about it yet. OpenGL ES 3.0
  requires ETC2, of which ETC1 is a subset, so handing ETC1 data to an ES 3 context
  as ETC2 is the first thing to try.

## Not known until it runs on a phone

The demo (`core/tests/guest/t_demo.c`, in the package as **Add the Demo** on an
empty library) is made to answer these by eye and ear, and logs what it was given.
What it has answered is in the table above. What is left:

1. That the picture keeps its 59 frames a second with fingers on the screen, now
   that the input queue is fixed (0.1.0.22; shown by the test, not yet on the phone).
2. Whether a touch lands exactly under the finger (nothing said it does not).
3. Any real app. Everything seen so far is this project's own test code.

## Unity

Begun, on a real game: an unofficial Android build of Hollow Knight (Unity 2020.2,
IL2CPP, with armeabi-v7a libraries). Under qemu, with the stand-in for OpenGL ES:

- `libmain.so`, `libunity.so` (17 MB) and `libil2cpp.so` (30 MB, a fifth of its
  functions in ARM mode) load with every import provided, and their constructors run;
- the driver (`core/src/drivers/mg_unity.c`) does what `UnityPlayer` does: the
  loader's `JNI_OnLoad`, `NativeLoader.load`, `initJni`, the surface, resume, focus,
  and `nativeRender` in a loop;
- IL2CPP starts, and its garbage collector stops and restarts the world. That needed
  signals between threads (`pthread_kill`, `sigsuspend`), which are new: a thread is
  interrupted in the middle of guest code, or woken where it waits. The test
  `t_signals` stops three threads twenty times, in Thumb and in ARM code;
- the engine reaches its graphics set-up and, finding the stand-in, says "Your
  device does not match the hardware requirements". That is as far as qemu goes.

On the phone, the signals work (build 0.1.0.28): `t_signals` passes there in Thumb
and in ARM code. A thread in the middle of guest code is stopped with
`SuspendThread`, sent to a function of this layer with `SetThreadContext`, and put
back exactly where it was through the vectored handler; a thread that waits or
sleeps is woken instead. (The first build to try it hung: a thread's handle was
closed as soon as the thread was made, since nothing joins a guest thread through
it, so there was nothing to stop it by. Each thread now keeps a handle of its own
for as long as it runs.)

**Hollow Knight is played on the phone** (build 0.1.0.68, 2026-10-13): the engine
and IL2CPP start, ANGLE takes the game's graphics (an ES 3 context, a 565
configuration turned into an 8888 one, no complaint about the hardware), the game
loads for forty seconds, and from its menus on it answers the finger: language,
main menu at 58 frames a second, options, save slots, a new game, its opening
verses, and then King's Pass itself, with the port's on-screen stick and buttons,
two fingers at once, the knight walking and striking, geo counted. The pictures
are right (seen in screenshots the shell takes: below). About 1100 MB in use,
690 MB of addresses free. **Sound** and the two **opening films** came with build
0.1.0.80 (both below), and the phone's owner has since heard and watched them
(builds 0.1.0.90 to 0.1.0.103): the sound is right, the films fill the screen
(they look coarse because the port's own files are 360x202 at 64 kbit/s), and a
film skipped with a tap goes on to the game since 0.1.0.98 (what it took is
under "Films" below). What it took, beyond what qemu had shown:

- Touches as the engine wants them: it does not read an event inside
  `nativeInjectEvent`, it asks for a copy (`MotionEvent.obtain(event)`) and reads
  that later. Every event is an object with its own contents now, from a ring.
  (With one object refilled for each call and no `obtain`, the first build that
  drew the menu ignored every tap.)
- Settings that are kept: `SharedPreferences` (Unity's PlayerPrefs) are files of
  the app's now (`mg_jni_prefs.c`), read back through `getAll()` as Unity does
  when it starts. Without them the game could not be begun at all: a new game's
  first time goes by way of a page of picture settings (the game's port has its
  "Select Graphics Tier" there, whose Back leads to the options, not on) and then
  one of brightness, each remembered as done, and a game that forgets asks for the
  first one for ever. With them the second try brings the brightness page, and
  its Done starts the game.
- `new String(bytes)`: native code makes its Java strings this way when it does
  not trust `NewStringUTF` with its text, Unity for every PlayerPrefs key and
  value. A `java.lang.String` constructor that did nothing left them all empty:
  one value under the key "", and a saved language read back as "".
- A thread's stack made whole before Android code runs on it (below).
- Unity's frame pacing: message loops and Java proxies (below).
- **Room.** A 32-bit process has 2 GB of addresses here, whatever the phone's
  memory (on Android the same 32-bit game has about 4 GB), and three things were
  eating them:
  - `munmap` gave no addresses back when a block was unmapped in two pieces,
    which is how Unity's allocator frees every large block (it maps a little more
    than it wants and unmaps the end first). Half a minute in, 755 MB of addresses
    were reserved and unusable. A region now knows which of its pages the app
    still has and goes when the last one does (`mg_libc_mem.c`; the test
    `t_map_pieces` maps and unmaps 4 GB that way).
  - The APK, 462 MB, was mapped whole. It is read where needed now (`mg_zip.c`).
  - The game's textures reach OpenGL as plain RGBA8: Unity decodes them itself
    from a format this OpenGL does not offer (almost certainly ASTC; ANGLE over
    Direct3D 11 has none), a few bits a pixel in the APK and thirty-two here, 930 MB
    of them by the time the game is up. `mg_gltex.c` counts what is asked for and,
    once an app has asked for 160 MB, keeps large RGBA8 textures that are only ever
    filled and sampled as DXT5, compressing the pixels as they are uploaded: 661 MB
    became 165. It costs some of the picture and nothing has judged how much.
    Flags `plain-textures` and `pack-textures` turn it off and on from the start.
    And while the process has less than 768 MB of addresses free, a texture of
    1024x1024 or more is packed whatever was asked for before, glTexImage2D's too:
    BC1 when its first level is opaque, else DXT5, its mipmaps made here (Yandex
    Maps' panorama of 2048x2048 textures ended in GL_OUT_OF_MEMORY, packages
    540 and 543).
  - (Asphalt 9, package 278.) A game made for OpenGL ES 3 ships ETC2, which
    Direct3D does not have: ANGLE decodes it to RGBA8 and keeps it so, 112 MB
    of ETC2 came to 507 MB, and the addresses ran out in the first race (231 MB
    left, the largest piece 17 MB; Direct3D's driver threw, and the game wrote
    through the null it got). ETC textures now reach ANGLE in a format Direct3D
    has, of the same size (`mg_gltex.c`): the RGB ones as ANGLE's own
    `GL_ANGLE_lossy_etc_decode` formats, which ANGLE turns into BC1 as it loads
    them; RGBA8 ETC2 EAC, which that extension does not offer over Direct3D, as
    DXT5, each block decoded here and packed again (the decoder is checked
    against ANGLE's own over 65536 blocks). A texture whose base level is not
    whole blocks stays as the app gave it. Flag `etc-by-angle` gives every ETC
    texture to ANGLE as the app gave it.
  - (Asphalt 9, package 281, with `trace-cpu`'s memory figures.) At the same
    size it still ran out in its first race: 1238 MB of 2048 in use, 254 MB of
    textures held, 507 MB of graphics memory, and 442 MB of addresses free in
    pieces of 31 MB at most; Direct3D could not make a texture (the road and
    buildings untextured) and then a vertex buffer's mapping, whose null the
    game wrote through. ANGLE over Direct3D 11 keeps a staging copy of a buffer
    to map or update it and of a compressed texture until it is first drawn,
    all in the same addresses. So an ETC texture of 1024 pixels a side or more
    at its base now goes at half its size, a quarter of the memory: each level
    decoded, each 2x2 averaged, packed as BC1 or DXT5, its level numbers kept
    (`mg_gltex.c`; flag `full-textures` keeps them whole). The RGB decoders are
    checked against ANGLE's own as the RGBA one is.

Unity's frame pacing is `core/src/jni/mg_jni_loop.c`: Android's `Looper`,
`Handler`, `HandlerThread`, `Message` and `Choreographer`, the boxed numbers, and
the Java proxies native code implements interfaces with (`JNIBridge`: calling one
is calling the library's own `JNIBridge.invoke` with a `java.lang.reflect.Method`
and the arguments boxed). Each loop is a thread; a Runnable or a Handler that is
one of the app's own Java classes is noted and dropped, since no Java runs.

Builds 0.1.0.29 to 0.1.0.47 all died 0.7 s into the first scene, and the log
blamed the wrong thing for a day. What it was, since the same will happen to any
engine:

- **A thread's stack has to be whole before Android code runs on it.** Windows
  hands a stack out a page at a time: the page below the used part is a guard, and
  touching it brings the next. Code built for Windows feels its way down (a frame
  bigger than a page probes every page in turn). Android code does not: a function
  with a 300 KB array moves `sp` and writes. That lands beyond the guard page, in
  memory not yet given, and the fault cannot be delivered, because the stack it
  would be delivered on is the one that is missing. Windows ends the process: no
  vectored handler, no unhandled-exception filter, nothing. Unity's texture decoder
  did it on the graphics thread. `mg_thread_whole_stack` now commits the whole
  stack (down to a guard page of its own) the first time a thread is given a guest
  thread pointer, whoever made the thread; the test `t_big_frame` writes a 300 KB
  frame bottom first, on the calling thread and on a new one.
- **What the log showed instead** was a fault of the shell's UI thread inside
  `CoreUIComponents.dll`, a tenth of a second later. That was the phone taking the
  window away from a process already being reported as crashed (`WerFault.exe`
  was running by then), and the system's `NavigationClient` falling over the
  unexpected order of events (hidden while still active) in what was left of the
  process. A fault on a thread that has never run Android code, in a system DLL,
  with nothing of this layer on its stack, should be read as "something else
  already ended this process". One such something else is the phone's own
  hang report, for a UI thread that does not answer: no dump, and the window
  taken away (Instagram signing in with Google, package 567). The shell watches
  its UI thread (`ui/AppShell.cpp`, `megler_thread_where`): one that has not
  answered a ping in two, five and ten seconds is said in the log with where it
  stands, and how late it answered when it does.
- **What found it** was the phone's own event tracing, relayed by the Device
  Portal: `tools/phone-etw.py` records any ETW provider registered on the phone
  while Megler runs. `Microsoft-WindowsPhone-CoreUIComponents` and
  `Microsoft-Windows-AppModel-Exec` showed the shell closing the app's task for an
  error report before the UI thread's fault; `Microsoft-Windows-Kernel-Process`
  showed `services.exe` and then Megler itself starting helper processes 120 ms
  before it.
- Ruled out on the way, each by a run on the phone: the swap chain on the XAML
  panel, interrupting threads for the collector, thread priorities, Windows Runtime
  calls from the app's threads, the thread that set ANGLE up ending, memory,
  address space, graphics memory, a removed Direct3D device.

The host layer now says something about every way a process ends that it can see:
an exception it does not repair (code, module and offset, `lr`), a C++ exception
when thrown (with the callers found on the stack), `abort()`, an argument the C
runtime refuses (which no longer ends the process: the call fails, as on Android),
a pure virtual call, a stack overflow (reported from a spare stack), and a last
line from the unhandled-exception filter. A call through a null pointer used to be
dropped by the fault handler without a word; it is a fault like any other now.

Seeing a phone's screen from the PC: an auto run (`tools/build-appx.sh arm --auto
play`) with the flag `shots` writes the app's picture to `LocalState` every five
seconds, and `python tools/phone.py shots` fetches them as PNGs; flags named
`tap-S-X-Y` make it put a finger down at second S at X,Y of a 1280x720 picture
fitted into the app's surface and centred (whatever size the game has since made
it) and take a picture two seconds later, `key-S-C` press the key with Android's
code C, `play-N` run for N seconds. A phone that has locked itself runs what the
Device Portal launches under its lock screen: the window is never activated, the
log says "background execution denied", and the screen does not turn, so a
landscape game gets an upright surface and draws itself small in the middle of it
(and Hollow Knight squeezes its films into a strip there). The flag `as-landscape`
gives the app the landscape surface all the same, for the pictures' sake. `python tools/phone-flags.py only NAME...` sets them (and `clear`
removes them); `tools/phone-tree.py` lists a folder of the app's on the phone. (Only the names of flag files are read: what the
Device Portal puts in `LocalState` cannot be opened by the app.) The driver logs
every finger that goes down or comes up, so a log tells a scripted tap (finger 1)
from the owner's own (finger 0) when both are at the phone.
`python tools/phone-get.py PATH` prints one of the app's files.

Tools made for this, all still there: the trace (`mg_trace`: a ring of the last
4096 calls of note, printed by the crash report), the crash report's module names,
stack scan, thread and memory lines, the flags (`mg_flag`: a file's presence in
`LocalState\flags`, put there through the Device Portal: `no-interrupt`,
`no-render`, `late-render`, `no-egl`, `no-panel`, `early-present`, `no-present`,
`fail-present` (every Present taken as refused, uncalled: docs/XBOX.md),
`normal-priority`, `trace-gl`), `trace-gl` (every GL call through a generated
thunk), `no-gl-turns` (the thunks that have two threads' calls of one ANGLE
display take turns left out; `ndk/mg_egl.c`, "turns"), the unattended `play` mode, and `tools/phone-etw.py`. And for what an app
does that it says nothing of: `trace-net` (every TCP connection the app makes,
where to, how it ended and how long it took, and every name it looks up with its
addresses; `bionic/mg_net.c`, `mg_netdb.c`) and `trace-exceptions` (every Java
exception thrown, once, with its message and the method it was thrown in, caught
or not; `dvm/mg_dvm_interp.c`), at most 1000 and 2000 lines a run; and
`trace-stacks` (five seconds in, every Java thread's stack, its 24 top frames;
then every two seconds those whose stack changed, the others counted; threads as
they end, and any that could not be stopped; each thread stopped only while its
frames are copied; at most 6000 lines; `dvm/mg_dvm_object.c`); and
`trace-nulls` (at a NullPointerException an instruction of the app's bytecode throws, once a place, which register
was null and where the method last gave it its value: which call's result, which field, the constant null, a
parameter; at most 320 lines; `dvm/mg_dvm_interp.c`); and
`trace-calls-<prefix>`, the prefix a class name with dots (every call of a method
of a class whose name starts with it, with its thread and depth, and its return
with the value or the exception it ends with; up to eight such flags; at most 8000
lines; `dvm/mg_dvm_interp.c`, the flags read by `mg_flags_each`); and
`trace-calls-within-<prefix>` (every call made while a method of such a class
runs, whatever the callee's class, up to three frames below the nearest such
method, and its return: what the method asks of the framework and is told,
without tracing the framework's classes for the whole run; the same lines and
the same 8000); and
`trace-cpu` (every five seconds, the processor the app had in the phone's cores,
and its eight busiest threads that ran guest code, by id and name, each as a
share of one core: whether a slow app is held by one thread; at most 2000 lines;
`bionic/mg_bionic.c`); and `trace-profile` (every second the three busiest
threads that ran guest code are chosen and each is sampled about sixty times a
second -- stopped while its pc and the top 2 KB of its stack are copied; a
sample in the system's own code is a wait, put to the first return address on
the stack outside the system and the layer; every ten seconds the log says each
thread's share of a core and its samples by module, running and waiting, and
the six addresses it ran at and waited for most, and `LocalState\profile.txt`
has every address with its count, which `tools/profile-names.py` names by
function with the build's maps and an app library's exported symbols: where a
game that is slow with the graphics processor idle spends its time; Windows
only; at most 1000 lines; `bionic/mg_bionic.c`); and `trace-views` (a Java app's
view tree as its window is laid out, at most every three seconds, and every four
seconds by a timer: each view's class, place and size, padding, translation, id,
text and layout parameters, indented by depth; what keeps content off a bar, or
twice off it, shows as padding; `megler/ViewRoot.java`, the flag read through
`megler.Native.flag`); and `malloc-guard` (64-bit processors: 64 bytes of 0xFD
after every block malloc gives, looked at when it is freed and each second while
it lives, and freed blocks kept poisoned before they are given back, 8192 of them:
a block written past its end, written after it was freed or freed twice is said,
with the code that asked for it and that freed it; Windows' own heap says only
c0000374 later, at another block; `bionic/mg_libc_mem.c`). Without any flag, on Windows, a
watchdog looks every two seconds at who holds the log's lock and the guest
threads' lock: one held by the same thread for ten seconds is an app that stands
still, whose core lines have stopped while the shell's go on (urent did, on the
page after asking for its SMS code). Once for each such stand-still it says who
holds which lock since when, the line that thread was writing or the function
that took the threads' lock (and the thread `trace-profile` was stopping), and
where every thread that runs guest code is, as `trace-native-stacks` says it:
first as bare addresses in `LocalState\hang.txt`, then named there and in the
log past its lock; at most five reports; `bionic/mg_bionic.c`. `trace-net` also says, when the app closes a connection,
what went over it: the bytes and calls each way, the first sixteen bytes each
way, when the first came in, the other end's close, errors and timeouts. Of a
connection made it says which call the app learnt it by (`getsockopt SO_ERROR`,
connect itself or asked again, a send, a receive), the first bytes each way as
they go, and every twenty seconds, for each connection still open whose counts
changed, the same counts with how long ago bytes last went each way: a
connection an app keeps open, and waits on, is otherwise never said. The
data app's second run under qemu has all of them on, and the test finds their lines.
For OpenGL ES drawn on a view (a GLSurfaceView, or native code's EGL surface on a
SurfaceView's window): `trace-surfaces` (every three seconds each window surface's
frame read back just before it is handed over -- its centre and a 4 x 4 grid of
pixels, R, G, B, A, rows from the top -- with where its view last said it is,
when, and who composes it; and the views' picture at the same grid over each
placed view, its alpha, `00` where the view's hole lets the frame through:
`java: trace-surfaces: ...` lines, `drivers/mg_java_gl.c`) and `gl-compose-here`
(the core composes the window from frames read back, as where EGL shares no
textures, instead of the shell on the graphics processor). A black frame, a frame
the views hide, and a frame the shell loses tell apart so: the first has a black
grid, the second alpha other than `00` over it, the third a good grid and alpha
`00` and a black screen that `gl-compose-here` mends. Without a flag the log says
each view's first six places (`java: OpenGL layer N (class) is at x,y wxh in the
window`), every EGL call of the app's for these surfaces that EGL refused with its
error (`egl: ... was refused (EGL error 0x...)`), and the OpenGL frames' minute
lines end with who composes and where each layer is, or that it is nowhere (its
view has not drawn its hole). The natives app runs under qemu with
`trace-surfaces` on, and the test checks its lines.

**Sound.** FMOD said "Error initializing output device" twice and made no player.
It had not got as far as OpenSL ES: before that it asks `AudioManager.getProperty`
for the output's sample rate and frames per buffer, under the names the constants
`AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE` and `..._FRAMES_PER_BUFFER` hold, and
parses the answers with `Integer.parseInt`. The constants were static fields
nobody had given a value, so the names were null, so were the answers. Three
things follow from that one: a table of Android's constants
(`android_constants` in `jni/mg_jni_android.c`: the ones engines read, by class),
the parsers (`jni/mg_jni_loop.c`), and a warning, once for each field, when a
static field is read that nothing has given a value -- a silent null is the kind
of thing that costs a day. With them FMOD makes its OpenSL ES player (44100 Hz,
stereo, 16 bits) and the game is heard: the flag `record-audio` writes half a
minute of the mixer's output to `megler-audio.wav`, and what came back was music
(tonal, at musical pitches, 0.01% of it clipped, no gaps).

**Films.** Unity's VideoPlayer wants `libmediandk.so` (and falls back to the Java
`MediaExtractor`, which is not there). `ndk/mg_media.c` is that library with no
decoder in it: the extractor copies the film out of the APK to a file and has the
shell open it (`megler_media_host` in `megler.h`), hands the engine a ticket for each
sample, and the codec, given a ticket, asks the shell for the next picture or
block of sound. The shell (`player/MediaHost.cpp`) decodes with Media Foundation's
source reader, in software, on a thread of its own: RGB32 pictures and 16-bit PCM.
Pictures go where Android puts them: the engine makes a `SurfaceTexture` on an
external texture of its own, and `updateTexImage` uploads the newest picture to an
ordinary texture and makes the external one an EGLImage of it (ANGLE has
`GL_OES_EGL_image_external`), with the transform that turns it the right way up.
Hollow Knight's opening is two such films, H.264 at 360x202 with AAC at 48 kHz,
30 and 38 seconds: every picture of both is decoded (906 and 1143), they fill the
screen, the right colours, and each ends when its length has passed.

What the first try got wrong is worth keeping. All of a film's sound was played
in its first four seconds, and the pictures skipped ahead after it. Unity feeds
both decoders in the order the file has the samples, takes sound as fast as it is
decoded and pictures as their times come; nothing in that paces the sound except
the picture decoder being *full* -- a real one has a handful of input buffers.
This one took everything it was offered. It now refuses input once eight pictures
(sixteen blocks of sound) are queued that nobody has asked the output for. With
that, the mixer's half minute from the start of the first film (the flag
`record-film`) matches the film's own sound track, decoded on the PC by ffmpeg,
with a correlation of 0.97 between the waveforms, a sixth of a second behind --
which is also where the first picture was.

Skipping a film took three builds, each wrong in a way the last one's log
showed. Unity ends a decoder's thread like this: it posts one last message to
the thread, whose handler quits the thread's loop from inside with
`message.getTarget().getLooper().quit()`; it calls `HandlerThread.join()`; and
then it deletes the handler's native object. Here `join` was a stub that
returned at once and `Message.getTarget` was not there at all. A film that
played to its end never showed it (the thread was idle when its handler was
deleted, and simply never ended). A film skipped in the middle did: the thread
called a handler that was being deleted -- a jump to nowhere the first time,
"Pure virtual function called!" the second. With a `join` that waits, the game
stood still for good, because the loop was never told to quit; the log's line
just above said why (`Message.getTarget ... is not implemented`). Now both are
there, a call through one of Unity's interface proxies and its being disabled
exclude each other (as they do in the engine's own Java), and a `join` on a
loop that is idle and was never told to quit ends it after a second and says
so. The test `test_proxies` plays the three endings. The rule drawn from it: a
Java method whose whole job is to *wait* (`join`, `await`, `acquire`) must
never be answered with "returns at once".

Touches and keys for the engine go through `nativeInjectEvent`: touches work
(two fingers at once in play), keys reach the engine but did
nothing in the one menu tried (`KeyCharacterMap` is not there). Not there (when
this was tried; sound through `AudioTrack` and decoders through `MediaCodec` are there now): films for an engine that asks Java (`MediaExtractor`,
`MediaCodec`, `MediaPlayer`) instead of `libmediandk.so`, films that are not
something Windows decodes (WebM/VP8, Ogg), and the listeners the engine registers
(display, input devices, system UI) are never called.

## Logs

Every package of Megler's, Megler and each app's own package, keeps its log in
its own `LocalState` (`megler.log`, the two runs before as `megler.prev.log` and
`megler.prev2.log`, `megler-boot.txt`, the checks' logs), which no other package
can read. Each also keeps copies of them in the shared folder,
`<shared>\logs\<its package identity name>\` (`shell/LogMirror.cpp`): at its
start (ten seconds in, out of the start's way) the runs before, whole; while it
runs, `megler.log`'s new lines every
30 s and as it is suspended or exits; the other files when they change, and
`flags.txt`, the names of its flags. A run that ended in a crash is there whole
from the package's next start, and the part before the crash at once. A log over
16 MB is copied as its last 16 MB, after a line that says so. Megler removes the
folder of a package that is no longer installed whenever it lists the apps'
packages.

**The start** (`App.cpp`, `shell/Startup.cpp`) says each of its steps before it
begins, the log written out with it (`start: reading the library`), and until
the window is shown a watch says at 2, 5, 10, 15, 20, 30, 45 and 60 s which step
the start is on and where the UI thread stands (`megler_thread_where`);
`start: the window is shown, N s after the launch` ends it. Windows ends an app
whose window it has not been shown in time and tells the app nothing, so the last
`start:` line is the step it was ended in, and the watch's last line how long it
lived. A step that throws is said as `FATAL` with its exception and the start
goes on (a page that says why where the screens could not be made), and
`std::terminate` on the UI thread is said with its exception. What the core says
of a process about to end (`host: abort() was called`, an exception nothing
handled, a pure call) is written out at once, as a fault's report is; it waited
for the writer before, and went with the process. The start's lines say too what
the phone is (`phone:` its maker and model as Windows names them, its memory and
how much is free) and the room on the disks Megler writes to (`disk:`), and the
log says each step of the app's use of memory toward its limit as Windows counts
it (`memory: Windows counts the app's use as high now`). **Passes on PC**:
`mg_shell_tests.exe` (`tools/build-tests.sh win`). Made for a Lumia 630 whose
Megler 0.1.0.671 closed at every start with its splash screen up, the log ending
a second in, with nothing of what the UI thread was doing (it was copying the
package's Google APKs: [PER-APP-PACKAGES.md](PER-APP-PACKAGES.md), "The Google
APKs Megler brings").

**Save All Logs** (the Log screen, in Megler and in an app's own package,
`shell/LogArchive.cpp`) writes one ZIP, `megler-logs-<yyyyMMdd-HHmmss>.zip`, to
`Pictures\Megler\logs`, which the phone's file manager shows and a PC reaches over
USB (on a PC, the user's Pictures); Share sends it from the phone itself. In it:

- `summary.txt`: Megler's and the core's versions, the processor, the device
  family and model, Windows's version, memory, whether Android code can run,
  ANGLE, the packages of Megler's publisher as Windows lists them, free space, the
  time with its offset from UTC, the last install Windows refused
  (`LocalState\install-refusal.txt`), and what the ZIP holds;
- `<identity>/`: the files of the package that saves it, from its own LocalState;
- `<identity>/` of every other package: its copies from the shared folder, with
  `package.txt` (what it is) and `flags.txt`;
- `shared/settings.ini`, `shared/apps/<package>/settings.ini`: the settings every
  package reads.

Where the Pictures folder cannot be had (an Xbox; an app's own package that does
not declare it) the ZIP stays in `LocalState\logs`, the newest three kept, and the
Log screen says so. An app's own package made by an older Megler copies nothing
until Update All makes it again. The shell writes the ZIP itself
(`shell/ZipWriter.cpp`, zlib's deflate). **Passes on PC**, the very files with
stand-ins for the rest of the shell: `tools/zip-writer-test.sh` (Python's zipfile
and Explorer read the writer's ZIP whole), `tools/log-mirror-test.sh` (29 checks:
a crashed run's start, new lines, changed and vanished files, a log over 16 MB,
the folder taken away, the removal), `tools/log-archive-test.sh` (Save All Logs as
an Xbox's Megler, its ZIP read by zipfile). **Not tried on a phone yet.**

## Not there yet

- **Drivers** other than NativeActivity: Unity, Unreal, Godot (cocos2d-x's and
  SDL's activities are Java ones, and run as the app's Java code). This
  is what stands between the layer and most games.
- **Java.** Apps with no native engine cannot run yet. What there is
  ([JAVA.md](JAVA.md) has all of it, with the commands): a Dalvik interpreter
  (`core/src/dvm`), joined to the JNI layer so that bytecode and guest native
  code call each other with one kind of object, and Android 9's own class
  library under it -- libcore's bytecode from Google's emulator image
  (`tools/android9-unpack.sh`, `vdex-dex.py`, `android9-boot.sh`) and libcore's
  native libraries compiled from AOSP's source for plain ARMv7
  (`tools/android9-sources.sh`, `android9-natives.sh`: no image of Android 9
  that Google publishes has 32-bit ones). Under qemu a Java program compiled by
  javac and d8 runs on it -- collections, streams, regex, formats, files,
  threads -- and prints what the JVM prints for the same classes, through the
  real `System.out` and libopenjdk's native write (`tools/build-tests.sh arm`:
  2861 checks on the runtime's built-in java.lang, 2896 on libcore). On the PC
  the same without the native libraries (2673 and 2685).
  **On the phone** (2026-10-13, builds 0.1.0.90 and 0.1.0.103, run by its
  owner): the Self-test with libcore in the package passes, 3595 checks, libcore
  starting in 167 ms; and two test apps that are Java alone
  (`core/tests/app`, packaged by `tools/build-appx.sh arm --java`) run: an
  Activity with Android 9's action bar, a text, a button and a view that draws
  on a Canvas, 0.7 s from Play to its first picture, 33 presses each answered;
  and a list whose rows try preferences, a delayed message, an AsyncTask, a
  second Activity with a result and a toast, all as on Android. They run on the
  Java framework in `core/framework` (built by
  `tools/build-framework.sh`) and are drawn by the shell through
  `megler_gfx_host` (`app/Megler/player/GfxHost.cpp`: Direct2D straight into
  the app's bitmaps, DirectWrite, WIC; `tools/build-tests.sh win` checks it on
  a PC), shown through `present_pixels`.
  With build 0.1.0.110 (driven from the PC with scripted taps, pictures taken
  by the shell): apps are held upright unless their manifest asks for
  landscape (the APK report has `orientation`), at the density an Android
  phone of this size would report; the second app's whole list was gone
  through twice -- a dialog and its button, a second Activity and its button,
  a toast, then single rows again -- and every touch was answered. The shell's
  drawing was checked on the phone by itself too
  (`tools/build-appx.sh arm --auto gfx`): 260 checks, none failed, shapes'
  insides equal to the stand-in's pixel for pixel, and text right in Latin,
  Cyrillic, Chinese, Japanese, Korean and colour emoji.

  **On the graphics processor** (2026-10-14; docs/ARCHITECTURE.md, "A Java
  app's views, on the graphics processor"): Direct2D's software rasterizer
  cost 18-20 ms of drawing for a caret's frame on the phone, so the shell now
  draws through Direct2D 1.1 on a hardware Direct3D 11 device (the flag file
  `gfx-software` brings the old way back), the host's own arithmetic on
  Direct2D's effects. Seen on the phone (build 0.1.0.202, Apktool M): the
  Adreno 530 found, drawing itself 1.4-2.7 ms a frame, but every read-back of
  a canvas into memory 10-17 ms whatever its size -- so the window's pictures
  are now shown straight from the processor (`megler_gfx_host.present`), and a
  window with OpenGL ES views is composed there too, from ANGLE's surfaces
  shared by handle (`gl_frame`, `gl_place`; seen with AIDA64 on build 0.1.0.204
  still composed in memory, the way builds before 0.1.0.205 take). On the PC
  (`tools/build-tests.sh win`): the scenes on the RTX 3090 and on WARP, 294
  checks each, the eighteen modes within a step of the table, nothing read back
  for the window. Not yet seen on the phone: 0.1.0.203's window path and
  0.1.0.205's GL composition.

  Four faults found on the way were the framework's own, each fixed in a copy
  of one file under `core/framework-overlay` (the framework's own files in
  `core/framework` have them still): a list that no longer answers a finger once its Activity has been
  covered and shown again (`AbsListView`, re-attached, never laid out); an
  `AlertDialog` that took 14 seconds to appear on the phone, because the first
  look-up of a resource by name walked all 161,000 entries of Android 9's
  resource table and the items of a style were searched one by one
  (`ResTable`, `Resources`: 710 million instructions down to 3 million, found
  with the interpreter's profile, `mg_dvm_profile`); a pressed row's ripple
  drawn as a disc over its neighbours (`ResInflate`, `RippleDrawable`: the
  mask that bounds it was dropped); and `MimeTypeMap` using a class Android 9
  does not have. The platform's resources are now cut to two languages and
  two densities when the Android root is made (`tools/dvm-android-root.sh`:
  43 MB to 12, and 28 MB less held by every app).

  **Real apps** (2026-10-14, the phone's owner added three; builds 0.1.0.110
  to 0.1.0.130). An app with bytecode *and* native libraries is an ordinary
  Java app now: `System.loadLibrary` loads the library out of the APK with
  the layer's linker and its native methods are bound to the bytecode
  (`core/tests/app/natives`, a test app with a library built by the NDK,
  goes through 28 steps of JNI and the NDK); the app's libraries and the
  runtime's own never see each other.
  - *QuickShortcutMaker 2.5.0* (Java only, ActionBarSherlock): runs on the
    phone -- its tabs, its settings; its second start, which ended on a
    missing native of Java serialization, works again. Its lists of other
    apps are empty: there are no other apps.
  - *Apktool M 2.4.0* (two native libraries): on the phone (0.1.0.128, by
    scripted taps) its licence is accepted, its two questions answered, and
    its drawer lists `/storage/emulated/0` -- twelve folders, its own
    `Apktool_M` among them -- and, `Music` tapped, the four files of the
    phone's Music library. The drawer had been empty because no button of a
    support-library dialog did anything (`Message.obtain(Message)` answered
    null), so the licence was never accepted; the files were not the cause.
    It lasts: four minutes of redrawing twenty times a second were run. The
    line under each name that says a size and a date shows "..." on the
    phone. Its Settings open and their switches are whole under qemu; on
    the phone that has not been looked at since.
  - *Device Info 3.4.3.4* (Kotlin, AndroidX, Material, two native libraries;
    its target is Android 15): runs on the phone since 0.1.0.122, the build
    with the WebP decoder -- its dashboard with the memory chart, the
    processor's cores, the display. Since 0.1.0.129 the selected tab has its
    pill, the storage card the phone's real room (59.6 GB, 34.1 free) and the
    battery card the phone's battery (100 % on the cable; the voltage and
    the temperature are made up, Windows saying neither). Its GPU (on its
    CPU tab) is asked through EGL14 and GLES20 and shown, under qemu with
    the stand-in's strings; not yet on the phone.
  Also since then: databases (`android.database.sqlite` on SQLite compiled
  into the core over the host layer's files, `tools/sqlite-fetch.sh`); WebP
  (libwebp's decoder in the core, `tools/webp-fetch.sh`: Windows 10 Mobile
  has none); the touch keyboard (asked for by the app through
  `megler_keyboard_host`; the shell brings Windows' keyboard up over a text box
  nobody sees and tells the app what was typed -- the request has been seen
  to arrive on the phone, the keyboard itself and typing have not, the phone
  having been locked); an app that ends on an uncaught exception says which,
  in one sentence.
  **Storage**, measured on the phone: with the library capabilities declared,
  the plain file functions are still refused in every library folder, but
  Windows.Storage's broker hands out real file and folder handles in Music,
  Pictures and Videos; Documents, Downloads and a memory card can only be
  given by the user picking a folder. On that the shared storage is built
  (the table above; docs/ARCHITECTURE.md, "Files and the shared storage"):
  one `/sdcard` for all apps in Megler's own folder, each app's `Android/data`
  and `obb` where they were (and, since 2026-10-18, there for the other apps
  too, as on Android 9: Total Commander puts the file Asphalt 9 reads in
  `Android/data/<its package>/files`), and `Music`, `Pictures`, `Movies`, `DCIM` and
  whatever the user adds in Settings being the phone's own folders. `statfs`
  answers the phone's real room (34.9 GB free of 56.7 on that phone).
  **One package, one entry** (2026-10-18): an APK or bundle added for a
  package the library already has replaces what was there once its report
  names the package, whatever its file's name, version or signature (none is
  compared: a debug build over a release one and back); the app's own files
  stay (`app/Megler/library/Library.cpp`, `analyse`).
  **Apps in several APKs** (2026-10-18; docs/JAVA.md, "Split APKs and
  bundles"): the library takes .apks and .xapk files beside plain APKs. Such
  a bundle is unpacked by the core into the base and the splits this phone
  can use, kept as `LocalState\apks\<name>.apk` and the folder
  `<name>.apk.splits` (the splits as `split_<name>.apk`, the bundle's icon,
  and the core's record of what it kept and left out); an .xapk's expansion
  files go straight into the app's own `Android/obb`. The report and the run
  take the base with the folder, so the app is one app: its native library
  out of its `config.armeabi_v7a` split, its resources merged from all its
  APKs, its assets and feature splits' code found, its split names and paths
  what Android would give. Under qemu and on the PC (the table above), not
  yet on the phone.
  **Text** is Android's own since 0.1.0.124: the fonts of Android 9's named
  families (Roboto, Roboto Condensed, Noto Serif, Droid Sans Mono and four
  more, 7.7 MB) are in the package's Android folder and the shell's drawing
  makes its faces of them, with the line measures Android reads out of a
  font (a line of Roboto is 23.4 for an em of 20; by DirectWrite's own
  measures it was 26.4, and Segoe UI before that 26.6). What the fonts lack
  -- other scripts, emoji -- is the phone's own fallback. A label that is
  still cut off is no longer the font's doing. QuickShortcutMaker's
  "APPLICATIONS" tab is cut off as Android's own rule has it for an app that
  targets Android 7 or later (four weighted tabs in a row that scrolls get
  equal shares of their summed widths: 187 each of 720); whether a real
  phone shows it so has not been compared.
  **The phone, for an app** (0.1.0.129): the shell tells the core the time
  zone as Windows names it (tzdata's names) and the battery with its
  changes (`megler_set_timezone`, `megler_set_battery`; `player/PhoneState.cpp`);
  a Java app's default zone is the phone's and a receiver of
  ACTION_BATTERY_CHANGED hears each change. The language is still English.
  **OpenGL ES from Java** (2026-10-14): the GLES classes' natives, EGL14
  and EGL10, a `GLSurfaceView`'s renderer on a GL thread of the layer's
  own, and the window composed in the core of the views over the frames read
  back at each swap (the table above; docs/JAVA.md, "OpenGL ES from Java").
  Under qemu AIDA64 lists the stand-in's vendor, renderer and version on its
  Display page and Device Info on its CPU tab. Not tried on a phone.
  **WebView** (2026-10-16; docs/JAVA.md, "The WebView"): `android.webkit`
  is AOSP's (Android 15's), and a `WebView` is Android's System WebView,
  Chromium 128.0.6613.88 for 32-bit ARM from AOSP's prebuilts, which the
  Android root carries where Android's image does and which runs in the
  app's process: its renderer on threads of the app's (single process), its
  network stack and TLS its own, its pages drawn in software into the view.
  Under qemu (the test app `webview`): a page from a server of the app's own
  on loopback is loaded and shown in the window, its script calls an object
  the app gave it, `evaluateJavascript` answers, cookies are kept and sent,
  a redirect to a callback URL of the app's scheme reaches
  `shouldOverrideUrlLoading`, as an OAuth login ends, and a tap on a field of
  the page brings the keyboard, whose text arrives in the field. On the
  phone (2026-10-17 and 18, packages 229 to 249) the test app passes the
  same, the WebView started in 0.8 s, Chromium's GPU thread on ANGLE; VK mp3
  mod and Kate show VK ID's "not a robot" sheet in their WebViews, but the
  check in it did not load ("something went wrong") while its canvases had
  no hardware buffers; those are there since (`ndk/mg_ahb.c`, with fence
  syncs), as is the WebView on a `RenderNode`'s canvas. Since (packages 256
  to 261) VK mp3 mod's sign-in has asked for a confirmation code instead,
  with a platform `DialogFragment`, which was a stub that showed nothing: a
  black screen; it is Android's now (docs/JAVA.md), and on the phone
  (package 262) VK's code dialog shows. VK now signs this app in only by a
  QR code, scanned with the old camera API (`android.hardware.Camera`, its
  own ZXing scanner on a `TextureView`): the phone's cameras are there for
  it now (docs/JAVA.md, "The cameras"). On the phone (package 267) the back
  camera opened without a prompt and streamed, ZXing read every picture, but
  the scanner showed white: the frame round the picture paints its corners
  over with a path toggled to inverse, which was taken as an ordinary path
  and painted over all of it. Inverse paths are drawn as Android draws them
  now (docs/JAVA.md, `Path.java`), and a view's canvas says the view's size
  while it draws, so that the frame's rounded corners and markers are its
  own (`View.java`); not yet tried on the phone. Asphalt 9 ended as it started
  (package 262): its window had focus, and lost it to its data installer,
  before the game it calls on a focus change was made; an activity that
  starts another for a result before it is shown now waits to be shown, as
  on Android (docs/JAVA.md, `android/app/Activity.java`). Its installer then
  said the download had failed: `WifiManager` said the Wi-Fi was off; it is
  on now (docs/JAVA.md, `android/net/wifi/WifiManager.java`). Past its
  installer it showed Gameloft's splash and then nothing (package 267): its
  engine draws from native code on its `SurfaceView`, and
  `ANativeWindow_fromSurface` gave NULL for the framework's `Surface`; a window
  of the view's surface, with EGL and software drawing on it, is there now
  (docs/JAVA.md, "A SurfaceView drawn by native code"). On the phone (package
  270) its engine had its window, made its surface on it and swapped 28 frames
  a second, all handed to the shell -- and the screen stayed black: ANGLE as
  gecko-w10m builds it shares every pbuffer's texture with a keyed mutex that
  nobody acquired, and Direct3D drops whatever is done with such a texture
  without it, the app's drawing and the shell's copy alike. The core now has
  ANGLE's device hold each pbuffer's mutex and lets go of it only while the
  shell copies a frame, which the shell does holding it (docs/ARCHITECTURE.md,
  "Keyed mutexes"). The same was true of every OpenGL ES view a Java app ever
  had here, none of which had been looked at on the phone. Not yet tried on
  the phone. Spotify's WebView
  then (packages 252 and 254) ended out of memory on an image of 126 MB that
  Chromium decodes and maps twice, in a 2 GB address space whose largest
  free stretch was 135 MB (docs/JAVA.md, "Out of memory"); the layer's
  changes for it (no shared memory view of the descriptor's own, the
  WebView's library at its own size) are not yet tried on a phone.
  **RenderNode and Picture** (2026-10-18; docs/JAVA.md, "Drawing"): display
  lists the core records and draws again with what each node has when it is
  drawn, as on Android, where each node had been a bitmap of its size, and
  Spotify, a Compose app, had ended with an `OutOfMemoryError` for one of
  1650 x 2560. Under qemu (the data app's `render-node` steps) nodes are
  drawn as their bitmaps were, pixel for pixel, children moved and made
  translucent without the parent recorded again, and 200 nodes of
  1440 x 2560 take 110 KB. On the phone (package 255, the same day) Spotify
  played, and showed its now-playing screen for a minute, with 14 to 44 KB
  of lists in up to 472 nodes. Its play button there was a white disc with
  no pause glyph. That is not the lists' doing: the button (the View form
  of Spotify's element, a disc drawing one glyph of its icon font, 224 px
  high under a scale of 0.46) is drawn straight onto the window, and on
  this PC the shell draws its glyph every way it draws, through a list and
  a node as well. Not known yet why the phone does not; the shell's scenes
  check a letter of that size under that scale for the phone to say.
  Not there: garbage collection (an app that animates lasts minutes); sound
  from Java; method
  handles; other apps (the
  package manager knows only the one running); Kotlin has run only as part of
  Device Info.
- **Sound from files.** OpenSL ES players fed PCM through a buffer queue, and AAudio output streams, are mixed; a player given a file or a URL (which would need MP3/Ogg decoding here) is refused. AAudio has no input streams.
- **The network on a phone.** On 2026-10-16 the HP Elite x3 passed the native
  network check (16 checks), Java loopback TLS tests, and external HTTP/HTTPS
  requests to example.com and Google with certificate verification. **TLS** is Conscrypt's
  (2026-10-14: `libjavacrypto.so` built for ARMv7 over Android 9's own
  `libssl.so`; docs/JAVA.md, "The security providers"): under qemu a TLS 1.2
  handshake on loopback and an https request through Android's HTTP client
  work, and the platform trusts Android 9's 137 certificate authorities.
  Build 222 also negotiates HTTP/2 ALPN through the Android 10 public socket
  APIs; see [the phone network investigation](PHONE-NETWORK.md). Not there: ancillary
  data over sockets (no descriptors passed), `AF_NETLINK` (an app asking the
  kernel for routes is told there is no such family), and a
  `ConnectivityManager` that follows the phone: the framework always says
  connected over Wi-Fi, whatever the phone has.
- **ETC1 textures** (see above).
- **Direct system calls outside the recognised loader patterns**, including newly generated JIT code. Recognised ARM/Thumb `svc 0` wrappers now dispatch through the Linux shims; literal-loaded `cacheflush`, raw errno and a 4-byte-aligned guest stack pass under qemu-arm. The Windows exception path still needs a phone run (ARCHITECTURE.md, "Direct system calls").
- **The on-screen keyboard**, the software `ANativeWindow_lock` path on a driver's window (a NativeActivity's; a `SurfaceView`'s has one).
- **Room.** A 32-bit process has 2 GB of addresses, and a big game wants more (see Unity above).
- C++ exceptions thrown through guest frames are untested (no NDK here to build a
  C++ test with); the unwinder's lookup (`dl_unwind_find_exidx`) is provided.
- **ARM-to-Thumb translation** is not needed on the phone tried. It stays designed
  ([ARM-TO-THUMB.md](ARM-TO-THUMB.md)) for a phone whose probe says otherwise.
