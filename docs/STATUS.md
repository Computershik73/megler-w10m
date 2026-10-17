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
| Loader: REL, packed (APS2) and RELR relocations, GNU and SysV hash, constructors, scope | **passes on the phone** |
| Thumb-2 guest library: data, calls both ways, floats, variadics, 64-bit division, heap, `qsort` callbacks, `setjmp`, threads, TLS, files, pipes | **passes on the phone** |
| The same library built in ARM mode | **passes on the phone** |
| PLT entries rewritten; the rescue of an entry entered in Thumb state | **passes on the phone** |
| Real libraries: OpenSSL `libcrypto.so`/`libssl.so` (ARM mode), CPU-Z's `libcpuid.so` | **load under qemu**; SHA-256, AES and bignum results correct. Not tried on the phone |
| A NativeActivity app through `megler_run`: manifest, JNI, assets, looper, EGL, frames, touch, focus, pause, Back, teardown | **passes on the phone**, with a stand-in for OpenGL ES |
| Sound: an OpenSL ES buffer-queue player, resampled and mixed to 44.1 kHz stereo for the shell | **passes on the phone** as far as the mixer's output (sample counts and levels) |
| Films: `libmediandk.so` (extractor, decoders, formats) over the shell's Media Foundation reader; pictures onto a `SurfaceTexture` | **works on the phone** with Hollow Knight's two opening films (H.264 360x202, AAC): every picture decoded and shown full screen, the sound in step with them; watched and heard by the phone's owner, played through and skipped |
| The shared storage: one `/sdcard` for every app, the app's own `Android/data` and `obb` in it, and links to folders of the phone's own through handles the shell hands over | **passes under qemu and on PC** with a stand-in for the shell (`core/tests/hk_storage_tests.c`, 424 checks), and on PC with the shell's own host over Windows.Storage (`player/StorageHost.cpp`, the same checks; a test program is no app container). **On the phone** (HP Elite x3, 0.1.0.126, `tools/build-appx.sh arm --auto storage`): Music, Pictures, Movies and DCIM are found and answer, and the check's 102 steps pass -- 24 in each link, in a folder of its own: files made, written, read back, appended to, emptied, renamed, moved, removed, folders likewise, and a file moved from one link to another. One thing the PC did not show: an open straight after a close is at times refused with a sharing violation, something of Windows' own having the new file for a moment (2 steps of 102, different ones each run); the host asks again for up to a second. A folder is not moved into another folder by renaming (the app is told EXDEV, and copies). **Not tried**: the Settings section (the list, Add a Folder with the folder picker, the folders Windows remembers across starts), Documents, Downloads, a memory card |
| The network: TCP and UDP sockets, servers included, blocking and not; the app's own AF_UNIX sockets; names (`getaddrinfo`, `getnameinfo`, `gethostbyname`); the interfaces; poll, select and epoll over sockets with pipes, eventfds and files (docs/ARCHITECTURE.md, "The network") | **passes under qemu and on PC** over loopback (`core/tests/hk_net_tests.c`: 234 checks under qemu, 225 on the PC, which cannot run the test of signal 34 ending a blocked call), and from Java under qemu (the test app `data`: `InetAddress`, `HttpURLConnection` against a `ServerSocket` of the app's own, `DatagramSocket`, a `ServerSocket` closed under the thread in `accept`, a `java.nio` selector, `NetworkInterface`). The check a phone is to run (`megler_net_check`: a public name, `HEAD /generate_204` on its port 80, a server of its own on 127.0.0.1, UDP on 127.0.0.1) passes on both with the internet. **Not tried on a phone at all**: `tools/build-appx.sh arm --auto net` is the run to make; whether an app container's Winsock lets an app reach its own server on loopback is the first thing it answers. No TLS: https fails with an `SSLException` as before |
| OpenGL ES from an app's Java: the 832 natives of `GLES10` to `GLES32` (793 generated, 39 by hand), EGL14 and EGL10 over `megler.EGLNative`, a `GLSurfaceView`'s renderer on a GL thread of the layer's own, the window composed in the core of the views over the frames read back (docs/ARCHITECTURE.md, "OpenGL ES from an app's Java") | **passes under qemu and on PC** on a stand-in for OpenGL ES that draws (`core/tests/hk_gl_tests.c`: 86 checks on libcore, 67 on the built-in java.lang; the test app `gl`: 49 checks under qemu -- a renderer's calls in Android's order, sixty frames a second at most, `RENDERMODE_WHEN_DIRTY`, `queueEvent`, a fixed size, pause and resume, EGL14 on a pbuffer, EGL10 and GL10 on a `SurfaceView`, and the picture). AIDA64 and Device Info read the GPU's strings and show them under qemu. A swap of a 720 x 1182 surface costs 1.45 ms on the PC with the stand-in's read-back (a copy of memory); ANGLE's waits for the GPU and **has not been measured: nothing of it has run on a phone** (the log's `java: OpenGL frames` line will say it there) |
| Sound out of the phone (XAudio2) | **heard on the phone** with build 0.1.0.21, the Probe's tone and the demo's; XAudio2 played every frame it was given. With 0.1.0.19 nothing was heard and the log could not say why: the engine was built on the app's own thread then, and nothing was counted |
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
  the T bit.) `hk_fault_resume_at` is the one place that knows; both crashes of
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
- the driver (`core/src/drivers/hk_unity.c`) does what `UnityPlayer` does: the
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
  the app's now (`hk_jni_prefs.c`), read back through `getAll()` as Unity does
  when it starts. Without them the game could not be begun at all: a new game's
  first time goes by way of a page of picture settings (this port has its
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
    still has and goes when the last one does (`hk_libc_mem.c`; the test
    `t_map_pieces` maps and unmaps 4 GB that way).
  - The APK, 462 MB, was mapped whole. It is read where needed now (`hk_zip.c`).
  - The game's textures reach OpenGL as plain RGBA8: Unity decodes them itself
    from a format this OpenGL does not offer (almost certainly ASTC; ANGLE over
    Direct3D 11 has none), a few bits a pixel in the APK and thirty-two here, 930 MB
    of them by the time the game is up. `hk_gltex.c` counts what is asked for and,
    once an app has asked for 160 MB, keeps large RGBA8 textures that are only ever
    filled and sampled as DXT5, compressing the pixels as they are uploaded: 661 MB
    became 165. It costs some of the picture and nothing has judged how much.
    Flags `plain-textures` and `pack-textures` turn it off and on from the start.

Unity's frame pacing is `core/src/jni/hk_jni_loop.c`: Android's `Looper`,
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
  did it on the graphics thread. `hk_thread_whole_stack` now commits the whole
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
  already ended this process".
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

Tools made for this, all still there: the trace (`hk_trace`: a ring of the last
4096 calls of note, printed by the crash report), the crash report's module names,
stack scan, thread and memory lines, the flags (`hk_flag`: a file's presence in
`LocalState\flags`, put there through the Device Portal: `no-interrupt`,
`no-render`, `late-render`, `no-egl`, `no-panel`, `early-present`, `no-present`,
`normal-priority`, `trace-gl`), `trace-gl` (every GL call through a generated
thunk), the unattended `play` mode, and `tools/phone-etw.py`.

**Sound.** FMOD said "Error initializing output device" twice and made no player.
It had not got as far as OpenSL ES: before that it asks `AudioManager.getProperty`
for the output's sample rate and frames per buffer, under the names the constants
`AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE` and `..._FRAMES_PER_BUFFER` hold, and
parses the answers with `Integer.parseInt`. The constants were static fields
nobody had given a value, so the names were null, so were the answers. Three
things follow from that one: a table of Android's constants
(`android_constants` in `jni/hk_jni_android.c`: the ones engines read, by class),
the parsers (`jni/hk_jni_loop.c`), and a warning, once for each field, when a
static field is read that nothing has given a value -- a silent null is the kind
of thing that costs a day. With them FMOD makes its OpenSL ES player (44100 Hz,
stereo, 16 bits) and the game is heard: the flag `record-audio` writes half a
minute of the mixer's output to `megler-audio.wav`, and what came back was music
(tonal, at musical pitches, 0.01% of it clipped, no gaps).

**Films.** Unity's VideoPlayer wants `libmediandk.so` (and falls back to the Java
`MediaExtractor`, which is not there). `ndk/hk_media.c` is that library with no
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
nothing in the one menu tried (`KeyCharacterMap` is not there). Not there: sound
through `AudioTrack`, films for an engine that asks Java (`MediaExtractor`,
`MediaCodec`, `MediaPlayer`) instead of `libmediandk.so`, films that are not
something Windows decodes (WebM/VP8, Ogg), and the listeners the engine registers
(display, input devices, system UI) are never called.

## Not there yet

- **Drivers** other than NativeActivity: SDL, cocos2d-x, Unity, Unreal, Godot. This
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
  original Megler's Java framework (built here from that repository,
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
  of one file under `core/framework-overlay` (the original project has them
  too): a list that no longer answers a finger once its Activity has been
  covered and shown again (`AbsListView`, re-attached, never laid out); an
  `AlertDialog` that took 14 seconds to appear on the phone, because the first
  look-up of a resource by name walked all 161,000 entries of Android 9's
  resource table and the items of a style were searched one by one
  (`ResTable`, `Resources`: 710 million instructions down to 3 million, found
  with the interpreter's profile, `hk_dvm_profile`); a pressed row's ripple
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
  and `obb` where they were, and `Music`, `Pictures`, `Movies`, `DCIM` and
  whatever the user adds in Settings being the phone's own folders. `statfs`
  answers the phone's real room (34.9 GB free of 56.7 on that phone).
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
  the page brings the keyboard, whose text arrives in the field. Not yet on a
  phone.
  Not there: garbage collection (an app that animates lasts minutes); sound
  from Java; a `SurfaceView` drawn by native code; method
  handles; other apps (the
  package manager knows only the one running); Kotlin has run only as part of
  Device Info.
- **Sound from files.** OpenSL ES players fed PCM through a buffer queue are mixed; a player given a file or a URL (which would need MP3/Ogg decoding here) is refused. AAudio is not there.
- **The network on a phone.** Sockets and names work under qemu and on the PC
  (the table above) and have not run on a phone. **TLS** is conscrypt's
  (2026-10-14: `libjavacrypto.so` built for ARMv7 over Android 9's own
  `libssl.so`; docs/JAVA.md, "The security providers"): under qemu a TLS 1.2
  handshake on loopback and an https request through Android's HTTP client
  work, and the platform trusts Android 9's 137 certificate authorities; not
  tried on a phone or against a server on the Internet. Not there: ancillary
  data over sockets (no descriptors passed), `AF_NETLINK` (an app asking the
  kernel for routes is told there is no such family), and a
  `ConnectivityManager` that follows the phone: the framework always says
  connected over Wi-Fi, whatever the phone has.
- **ETC1 textures** (see above).
- **Direct system calls** in guest code (`svc 0`), which a JIT inside an app makes.
- **The on-screen keyboard**, sensors, the software `ANativeWindow_lock` path.
- **Room.** A 32-bit process has 2 GB of addresses, and a big game wants more (see Unity above).
- C++ exceptions thrown through guest frames are untested (no NDK here to build a
  C++ test with); the unwinder's lookup (`dl_unwind_find_exidx`) is provided.
- **ARM-to-Thumb translation** is not needed on the phone tried. It stays designed
  ([ARM-TO-THUMB.md](ARM-TO-THUMB.md)) for a phone whose probe says otherwise.
