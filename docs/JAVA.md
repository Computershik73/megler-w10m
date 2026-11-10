# Java: running an app's own bytecode

An ordinary Android app is DEX bytecode; its native libraries, if it has any, are
called from that. Everything else in Megler runs apps whose code is native and
plays their Java side in C (see [ARCHITECTURE.md](ARCHITECTURE.md), "The Java
side"). This document is about the other kind: a runtime that executes the
bytecode itself.

Three milestones of it are written and tested. The first was a DEX reader and a
Dalvik interpreter with a small `java.lang` of its own. The second joined it to
the JNI layer and put **Android 9's real libcore** under it, with libcore's own
native libraries (32-bit ARM, loaded by Megler's linker) doing the native
work. The third put an Android framework on top -- the one in `core/framework`,
built here from its sources -- and a driver that starts an app with it: **an APK that
is Java alone starts through `megler_run`, its `Application` and `Activity` run,
its layout is inflated from its resources, measured, laid out and drawn, the
picture reaches the window host, a touch runs a listener, and Back sends it
to the back as Android 12 does.** That is *proven* under qemu-arm, with a
stand-in for the shell's drawing.

**Nothing of it has run on a phone**, and the shell has not yet got its half:
the drawing (`megler_gfx_host`: Direct2D, DirectWrite and WIC behind a C
interface) and the showing of a picture that is memory, not a texture. The
phone's package builds with all of it.

As in the other documents, **proven** means a test that was run shows it, and
where; **expected** and **not verified** mean what they say.

## What is there

| | |
|---|---|
| `core/src/dvm/mg_dvm.h` | the interface: start (two ways), add DEX files, register native methods, call, exceptions, tracing |
| `core/src/dvm/mg_dvm_internal.h` | the structures (class, method, field, object, frame, thread) and how registers hold values |
| `core/src/dvm/mg_dvm_dex.c` | reading a DEX file: header, checksum, tables, strings, line numbers, instruction lengths, a method's annotations |
| `core/src/dvm/mg_dvm_class.c` | which file defines which class, linking, vtables, lookups, resolution, static initialisation, binding native methods |
| `core/src/dvm/mg_dvm_object.c` | objects, arrays, strings, Class objects, monitors, a thread's state, exceptions |
| `core/src/dvm/mg_dvm_interp.c` | the interpreter: frames, calls, every instruction, exception delivery |
| `core/src/dvm/mg_dvm.c` | the interface's functions; the two ways to start |
| `core/src/dvm/mg_dvm_apk.c` | `classesN.dex` out of an APK |
| `core/src/dvm/mg_dvm_boot.c`, `boot/*.smali`, `mg_dvm_boot_dex.h` | the first way to start: a `java.lang` of 66 classes, in smali and C, built in |
| `core/src/dvm/mg_dvm_art.c` | the second way: what Android's runtime implements of libcore inside itself (`art/runtime/native/*.cc`), in C: `Object`, `Class`, `String`, `StringFactory`, `Thread`, `Throwable`, `System.arraycopy`, `VMRuntime`, `VMStack`, `Unsafe`, references, arrays |
| `core/src/dvm/mg_dvm_art_reflect.c` | `java.lang.reflect`: `Field`, `Method`, `Constructor` and the `Class` methods that make them |
| `core/src/dvm/mg_dvm_art_host.c` | what stands in for libcore's native libraries where they cannot run (a PC): a few dozen methods, enough to start and to run what needs no ICU |
| `core/src/dvm/mg_dvm_jvm.c` | loading libcore's native libraries, and `libopenjdkjvm.so`'s `JVM_*` functions for them |
| `core/src/dvm/mg_dvm_art_annot.c` | annotations as reflection reads them, and what a DEX file keeps as annotations of its own (nesting, generic signatures, declared exceptions) |
| `core/src/jni/mg_jni.c` | the JNI layer, which is now also the runtime's JNI: see "One Java world" below |
| `core/src/drivers/mg_java.c` | the driver for an app that is Java: starts the runtime, the framework and the app; `megler.Native` (the log, the APK's files, the screen, input, the end) |
| `core/src/drivers/mg_java_gfx.c` | `megler.Gfx`: bitmaps as memory, and `android.graphics` handed to the shell through `megler_gfx_host` |
| `core/src/drivers/mg_java_ndk.c` | the NDK's functions that are about the framework's objects: libjnigraphics on its `Bitmap`; `ANativeWindow_fromSurface` on a `SurfaceView`'s or a `SurfaceTexture`'s `Surface`, and that window's EGL surfaces and software buffers ("A SurfaceView drawn by native code") |
| `core/src/drivers/mg_java_sqlite.c` | `megler.Sqlite`: an app's databases |
| `core/src/drivers/mg_java_camera.c` | `megler.CameraNative`: the phone's cameras behind the overlay's `android.hardware.Camera`, over the shell's (`megler_camera_host`): pictures copied as NV21, drawn for a view mirrored and turned, the news a preview thread waits for ("The cameras") |
| `core/src/drivers/mg_java_webview.c` | what Android's platform gives the WebView besides Java: `WebViewLibraryLoader`'s natives (address space reserved, the library loaded into it from the WebView APK) and `libwebviewchromium_plat_support.so`'s tables, which are none ("The WebView") |
| `core/src/drivers/mg_java_gl.c`, `mg_java_gles.c` | OpenGL ES and EGL from Java: the natives of `android.opengl.GLES10` to `GLES32` (`mg_java_gles.c` generated by `tools/gles-natives.py`, the rest by hand), `megler.EGLNative`, a `GLSurfaceView`'s GL thread, and the window composed of the views and the frames of GL surfaces under them (docs/ARCHITECTURE.md, "OpenGL ES from an app's Java") |
| `core/src/sqlite/mg_sqlite3.c`, `mg_sqlite.c` | SQLite's amalgamation compiled in where it is there, and its files, locks (in the process and on the file, for other processes) and mutexes on the host layer |
| `core/src/webp/` | WebP pictures decoded in the core: `mg_webp.c`, and libwebp's decoder compiled in where it is there, file by file |
| `core/include/megler.h` | `megler_gfx_host`, the contract for the shell's drawing; `megler_window_host.present_pixels`; `megler_run_config.android_root` |
| `core/framework-overlay/` | what differs here from the Java framework as it came (`core/framework`): files that replace its files and members added to its classes, each saying at its top what differs and why |
| `core/src/dvm/art/*.smali`, `mg_dvm_art_dex.h` | seven classes the runtime adds to libcore's: a security provider that stands in for conscrypt's where conscrypt's library is not in the Android root, with SSLContexts whose connections fail politely and a trust manager factory that trusts nothing, and the getter libcore asks for the phone's time zone |
| `tools/build-framework.sh` | builds the framework's DEX from its sources in `core/framework` and the overlay |
| `core/tests/app/`, `tools/build-test-app.sh` | the test apps, ordinary Android apps (aapt2, javac, d8), and their build |
| `core/tests/mg_java_tests.c` | the test of the driver: the apps started through `megler_run`, their log and their pictures checked |
| `core/tests/mg_gfx_standin.c`, `mg_gfx_tests.c`, `mg_gfx_natives_tests.c` | a stand-in for the shell's drawing (no anti-aliasing, text as boxes, PNG), and the tests of it and of `megler.Gfx` |
| `core/tests/mg_camera_standin.c` | a stand-in for the shell's cameras: a back and a front camera whose pictures' every byte is known, made on a thread of their own |
| `core/tests/mg_gl_standin.c`, `mg_gl_tests.c` | a stand-in for EGL and OpenGL ES that draws (into memory: clears, triangles of one colour, read-back), and the tests of OpenGL ES from Java on it |
| `core/tests/mg_dvm_tests*.c`, `core/tests/dvm/` | the tests, part of the core's own (`mg_tests_run`, so the phone's Self-test too); 39 `.smali` fixtures that make four DEX files; two programs compiled by javac and d8, kept with the JVM's answers beside them |
| `core/tests/guest/t_jni.c` | a guest library with native methods, for the tests of the join |
| `tools/dexasm.py` | a DEX assembler for a subset of smali, a structural checker, a lister |
| `tools/dvm-ops.py`, `tools/dvm-boot.py` | generate the arithmetic tests and their answers; the built-in `java.lang` as a C array |
| `tools/build-dvm-fixtures.sh` | regenerates the generated sources and puts the tests' DEX files in `build/guest/armeabi-v7a/dvm` (`tools/build-tests.sh` and `tools/build-appx.sh` run it) |
| `tools/dvm-d8-fixture.sh` | rebuilds the two javac-and-d8 programs and the JVM's answers for them |
| `tools/dvm-android-root.sh` | puts together the folder libcore is started from, out of `build/android9` |
| `tools/dvm-report.sh`, `tools/dvm-natives.py` | the measurements this document quotes |

The runtime is compiled into the core like any other module. An app with a
native engine does not pay for it: nothing of it runs until `mg_dvm_start` or
`mg_dvm_start_android` is called, and only the Java driver calls either.

## What is proven

Two commands. Each runs the whole of the core's tests twice, in two processes
(a process has one runtime and one boot class path): once with the runtime on
its built-in `java.lang`, and once on Android 9's libcore from
`build/android9/root`. Under qemu-arm the second command then starts each of the
nine Java test apps through `megler_run`, a process each (five of them twice).
These numbers are with SQLite and the WebP decoder compiled in
(`build/sqlite/sqlite3.c` and `build/webp/src` there); without SQLite each of
the first two pairs is 38 checks fewer and the app `data` is not run, without
the decoder 40 fewer, and the script says which is missing.

```
bash tools/build-tests.sh win      (MSVC x64, run on the PC; libcore's native libraries cannot run there)
    4575 checks, 0 failed          on the built-in java.lang (OpenGL ES from Java's 117 among them)
    4628 checks, 0 failed          on libcore, its bytecode alone (OpenGL ES from Java's 136 among them)
    (then the shell's own drawing and the storage on Windows' own folders, which are not this document's: 296, 266, 266, 296, 296, 451, 110, 12, 8, 8, 8, 6 and 6 checks;
     and the shell's cameras on this PC's one, DroidCam's virtual camera: 9 checks)

bash tools/build-tests.sh arm      (gcc for arm-linux-gnueabihf, Thumb-2, hard-float, under qemu-arm)
    4817 checks, 0 failed          on the built-in java.lang
    4915 checks, 0 failed          on libcore with its native libraries
      50 checks, 0 failed          the Java test app hello, through megler_run
     121 checks, 0 failed          the Java test app demos, the keyboard's 25 and the frame meter's 3 among them
      85 checks, 0 failed          the Java test app natives: a library of its own, built by the NDK; native code drawing on SurfaceViews and a TextureView
      18 checks, 0 failed          the Java test app crash: an exception nothing catches, launcher aliases, an app written for Android 6
      19 checks, 0 failed          the Java test app gc: references, finalizers, threads allocating, a heap that stays small
      49 checks, 0 failed          the Java test app gl: a GLSurfaceView, EGL14 on a pbuffer, EGL10 on a SurfaceView, on a stand-in for OpenGL ES that draws
     106 checks, 0 failed          the Java test app data: a database, the storage, the battery, the time zone, TLS (conscrypt's, on loopback), key pairs, codecs, the network and RenderNodes, the first run
     111 checks, 0 failed          data again, on what the first run left, with another battery and a time zone that is none
      29 checks, 0 failed          the Java test app webview: Android's System WebView from the Android root, on pages of the app's own server
      38 checks, 0 failed          the Java test app camera: the old android.hardware.Camera on a stand-in for the shell's cameras, the previews' pictures in the window
      13 checks, 0 failed          camera again, every camera refused
      50 checks, 0 failed          gl again, the window composed by the core
      85 checks, 0 failed          natives again, the window composed by the core
      50 checks, 0 failed          hello again, on an upright surface (720 x 1280 at 320 dpi)
    (then each app once more, its drawing recorded for the PC's run to draw again)
```

(Those are the numbers of 2026-10-18, in a worktree of this repository.
On 2026-10-14, in a worktree whose path was longer, the storage's run on
Windows' own folders failed 4 of its 441 checks, the names Windows refuses in
a scratch folder of 112 characters; the same program passes all 441 with a
scratch folder of 93 characters or fewer. It is that test's folder running
into Windows' 260 characters, not anything of the Java side.)

82 and 242 of the first were the core's own before the runtime joined them, and
740 are the drawing's (below). The script fails if any run does. The Java apps
need a JDK and the Android SDK's build tools to build the framework and the
apps, and `natives` an NDK as well; where those are not there the script says
so and leaves them out (`--no-java` does too, `--no-libcore` the libcore run
as well).

`bash tools/build-appx.sh arm` compiles all of it with the phone's compiler
(MSVC for ARM, `/W3`) without a warning and packs `Megler_<version>_ARM.appx`
(3078 KB); with `--libcore` the package also carries everything Java runs on
(37,745 KB). **Neither package has been installed or run.**

### The interpreter (both runs, both hosts)

- **Every arithmetic instruction** (1991 checks): `tools/dvm-ops.py` writes one
  method for each binary operation in its three-register, two-register and
  literal forms, each conversion, comparison and conditional branch, and works out
  in Python, from the Java language's rules, what each returns for operands
  chosen at the edges (the most negative number over -1, shifts by 32 and more,
  NaN, infinities, the smallest denormal, values that round). The interpreter
  must agree. Division by zero must throw.
- **The fixtures** (`core/tests/dvm/fixtures`, assembled by `tools/dexasm.py`):
  values of two registers in every form of move and at every argument offset;
  packed and sparse switches; arrays of all eight primitive types and of
  references, `fill-array-data` at each width, `filled-new-array`, the checks
  Java makes on them; exceptions caught by class, superclass and catch-all,
  through several frames with `finally` blocks on the way, thrown by the
  interpreter, by a native method and inside a handler, with stack traces that
  name the right lines; virtual, super, direct, static and interface calls,
  default methods, methods an abstract class only inherits from an interface, a
  class in one DEX file under classes of another; the order of static
  initialisation, classes whose initialisers need each other, the values a file
  gives static fields; string constants and their identity across files, what
  d8 turns concatenation into, text outside ASCII; monitors; an enum; a DEX
  file of version 038 with a call site.
- **Two threads at once**: a class whose initialiser takes a while, asked for
  by both (it runs once); the same classes linked and the same strings interned
  by both in opposite orders (one object each); a counter both add to 50,000
  times under its monitor (100,000).
- **Limits and refusals**: 10,000 calls each inside the last; recursion without
  end, which is a `StackOverflowError` the code can catch; native code calling
  bytecode calling native code 50 deep, and a million deep (an error, after
  which everything still works); `invoke-polymorphic`, `invoke-custom` and
  `const-method-type`, each an `UnsupportedOperationException` naming the
  instruction; classes that are their own superclass, extend an interface, or
  lack an ancestor; files that are not DEX, are cut short, have one bit changed
  or a table pointing outside the file.
- **A real app's DEX**: `classes.dex` of the Hollow Knight APK (Unity's Java
  side), read out of the APK. Every class, method and instruction is read: 106
  classes, 358 fields, 614 methods, 554 with code, 7232 instructions, the same
  counts `tools/dexasm.py check` gives, which shares no code with the C reader.
  On libcore 78 of the 106 link (71 on the built-in `java.lang`); the 28 that
  do not each name an `android.*` class that is not there, and none fails for
  another reason. 77 static initialisers run; the one that cannot wants
  `android/os/Build$VERSION`.

The same fixtures give the same answers on libcore as on the built-in
`java.lang`, but for three messages that libcore words its own way and the
tests expect each way where it applies (an enum constant that is not there,
a `substring` out of range, an `ExceptionInInitializerError`'s text).

### The join with the JNI layer (both runs; the guest library under qemu-arm)

- **The JNIEnv on the runtime's classes**, called through the function table
  as native code calls it, on every host: `FindClass` (a class a DEX file
  defines is the runtime's; one that cannot be linked is an exception and no
  class), `GetSuperclass`, `IsAssignableFrom`, method and field ids with
  `NoSuchMethodError` and `NoSuchFieldError` for what is not there, virtual,
  non-virtual, static and interface calls with long arguments, `NewObject`,
  instance and static fields (an int, a reference, a long), strings both ways
  in UTF-16 and UTF-8, int and object arrays both ways, `RegisterNatives`,
  `Throw`, `ThrowNew`, `ExceptionOccurred` and `ExceptionClear` with
  an exception thrown by bytecode and caught by native code and the reverse,
  monitors; and a class no DEX file defines, which stays the layer's own and
  whose objects bytecode can hold.
- **A guest library** (`core/tests/guest/t_jni.c`, `libt_jni.so`), under
  qemu-arm: its native methods, eleven registered by its `JNI_OnLoad` (with a
  twelfth the class does not have, which is said and not fatal) and one found
  by its `Java_` name, are called from bytecode; they call a static and a
  virtual method back, take and return strings and arrays, throw to bytecode
  and catch what bytecode throws.

### On Android 9's libcore

The six files of the boot class path the test starts from are Android 9's own
(`core-oj`, `core-libart`, `conscrypt`, `okhttp`, `bouncycastle`, `apache-xml`:
7617 classes, 11.4 MB of DEX), and after them seven classes of the runtime's
own ("The security providers" below, and the getter of "The time zone"): 7624.

**Without libcore's native libraries** (the PC, and the same code as ARM):

- All six files are read and **every one of the 7624 classes links** (in both
  runs: `7624 linked, 0 not`).
- The program that uses `java.lang` alone (`d8t/Main.java`) gives the JVM's
  answers for 9 of its 10 tests, and the one that uses `java.util`, `java.io`
  and `java.text` (`d8t/Lib.java`) for the 7 of its 19 that need no native
  library: `ArrayList`, `LinkedList`, `ArrayDeque`, `PriorityQueue`, sorting
  with comparators, `HashMap`, `TreeMap`, `LinkedHashMap`, `HashSet`, `BitSet`,
  iterators, `StringBuilder`, `String`'s own methods, boxes, `Optional`,
  `Objects`, `java.util.stream` with lambdas and collectors, byte-array
  streams, `DataInput`/`DataOutput`, `ByteBuffer` in both byte orders; and
  what a framework does to an app's classes -- reflection on fields, methods
  and constructors with boxing and widening, `Proxy`, annotations of every
  kind of element with their defaults, on classes, fields, methods and
  parameters, nested and anonymous classes' names and enclosers, generic
  signatures, declared exceptions, `ThreadLocal`, `WeakReference`,
  `SoftReference` and `WeakHashMap`. The others are left out there and counted
  as left out.
- **Which static initialisers fail without the libraries** (`bash
  tools/dvm-report.sh init`): of 7619, 7240 run and 379 do not. 21 fail
  themselves, each for a native method that is in `libjavacore.so` or
  `libopenjdk.so` and nowhere else: `libcore.io.Linux` (twice),
  `java.io.UnixFileSystem`, `java.lang.Character` (ICU's tables),
  `ProcessEnvironment`, `UNIXProcess`, `java.math.NativeBN`,
  `java.util.regex.Pattern` and `Matcher`, `libcore.icu.ICU` and
  `NativeConverter`, the Expat parser, eight classes of `sun.nio.ch` and two of
  `sun.nio.fs`. The other 358 need one of those. So without the libraries
  there is no charset but what `String` does itself, no regular expression,
  no `BigInteger`, no file and nothing that prints: a string cannot be turned
  into bytes.

**With them** (qemu-arm; `build/android9/lib`, built by the other half of this
work: `libjavacore`, `libopenjdk`, `libicuuc`, `libicui18n`, `libnativehelper`,
`libexpat`, `libcrypto`):

- The libraries load through `ld/mg_ld.c` with **no import left unprovided**
  (a check), `libnativehelper`'s `JniConstants::init` and the `JNI_OnLoad` of
  `libjavacore` and `libopenjdk` run, and they register **569 native methods,
  every one of which the boot class path declares** (two checks: 569, and 0
  table entries that named nothing).
- `System.out.println("hello from libcore")` comes out: through libcore's
  `PrintStream`, ICU's converter and `libjavacore`'s `write` on the layer's
  file descriptors, into the layer's log. The test also turns `System.out`'s
  descriptor to a file for one line and reads back exactly those 19 bytes.
  (The descriptor is not 1: libcore's `FileDescriptor.out` is a duplicate of 1
  it makes when the class starts.)
- **Both programs match the JVM**: all 10 tests of `Main` and all 19 of `Lib`,
  which add regular expressions, `String.format`, `DecimalFormat`,
  `SimpleDateFormat` in UTC and in `Europe/Moscow`, `BigInteger` and
  `BigDecimal`, case conversion and character classes outside ASCII, charsets
  (UTF-8, UTF-16LE, ISO-8859-1, US-ASCII, windows-1251), `CRC32`, `Deflater`
  and `Inflater`, files through `java.io`
  (`File`, streams, readers and writers, `RandomAccessFile`) and through
  `java.nio.file` and a `FileChannel`, a second thread with `join`, `wait` and
  `notify`, exceptions the interpreter throws, and `System.currentTimeMillis`,
  `nanoTime`, `getProperty`, `arraycopy`, `identityHashCode`; and through
  conscrypt over BoringSSL, digests, HMAC, AES-CBC, AES-GCM (a known answer,
  and a changed bit refused) and a TLS 1.2 handshake with a server of the
  test's own on loopback, ECDHE_ECDSA with AES-128-GCM and a line each way
  (`lib_tls`). The JVM's answers were made by JDK 1.8.0_202 from the same
  class files.
- **Two tests' answers are Android's and not the JVM's**, and
  `core/tests/dvm/d8/lib-android.txt` says which and why: three messages of
  exceptions the runtime itself throws, which Java does not specify and
  Android words its own way -- `divide by zero` (the JVM: `/ by zero`), a
  negative array size as `Dimension 2: -1` (the JVM 8: no message), and an
  index out of bounds as `length=5; index=5` (the JVM: `5`); and the ECDHE
  group of the TLS handshake, which a relay of the test's own reads off the
  server's ServerKeyExchange: `x25519`, BoringSSL's first, where Java 8's
  SunJSSE, which has no X25519, takes `secp256r1`. A difference that is not in
  that file fails the test. There is no other.
- `java.nio.file`'s native half is in no table: `libopenjdk.so` exports it as
  `Java_sun_nio_fs_UnixNativeDispatcher_...`, and those are found by name (a
  check that some were; 15 by the end of the two programs).
- **Who answers each native method** (`bash tools/dvm-report.sh natives arm`):
  the boot class path declares 1239 in 97 classes. The runtime answers 304 in
  C, the libraries 923 (846 they registered, 77 found by their JNI names), and
  12 have no answer (`sun.misc.Version` 4, `NativeStart` 2, `TimeZone` 2,
  `sun.nio.ch.Net` 2, and one each of `MethodHandleImpl` and a test class).
  Calling one is an `UnsatisfiedLinkError`. conscrypt's `NativeCrypto` has
  280, all registered by `libjavacrypto.so` when `NativeCrypto` starts (the
  report starts it; 2026-10-15); before that library was built they were
  nobody's.
- **Which static initialisers fail with the libraries** (`bash
  tools/dvm-report.sh init arm`): of 7624, 7620 run, conscrypt's
  `NativeCrypto` and the ten that follow from it among them (2026-10-15;
  before its library was built they could not). The four that did not each
  used up the 50 million instructions they are given there, all in
  `android.icu.text.UnicodeSet.applyFilter` (ICU's transliterators), which is
  *not* shown to be a failure or not to be one.
- **What apps use of `java.security`** (two more tests of `Lib`, against the
  JVM): `MessageDigest` MD5, SHA-1, SHA-256 and SHA-512, in one piece and in
  several; `SecureRandom` (bytes, ints, seeds); `UUID.randomUUID()` and the
  name-based kind; `CRC32` and `Adler32`; and one step further, `Mac`
  HmacSHA256, `Cipher` AES/CBC/PKCS5Padding there and back, PBKDF2 key
  derivation, `Base64`. All give the JVM's answers. Under qemu, with
  conscrypt's library in the root, it is conscrypt's; on the PC's build the
  runtime's stand-in's: see "The security providers".
- **java.nio's covariant returns.** Java 9 gave `ByteBuffer`, `CharBuffer`
  and the other buffers their own `position(int)`, `limit(int)`, `mark()`,
  `reset()`, `clear()`, `flip()` and `rewind()`, each returning the buffer's
  own type, and Android's libcore has them from a later release than 9 (the
  framework says it is API 34). Code compiled against such an `android.jar`
  names them: `java/nio/ByteBuffer.position(I)Ljava/nio/ByteBuffer;`, which
  Android 9's libcore does not have, and every such call was a
  `NoSuchMethodError` (Brave 1.97's Mojo bindings, on its first IPC message).
  Each of them returns the buffer it was called on, so `Buffer`'s method is
  the same call: the runtime resolves a reference to one of these seven, in a
  class of `java.nio` and returning that class, to `Buffer`'s
  (`nio_covariant` in `core/src/dvm/mg_dvm_class.c`) when the class has no
  method of that exact signature. Nothing else is looked up that way.
  Fixture `t/Nio`, on libcore only: a `ByteBuffer` through all seven and a
  `CharBuffer`'s `position`, through their own types; a name that is not one
  of them (`capacity()Ljava/nio/ByteBuffer;`) is still a `NoSuchMethodError`.

### A Java app with an Activity (qemu-arm)

`core/tests/app/hello` is an ordinary small app, built the ordinary way
(`tools/build-test-app.sh`: aapt2, javac against the SDK's `android.jar`, d8;
not signed): a manifest, an `Application` subclass, one `Activity`, a layout in
its resources (a `LinearLayout` with a `TextView`, a `Button`, a view of its own
that draws with `Canvas`, and an `ImageView` of a PNG), strings and colours. It
has no native library. `mg_tests --java hello ...` starts it through
`megler_run`, on a 480 x 800 screen at 160 dpi so that a dp is a pixel, with a
stand-in for the shell's drawing, and checks (47 checks):

- **What the shell reads first**, `megler_apk_report`: the engine is `java`, the
  APK is runnable, and how its launcher activity asks to be held
  (`"orientation"`: `unspecified` for this app, `portrait` for the other,
  which says so in its manifest).
- **What it logs, in Android's order**: `Application.onCreate`; its class
  loader, which is a real `dalvik.system.PathClassLoader` over the APK, the same
  for all its classes, finds the APK's other files
  (`getResourceAsStream("AndroidManifest.xml")`), loads its classes by name and
  throws `ClassNotFoundException` for one that is not there; then the
  activity's `onCreate`, the views found by id with the texts the resources
  give them, `onStart`, `onResume`.
- **A message copied and the copy sent** (`Message.obtain(Message)`,
  `sendToTarget`): the copy is another message for the same handler, and
  arrives there saying what the first said. A dialog of the support library
  sends its buttons' messages this way.
- **A row with no limit on its width** whose two children have no width and a
  weight each and ask for 40 and for 100: they are given 70 each, Android's
  rule for an app written for Android 7 or later, which this app is. (The
  crash app says it is written for Android 6, and there they keep 40 and
  100.) With an exact 200 they have 100 each in both.
- **A launcher's questions**: `AppWidgetHostView.getDefaultPaddingForWidget`
  is Android 9's 8 dp each way (framework-res, found by its name: the
  numbers of the framework's own `megler.IR` are a later Android's); and
  `LauncherApps.getActivityList` lists the app's one launcher activity with
  its label and icon, and `resolveActivity` finds it.
- **A switch measured by itself**: two thumbs and the track's padding wide, as
  Android's `Switch.onMeasure` has it, wider than it is high and wider than
  the picture of its track (38 x 27 with a thumb of 19 at 160 dpi; the track
  is a nine-patch 7 pixels wide).
- **The layout**, as the app reads it back: every view the full 480 wide, the
  button 64 high, the drawn view 100, the picture 64 square, one under the
  other.
- **The first picture**, on pixels: the drawn view's blue field, its red
  rectangle at (10,10)-(50,30) and nothing just outside it, its green one moved
  by `translate`; a `GradientDrawable` with rounded corners and no colour but
  a tint, filled with the tint and its corner left to the field; two letters
  made twice the paint's size by a `RelativeSizeSpan` beside the same two
  drawn by a paint of twice the size, pixel for pixel as many; the PNG
  stretched from 16 to 64 pixels with its orange and
  green halves and the layout's grey showing through its transparent border;
  black boxes where the text is. And in the bottom right corner two
  `SurfaceView`s over two views: one that nothing draws, through which the
  green view behind it must be seen, and one that draws magenta in its
  `surfaceCreated`, whose frame must be seen.
- **A touch on the button** (down and up through `megler_touch`): the
  `OnClickListener` runs, the text becomes `pressed 1`, the drawn view is
  invalidated, and the next picture shows both -- the green rectangle twice as
  wide, and exactly eight letters' worth of black where there were ten. A
  touch beside the button is no press.
- **Pause and resume** from the shell (`megler_set_paused`): `onPause`,
  `onResume`.
- **Back** on the first screen (`megler_back`, since 670): as on Android 12
  and later the task goes to the back and nothing finishes, which
  `megler_back` answers MEGLER_BACK_LEAVE (the shell then leaves the Back to
  the system, which takes the window away). The test app hands its Back on,
  as React Native's and Flutter's activities do, and goes to the back 900 ms
  later the first time -- past the 400 ms `megler_back` waits for an app
  that showed nothing new, so that Back is the app's and the next, a moment
  later, the system's at once -- and after 150 ms the second, within the
  wait. Hidden then, the activity has `onPause` and `onStop`; shown again,
  `onRestart`, `onStart`, `onResume`. A third Back the app takes as
  CoolReader does, finishing its activity 100 ms later: `megler_back`
  answers MEGLER_BACK_ENDED (the shell closes as the app goes), and
  `onDestroy`, the main loop ends, and the app is reported ended with
  status 0.

Each picture is written as a BMP into `build/java-pictures`
(`hello-1-started.bmp`, `hello-2-pressed.bmp`, `hello-3-pressed-twice.bmp`,
and `demos-*.bmp`). They are what the stand-in draws: no anti-aliasing, and
**text as a row of boxes**, one for each letter. With the platform's resources
in the Android root the app has Android 9's dark action bar with its title and
a grey button with its text in the middle; without them it is the bare views.

The same app on **an upright surface**, 720 x 1280 at 320 dpi, passes the same
checks with the layout's numbers doubled (`build/java-pictures-upright`): the
driver takes whatever width, height and density `megler_run_config` gives it,
and the framework lays out for them.

`core/tests/app/demos` is the less tame one (47 checks). It is a `ListView`
with an `ArrayAdapter` over forty strings and the platform's own row layout
(`android.R.layout.simple_list_item_1`); the first row touched starts a run
down the list, and after that a row touched does its own thing alone. What the
test does to it, and sees:

- A drag of the list by half the screen and back to where it began: the list
  scrolls (rows leave at one end and are bound again at the other), at least
  four pictures come of it, no row is clicked, and afterwards every pixel is
  where it was. What it costs is printed (below, "What an AlertDialog cost").

- A finger on the first row: an `AlertDialog`, which must be up within a
  budget (thirty million bytecode instructions; it takes three, and took
  seven hundred). Its picture is checked (a panel where the app says its
  window is, the list dimmed around it), and it waits for a finger on its
  "Yes", which is a touch on another window than the list's.
- `SharedPreferences` written with `apply` and `commit` and read back through
  a second instance, with text outside ASCII; `Handler.postDelayed` in the
  right order, with one callback removed; an `AsyncTask` whose work runs on
  another thread and whose progress and result are delivered on the main one.
- A second `Activity` started for a result with extras in the `Intent`. While
  it is on top, a finger where a row of the list was is *its* touch and no
  click on the list; a finger on its button gives the result back, and the
  list's activity resumes.
- A `Toast`: looked for in the picture itself, since an app cannot ask where
  one is. While it is shown, a patch at the bottom of the screen, across its
  middle (91 x 39 pixels here), is not the list's; two and a half seconds
  later every pixel is the list's first picture again.
- **Then the list is touched again**, after an activity has come and gone
  over it: the dialog's row (its picture and its button once more), the
  toast's row, and a finger that moves down the list and comes up elsewhere,
  which is no click. This is the part that failed on the phone the first time
  an app was tried there; "What differs from the framework as it came" below
  says why.
- A row held down shows that it is pressed inside its own rectangle and
  nowhere else (it was a grey disc as wide as the screen, over the rows above
  and below; the same section says why).

It needs the platform's resources: without `framework-res.apk` in the root it
stops at the row layout's id (`Resources$NotFoundException`).

**On the PC**, where libcore's native libraries cannot run (and `megler_run`
refuses every app, since guest code cannot run; the test starts the driver
itself there: `mg_tests.exe --java hello <apk> <data dir> build\android9\root`),
`hello` gets as far as `Application.onCreate` and into launching its activity,
and stops at the first string out of its resources:
`ResTable$StringPool.get` wants a charset, which is ICU's converter in
`libjavacore.so`. It is not part of `tools/build-tests.sh win`.

**The drawing's own tests** run in both suites on both hosts (740 checks):
the stand-in through the `megler_gfx_host` struct and nothing else (fills,
blending arithmetic on exact values, clips, matrices, save and restore, round
rectangles, blits, text boxes and their measures, PNG read and written, a
scene whose every pixel is hashed and is the same on both machines), and all 54
native methods of `megler.Gfx` called as the runtime calls them.

### A Java app with native libraries (qemu-arm)

`core/tests/app/natives` is the third test app: one Activity and a library of
its own, `libnatives.so`, which needs a second one of the APK's,
`libnatives_dep.so`. Both are built by the NDK (r21e) against its real headers
and libraries, as an app's are -- armeabi-v7a, Thumb-2, softfp --
by `tools/build-test-app.sh`; where there is no NDK the app is not built and
`tools/build-tests.sh` says that it is not run. `mg_tests --java natives ...`
starts it through `megler_run`; the app goes through JNI and the NDK in 28 named
steps and says of each in the log, and on its screen, whether it held. 42
checks:

| Step | What holds |
|---|---|
| `load` | `System.loadLibrary("natives")` in a static initialiser; `JNI_OnLoad` ran once, and its `FindClass` of a class of the app's gave that class |
| `registered` | natives bound by `RegisterNatives`, static and instance |
| `primitives` | natives bound by their `Java_` names; every primitive type in and out, at its limits |
| `stack-mix` | ten arguments of all widths, `int`, `long`, `double` and `float` mixed, most of them on the stack (softfp: a 64-bit value at an even word); an instance method with eight |
| `overloads` | two natives of one name, exported with their parameters in the name (`__I`, `__Ljava_lang_String_2`) |
| `arrays` | `Get/ReleaseIntArrayElements` written back, regions of every other primitive type, `GetPrimitiveArrayCritical`, a `long[]` and a `String[]` made in C, a `String[]` read |
| `bounds` | a region outside an array or a string, an element outside an array: nothing copied, `ArrayIndexOutOfBoundsException` or `StringIndexOutOfBoundsException` pending; the last region inside is none |
| `strings` | UTF-8 both ways with Cyrillic, UTF-16 through `GetStringCritical` and `NewString`, `GetStringRegion` and `GetStringUTFRegion` |
| `callback` | Java called from C on the calling thread: an interface method (a lambda's), a static method with a `float`, a static `long` field read and written, an object made with `NewObject` and a field set |
| `throw-native` | `ThrowNew` of a runtime's exception and `Throw` of an object of the app's own exception class, each caught in Java |
| `throw-java` | Java throws inside a call made from C: `ExceptionCheck`, `ExceptionOccurred`, `ExceptionDescribe` (the log shows it with its stack), `ExceptionClear`, its message read |
| `find-class` | `FindClass` of a class that is not there is `ClassNotFoundException` and makes no class (`Class.forName` still fails after); arrays; a framework class |
| `references` | global, weak global and local references, `IsSameObject`, `EnsureLocalCapacity`, `PushLocalFrame`/`PopLocalFrame`, `IsInstanceOf`, `IsAssignableFrom`, `GetSuperclass`, `GetVersion`, `GetJavaVM` |
| `reflection` | `FromReflectedMethod` and `FromReflectedField` on libcore's real `Method` and `Field`, and a call through the identifier; `ToReflectedMethod` and `ToReflectedField` give objects Java can use (`Method.invoke`) |
| `monitor` | `MonitorEnter`: `Thread.holdsLock` says so in between, and no longer after `MonitorExit` |
| `thread` | a thread the library starts with `pthread_create`: `GetEnv` is `JNI_EDETACHED` before `AttachCurrentThread` and after `DetachCurrentThread`, `JNI_OK` between; attached with a name, `Thread.currentThread()` on it has that name and the main group; `FindClass` there finds `java.lang.String` and not the app's class, as on Android, and the class kept from `JNI_OnLoad` serves |
| `thread-daemon` | `AttachCurrentThreadAsDaemon`: `Thread.isDaemon()` |
| `needed-library` | a function of `libnatives_dep.so`, whose constructor has run |
| `asset` | `AAssetManager_fromJava`, `AAssetManager_open`, `AAsset_read` on an asset of the APK; one that is not there is NULL |
| `bitmap` | `AndroidBitmap_getInfo` (64 x 64, stride 256, `RGBA_8888`), `lockPixels`, the pixels written as R, G, B, A, `unlockPixels`; `Bitmap.getPixel` then reads each colour back, a half-transparent premultiplied white as `0x80FFFFFF` |
| `direct-buffer` | `GetDirectBufferAddress` and `Capacity` of `ByteBuffer.allocateDirect`, written in C and read in Java; `NewDirectByteBuffer` over the library's memory, read and written in Java and seen in C |
| `configuration` | `AConfiguration`: the surface's density and size in dp, and the API level the framework's `Build.VERSION.SDK_INT` says |
| `library-folder` | `ApplicationInfo.nativeLibraryDir` holds both libraries as real ELF files; `BaseDexClassLoader.findLibrary` gives the path there |
| `loaded-once` | `System.loadLibrary` and `System.load` of the library again: `JNI_OnLoad` is not run again |
| `own-file` | a library the app copied into its files folder under a name the APK does not have, loaded by its path |
| `missing-library` | `UnsatisfiedLinkError` in Android's words: `...PathClassLoader[...] couldn't find "libnothere.so"` (libcore's own), and `dlopen failed: library "<path>" not found` for `System.load` |
| `missing-method` | `No implementation found for void megler.test.natives.Lib.notThere() (tried Java_megler_test_natives_Lib_notThere and Java_megler_test_natives_Lib_notThere__)` |
| `looper-watched`, `looper` | `ALooper_forThread()` on the main thread, a pipe added with `ALooper_addFd` and written: the callback runs on the main thread with a later turn of the Java message loop |
| `system-icu` | `dlopen("libicuuc.so")` and `dlopen("libicui18n.so")` by name, `u_strlen` and `ucsdet_open` found by trying their versioned names as MX Player does, `u_strlen` called: version 60, the Android root's ICU |

and, from outside: the library's own `__android_log_print` line, and the bitmap
it painted on the picture, pixel for pixel where the app drew it. The picture
is `build/java-pictures/natives-1.bmp`.

Then native code on two `SurfaceView`s and a `TextureView` side by side, under
the picture (`WindowTest`, and a second library of the app's, `libwindows.so`
from `jni/windows.c`, which needs EGL and OpenGL ES): the left one drawn with
EGL from a render thread of the library's own, Asphalt 9's way ("A SurfaceView
drawn by native code" below), the middle one in software, the `TextureView`'s
texture in software and then once with EGL. Each frame is a colour with its top
left quarter drawn in another, so the four quarters of the view say where the
frame went and which way up; the test looks at the window after each phase the
app says and taps for the next. 31 checks more (78 in all), twice:
with the test's shell composing the window and with the core composing it
(`readback`):

- The window: `surfaceChanged` gets a window of the view's size, format 2
  (`RGBX_8888`); the renderer chooses its configuration out of
  `eglGetConfigs`, makes an OpenGL ES 3 context, gives the window its own
  format back with the geometry, makes a window surface on it of the view's
  size, current for drawing and reading (`eglGetCurrentSurface` gives its
  handle back), and a lock of the window is refused (-22) while EGL draws on
  it. Thirty frames are in the view's place in the window, seen through the
  hole the right way up: the top left quarter yellow, the rest magenta.
- The right view, `ANativeWindow_setBuffersGeometry(32, 16, RGB_565)` and
  `ANativeWindow_lock`: a 32 x 16 buffer of stride 32 in format 4, written at
  its stride and posted; its four colours in the view's quarters, scaled.
- The `TextureView`'s texture through a `Surface` made on it: a window of the
  texture's size, `setBuffersGeometry` to `RGBX_8888`, the buffer of that
  format and stride, its four colours in the view's quarters; after the first
  tap one frame drawn with EGL on the main thread, read back and given to the
  texture the right way up (its top left quarter red, the rest green), and
  still so, scaled, when the `TextureView` is wider later.
- A fixed size of half the view's (`setFixedSize`): `surfaceChanged` gets the
  same window, whose size is now that; the renderer's surface is that size from
  its next swap (`eglQuerySurface`), and the picture the same, scaled.
- The view taken out of its layout while the renderer swaps: `surfaceDestroyed`
  lets go of the window without waiting; the window is no longer valid; the
  next swap is refused with `EGL_BAD_NATIVE_WINDOW` and the window's width is
  -19; the renderer destroys its surface. The right view, now the whole row,
  drawn again at its new size.
- The view put back: a new window, the renderer's new surface 64 x 32 as the
  geometry it asked for says, and that in the view's place, scaled (a blue
  quarter, the rest cyan).
- Back: the renderer, swapping when the activity went, ends with it, and nothing
  threw. Pictures `natives-2.bmp` to `natives-5.bmp`.

The linker's part has a test of its own in the core's tests (`test_guest_kinds`
in `core/tests/mg_tests.c`, 15 checks, both ARM runs): two libraries named
`libt_same.so`, one in the folder that stands for the app's and one in a folder
that stands for the Android root's, and a library of each kind that needs
"libt_same.so". The runtime's are loaded first. Each gets the one of its own
kind; a weak import of a name that only the other kind exports stays unbound on
both sides; `dlsym(RTLD_DEFAULT)` sees the app's only; neither kind can be
loaded from the other's place.

On the phone (0.1.0.138's Self-test) two of its checks failed, and only there.
The linker was not at fault: whether a library is the app's or the runtime's
is a flag set by whoever asked for it, and libraries are found by file name,
never by comparing paths. The package left the runtime's two out:
`tools/build-appx.sh` copied `build/guest/armeabi-v7a/*.so` into `selftest`
but not the folder `runtime` beside them. It now copies that folder too and
stops if any library `tools/build-guest.sh` made is missing from the package.
The test checks first that the runtime's folder has its library and says so
when it does not. A dlopen of the system's ICU (MX Player) does not depend
on this. It finds the copies libcore's start loaded from the package's
`android/lib`, as `libjavacore.so` needs them, and that folder was in the
package. The same Self-test shows libcore's native libraries loaded on the
phone.

### An app in several APKs (both hosts; the app under qemu-arm)

`core/tests/app/splits` is built as Google Play has an app built from an
Android App Bundle: a base APK with none of the app's native code, none of
its pictures for one density and none of its strings for one language, and
eleven splits -- for three processors (only `armeabi_v7a` with the real
library; `arm64_v8a` and `x86` carry the same file under their names), three
densities (`xxhdpi`, `mdpi`, `ldpi`, made by aapt2's `--split`, which puts
every density's pictures into each, as bundletool does), three languages
(`en`, `ru`, `de`), a feature split `extra` with a class of its own and an
asset pack `pack` with a file under `assets/pack/`. `tools/build-split-app.py`
(called by `tools/build-test-app.sh` for an app folder with a file named
`splits`, which lists them) bundles them as `splits.apks` in bundletool's
layout (`splits/base-master.apk`, `splits/base-xxhdpi.apk`..., a standalone
APK and a `toc.pb`) and `splits.xapk` in APKPure's (`megler.test.splits.apk`,
`config.xxhdpi.apk`..., `icon.png`, `manifest.json` with the splits and an
expansion file, `Android/obb/megler.test.splits/main.7.megler.test.splits.obb`).

`mg_tests --bundles` (`core/tests/mg_bundle_tests.c`, on both hosts, 125
checks) unpacks both as the shell does, for 320 dpi in English and Russian:
the same seven splits kept from each (`config.armeabi_v7a`, `config.en`,
`config.mdpi`, `config.ru`, `config.xxhdpi`, `extra`, `pack`) and the others
left out with their reasons (another processor, by its ABI; another density;
another language; the standalone APK), the record, no work files left, the
progress counted to its end, the expansion file in the app's own
`Android/obb` after the .xapk and nowhere after the .apks, the icon found in
the density split (the base has no picture of it) and the bundle's own
`icon.png` kept; the report of the app as one (`armeabi-v7a` used, the three
ABIs listed, the library of the split listed, Java, runnable); a second
unpacking with Russian no longer wanted, which leaves neither that split nor
a stale file of the folder's; a damaged bundle and one with no base, which
leave nothing; and a bundle made by the test from the app's APKs with SAI's
names and only the arm64 split, whose report says it cannot run because
"the bundle it came in has native code for arm64-v8a only". Then the three
real bundles the phone's owner has, where they are copied into
`build/bundles` (not in the repository; the PC takes half a second for BiP,
qemu two):

| Bundle | Kept | Left out | Report |
|---|---|---|---|
| BiP 4.2.42-HEAD (.apks, 144 MB) | base (185 MB), `config.armeabi_v7a`, `config.xxhdpi`, `config.xxxhdpi` | three processors' splits, four densities' | `com.turkcell.bip`, 4450, `armeabi-v7a`, Java, upright, runnable |
| MangaDex 1.0.0.0 (.xapk) | base, `config.en`, `config.ru`, `config.xxhdpi` | fifteen languages | `org.mangadex.mangadex`, Java with no native code, runnable; the icon a PNG of the density split |
| Magnit 8.107.1 (.apks with manifest.json) | base, `config.en`, `config.xhdpi` | `config.arm64_v8a` | `ru.tander.magnit`, not runnable: "This app has no 32-bit ARM code: the bundle it came in has native code for arm64-v8a only..." |

`mg_tests --java splits <bundle>` (qemu-arm, twice: from the .apks and from
the .xapk, 32 checks each) unpacks the bundle into its data folder as the
shell does, for the screen's density, and starts the app from the base and
the folder through `megler_run`. The app says in thirteen steps what it found:
`ApplicationInfo.splitNames` sorted as Android sorts them, each of
`splitSourceDirs` beside the base as `/data/app/megler.test.splits-1/split_<name>.apk`
and an APK a `ZipFile` opens; `PackageInfo.splitNames` the same;
`getPackageCodePath()` the base; `System.loadLibrary` of the library that only
`config.armeabi_v7a` has, and its answer; a picture only the density splits
have, at the size its density gives it here (16 px for 24 at 480 dpi on a
320 dpi screen) and of its colour, and one of the mdpi split; the string of
`config.en` for this English phone, that of `config.ru` through a
`Resources` made for Russian, and the base's for German, whose split was left
out; an asset of the base and one of the asset pack, both listed in the
assets' root; the feature split's class through the app's class loader; and
the expansion file in `getObbDir()` after the .xapk. From outside: the core's
line that the app is in eight APKs, the library placed and loaded from the
split, and the density split's picture in the window, magenta in its middle
(`build/java-pictures/splits-apks.bmp`, `splits-xapk.bmp`).

And one real app from its bundle, by hand: BiP through `mg_tests --java app`
with its .apks (not run by default; "Real apps" below), unpacked for 320 dpi
(base, `config.armeabi_v7a`, `config.hdpi`, `config.xhdpi`). It starts as an
app in four APKs, its base's thirty DEX files on the class path, and loads
its native libraries out of the split as it asks for them -- Crashlytics',
React Native's and Hermes' (`libc++_shared`, `libfbjni`, `libjsi`,
`libreactnative`, `libhermes`) through SoLoader -- connects to its servers, and
ends in its first activity's `onStart` on its own theme engine's lookup of an
attribute ("color com.turkcell.bip:attr/themeHeaderBackground not found in
theme: Day"). That attribute and the styles that give it are in the base's
own table, not in a split, so this is a matter of themes for another day,
not of splits.

### OpenGL ES from Java (both hosts; the app under qemu-arm)

There is no OpenGL ES under qemu or on this PC: no ANGLE for either (the
package's is gecko-w10m's ARM32 build, which loads only on the phone, and
nothing on the PC offers an x64 one). So the tests give the core a stand-in
that draws (`core/tests/mg_gl_standin.c`, through `mg_egl_set_provider`, as the
native app's test does with its recording stand-in): EGL with contexts of
OpenGL ES 2 and 3 and pbuffers in memory, `glClear`, triangles of one colour
(the program's `vec4` uniform with "olor" in its name, positions from the
attribute with "osition" in it), shaders whose attributes and uniforms are read
out of their source, `glReadPixels` as R, G, B, A or B, G, R, A; a context of
version 1 made as ANGLE makes it, with a configuration alone (refused without
one with `EGL_BAD_MATCH`), and in the runs whose shell refuses the frames
("readback") refused, as an EGL without OpenGL ES 1 refuses it; and every
surface with a keyed mutex, as gecko-w10m's ANGLE gives every pbuffer, what is
done with one that is not held dropped (docs/ARCHITECTURE.md, "Keyed
mutexes"). Every Java app of the tests runs with it.

**The natives, called as the runtime calls them** (`core/tests/mg_gl_tests.c`,
in both suites on both hosts; 86 checks on libcore, 67 on the built-in
java.lang, which has no `java.nio`):

- **All 832 natives** the framework's `GLES10`, `GLES10Ext`, `GLES11`,
  `GLES11Ext`, `GLES20`, `GLES30`, `GLES31` and `GLES32` declare are bound:
  793 generated by `tools/gles-natives.py`, 39 by hand (seven
  `_nativeClassInit`, sixteen that return or take strings or buffers, nine
  that Android's own glue leaves "not yet implemented" or "deprecated" and that
  throw what Android throws, and the rest). The generator says which and why
  when it runs.
- Values, and floats reaching the function as floats; an array and its offset
  (the element at the offset is the first written, the one before it is
  untouched); Android's `IllegalArgumentException` for a null array, a
  negative offset, an offset past the end, a null buffer where C would write;
  strings in (`glShaderSource`, `glGetAttribLocation`) and out
  (`glGetString`, the logs, the shader's source, an active attribute's name
  with its size and type into arrays at their offsets, and the generated form
  with a `byte[]`).
- `java.nio`: a direct `FloatBuffer` from `ByteBuffer.allocateDirect` in the
  native order (its address), its position counted in its own elements, a
  `FloatBuffer.wrap` and an `IntBuffer.wrap` with a position (the array's own
  memory), a view of a heap `ByteBuffer` (Android's "Must use a native order
  direct Buffer"), `glReadPixels` into a direct `ByteBuffer`.
- **The window**: a views' picture with a hole, a row of black at half alpha
  in it; a frame swapped before the views have shown the hole is not shown;
  then the frame under the hole, the half-black row the views' black plus
  127/255 of the frame, OpenGL's rows bottom up (a triangle drawn at the top
  left in OpenGL's terms is at the view's top left); a new frame shown as the
  view's rectangle alone; the views changed in part over the hole; the app's
  framebuffer object and (OpenGL ES 3) pixel pack buffer bound at the swap,
  read past and bound again after; the same read back as R, G, B, A; a
  surface of half the view's size scaled into it; the view moved; the view's
  surface gone. And a thread swapping forty frames while the views change a
  hundred times on another.
- **What a swap costs** at 720 x 1280 with a `GLSurfaceView` of 720 x 1182
  under the views (AIDA64's main screen), on this PC: 1.45 ms of the swap's
  own work, of which 1.27 is the stand-in's read-back (a copy of 3.4 MB of
  memory with its bytes swapped), 0.12 composing (the hole's rows are copied
  as they are), 0.06 the test's shell copying the rectangle; a picture of the
  views composed over the frame 0.9 ms. The phone's read-back is ANGLE's,
  which waits for the GPU to finish the frame: the log line
  `java: OpenGL frames ...` (every five seconds while frames are shown) says
  it there.

**The test app `gl`** (`core/tests/app/gl`, 49 checks under qemu-arm, 480 x
800 at 160 dpi): a `GLSurfaceView` over the top half with a magenta label
over its bottom right corner, a `SurfaceView` over the bottom half, and an
EGL14 pbuffer on a thread of the app's own.

- `ActivityManager.getDeviceConfigurationInfo` says OpenGL ES 3.0, what a
  context can be made of (the stand-in has 3).
- EGL14 on a 16 x 8 pbuffer: display, configuration, a version-2 context,
  `glGetString` of the vendor, renderer, version and extensions,
  `glGetIntegerv` into an array at an offset, a clear read back into a direct
  `ByteBuffer` (255, 0, 128), `eglQuerySurface`, `eglQueryContext`, the
  current context the one made and none after release.
- The renderer on `GLThread 1` with its context current: `onSurfaceCreated`
  (a configuration of 8 bits and depth 16, the default chooser's exact match),
  the GPU's strings through `GLES20` and through the `GL10` it is handed, two
  shaders compiled and linked, `onSurfaceChanged` with the view's size,
  `onDrawFrame` clearing to blue and drawing a yellow triangle from a direct
  `FloatBuffer`.
- `RENDERMODE_CONTINUOUSLY`: thirty frames in 505 ms (paced at sixty a
  second). Then `RENDERMODE_WHEN_DIRTY`: no frame in a second, exactly one for
  one `requestRender`; an event queued with `queueEvent` runs on `GLThread 1`
  with the context current.
- A thread of the app's on the `SurfaceView`, as older apps do: `EGL10`, six
  configurations for "565 with depth 16", one of them exactly 565, a context
  with no attributes (version 1, made with the configuration of the surface,
  as ANGLE makes it: "OpenGL ES-CM 1.1 (stand-in)"; in the readback run, where
  the stand-in has no version 1, 2, and the log says so once), a window
  surface on the `SurfaceHolder` the view's size, `GL10` clearing to green,
  five swaps.
- The picture: the frame under the hole, the triangle the right way up, the
  label over the frame, the bottom view green (`build/java-pictures/gl-1.bmp`).
- `getHolder().setFixedSize` to half the view's size: a new surface,
  `onSurfaceChanged` with that size, the frame scaled into the view
  (`gl-2.bmp`).
- Paused by the shell: no swap in half a second; resumed: a new context
  (the app did not ask to keep it), `onSurfaceCreated` and `onSurfaceChanged`
  again, frames again.
- Back: the GL thread ends with the view, the app's thread with its surface.
  No exception anywhere; of 35 pictures 34 were handed over as a part, and no
  pixel changed outside the part said.

### A database (both hosts; the app under qemu-arm)

SQLite is compiled into the core where its amalgamation is
(`build/sqlite/sqlite3.c`, SQLite 3.54.0, which `tools/sqlite-fetch.sh` puts
there; it is not part of the repository). See "Databases" below for how.

**The core's own test** (`test_sqlite` in `core/tests/mg_tests.c`, 39 checks,
in every run of the core's tests on both hosts, and so in the phone's
self-test) uses SQLite's C API on a file in the scratch folder: the file is
made only when asked; it is a file of the host's with SQLite's header and no
journal left beside it; a statement with an argument; a transaction rolled
back, whose journal is there while it is open and gone after; a second
connection to the same file, which reads beside a writer that has not written
yet, is refused (`SQLITE_BUSY`) while that one holds its lock, and writes
after; a reader in the middle of its rows keeps a writer from committing;
`PRAGMA journal_mode=WAL` answers `delete`; closed and opened again read-only,
what was committed is there and a write is `SQLITE_READONLY`; a database in
memory with a full-text table (FTS4); the clock.

**The app** `core/tests/app/data` goes through `android.database.sqlite` as an
app does, 16 steps, each a line in the log; what each expects is what
Android's SQLite gives. `open`: a `SQLiteOpenHelper` whose `onCreate` runs on
the first start and not again, the file in `/data/data/<package>/databases`.
`insert` with `ContentValues` (text, a null, an integer, a real, a blob), row
ids from `AUTOINCREMENT`. `query` with a selection and arguments, ordered,
read through a `Cursor`. `types`: `Cursor.getType` for each of the five, a
blob byte for byte, an integer read as a string and a real as an int.
`update` and `delete` with their counts. `rollback`: a transaction ended
without `setTransactionSuccessful` leaves nothing. `commit`. `constraint`:
`insertOrThrow` of a duplicate is `SQLiteConstraintException`, `insert`
answers -1. `statement`: `compileStatement`, `simpleQueryForLong`,
`executeUpdateDelete`. `text`: Cyrillic, a euro sign and an emoji go in and
come out, `length()` counts characters, `COLLATE LOCALIZED` exists. `bad-sql`:
`SQLiteException` with SQLite's "syntax error". `memory`:
`SQLiteDatabase.create(null)`. `upgrade`: a second database made at version 1
and opened at version 2, `onUpgrade` once, the column it adds there. `left`
and `kept`: the app leaves three rows and counts its runs; `tools/build-tests.sh`
runs it twice, the second time on the data folder of the first, and the
second run must find the rows of the first and say that it is run 2.
With the storage's, the battery's and the time zone's steps (below), one
for a popup list, three for TLS, one for the key store's key pairs ("On
libcore"), one for the list of decoders (empty every way it is asked for,
`createDecoderByType` an `IOException`, and no line that a class is missing)
and seven for the network (docs/ARCHITECTURE.md, "The network": names,
`HttpURLConnection` against a `ServerSocket` of the app's own,
`DatagramSocket`, a `ServerSocket` closed under the thread in its `accept`, a
`java.nio` selector, `NetworkInterface`, `ConnectivityManager`), 55 checks in
the first run and 56 in the second. The popup:
a `ListPopupWindow` of three rows that wraps its
content, shown under a strip at the top of the window, has all three rows
inside its list and clear of the list's padding (160 high: three rows of 48
and 8 above and below).

On the phone the proof is QuickShortcutMaker, which opens its database when
its Favourites or History tab is first shown; under qemu the database opens
and the tabs show their empty state (`build/java-pictures/qsm/app-4.bmp`).

### The storage as an app sees it (the app under qemu-arm)

The same app, `core/tests/app/data`, then asks about the phone's storage the
ways an app does, seven steps more. Paths are checked as Android names them,
never by where they are on the host. Where the storage is and what is in it
from the start is the core's (`mg_run_files`, the mount table in
`bionic/mg_libc_io.c`; docs/ARCHITECTURE.md); what is tested here is that
Java's calls arrive there.

- `environment`: `Environment.getExternalStorageDirectory()` is
  `/storage/emulated/0`, the public Download folder is under it, the state is
  `mounted` (asked plainly and of a path), emulated and not removable;
  `getDataDirectory()` is `/data`, `getRootDirectory()` `/system`,
  `getDownloadCacheDirectory()` `/data/cache`.
- `own-folders`: `getExternalFilesDir(null)` and `(DIRECTORY_PICTURES)`,
  `getExternalCacheDir()`, `getObbDir()`, `getExternalMediaDirs()` are
  `Android/data/<package>/files`, `.../files/Pictures`, `.../cache`,
  `Android/obb/<package>` and `Android/media/<package>` under the shared
  storage, each there once asked for, and the Activity and the application
  context agree.
- `cache-cleared`: the app's cache cleared between the runs, as Megler's app
  page (Clear Cache) and Android's app info screen (Clear cache) ask for it:
  the test's C side asks the core before the second run
  (`megler_ask_clear_cache`), and the core clears it as the app starts
  (`mg_run_clear_cache_if_asked`, said in the log). What the first run left in
  `getCacheDir()`, `getCodeCacheDir()` and `getExternalCacheDir()` is gone in
  the second, the folders there; what it left in `getFilesDir()` and
  `getExternalFilesDir(null)` stays.
- `volumes`: `StorageManager.getStorageVolumes()` is one volume, the primary:
  "Internal storage", emulated, not removable, mounted, with no UUID. The
  hidden `getVolumeList`, `getVolumePaths` and `getVolumeState`, called by
  reflection as an old file manager calls them, give the same volume, its
  path, and `unknown` for a path that is none. `UUID_DEFAULT` is Android's.
- `space`: `StatFs` on the shared storage and on `/data`: a total of at least
  a gigabyte, free space within it, and the same answer whichever way it is
  asked -- the long calls, the old int ones multiplied as longs,
  `java.io.File`. `StorageStatsManager.getTotalBytes(UUID_DEFAULT)` is not
  less than what is there and less than twice it; `getFreeBytes` within it.
- `permissions`: Android's runtime permissions ("Permissions", in Design),
  asked five ways (`checkSelfPermission`, `checkCallingOrSelfPermission`,
  `checkPermission` with the app's own pid and uid,
  `PackageManager.checkPermission`, `getPackageInfo`'s
  `requestedPermissionsFlags`) and `shouldShowRequestPermissionRationale`.
  Before the request: `VIBRATE`, normal, granted with the install;
  `READ_CALENDAR`, not in the manifest, denied; `WRITE_EXTERNAL_STORAGE`,
  asked for only up to Android 9 (`maxSdkVersion` 28), denied and not among
  `requestedPermissions`; `READ_CONTACTS` denied with no rationale, a group the
  shell has nothing behind and would grant with no one asked, which the user
  refused in the app's App Permissions; the dangerous ones not yet in the
  first run, and in the second as the first left them; `getPermissionInfo`
  says `CAMERA` is dangerous, of the group `CAMERA`, and `VIBRATE` normal.
  Then one request of ten (`READ_CONTACTS` told no, nobody asked), and a
  second while it is answered, told at once it was cancelled
  (empty arrays). The C side answers for the user (`system_permission`, the
  shell's `megler_system_host.permission`, its answers kept in a file of the
  data folder): every group allowed but the camera. The answer comes later,
  on the main thread: in the first run, the user asked about the storage, the
  messages, the phone, the calls and the camera, one after another, the
  activity paused around the answer and resumed after it, as Android's
  permission dialog does; with the rationale shown the camera is asked for
  again and refused for good, and a third request asks no one and is answered
  unpaused. In the second run no one is asked. Then a platform `Fragment`'s
  request, answered to the fragment. The report the shell reads of the app
  names its permissions, the last Android each is asked on and its receiver of
  Google Play services' SMS Retriever (the test's `start`); the microG test
  app's report finds the SMS Retriever in its code, which registers the receiver
  at run time, and the other test apps' name it nowhere.
- `download`: a file written to the public Download folder is there, reads
  back, is listed in its folder, has the same length by the path's other
  name (`/sdcard/Download/...`), and the second run finds what the first
  wrote.
- `list-root`: `File.listFiles()` of `/storage/emulated/0` has the folders a
  phone's storage has before any app runs -- Alarms, Android, DCIM,
  Documents, Download, Movies, Music, Notifications, Pictures, Podcasts,
  Ringtones -- each a directory, the root readable and writable, and the
  app's own `Android/data/<package>` under it. This list is what a file
  manager's first screen is made of (Apktool M's drawer, in "Real apps").

**A shared writable file mapping is written back.** `mmap` with
`MAP_SHARED` and `PROT_WRITE` gives the app the layer's own copy of the
file's pages, and nothing wrote them back: MMKV and MX Player's mx-kv keep
their stores in such a mapping, and every setting they wrote was lost at the
next start. Now the pages the app still has are written to the file at
`msync`, at `munmap` (of the part unmapped) and when the app ends
(`mg_mem_write_back`, from `mg_run_ended`), through the file the mapping was
made of, held apart from the descriptor the app closes. Not as each byte is
written, as Linux would: another process does not exist to see it, and a
second mapping of the same file in this one does not see the first's writes
until one of those, and then it does. Over memory already mapped (`MAP_FIXED`)
it is still a private copy, and the log says so. The storage tests map a file
shared and writable, close its descriptor, write into the mapping, and read
the file back after `msync` and after `munmap`.

The file is read and written with the mappings' lock let go (`write_back_out`):
a chunk of the mapping's pages is copied under it, and compared with the file
and written where it differs past it, one write-back at a time under a lock of
their own, which nothing else waits for. It was all done under the mappings'
lock, which every `mmap`, `munmap`, `mprotect` and `madvise` of the app's and
the fault handler's mending of a page wait for -- every 5 s for each such
mapping, as long as the phone's flash took, which is far longer while another
thread puts a file on the disk (Viber's hang report, 0.1.0.669, named the
write-back thread as the lock's holder). `mg_tests`' "a shared file mapping is
written back with the mappings' lock let go" maps memory on another thread while
`msync`, the 5-second pass, the suspension's and `munmap` each read and write
their file, and finds the lock free.

Nothing is waited for under the mappings' lock, the descriptors' lock among
them. A thread that holds the descriptors' lock -- a pipe's read copying its
bytes into the app's buffer, `epoll_wait` writing its events, `select` reading
its sets -- and meets a page the app gave back with `madvise` is mended by the
fault handler, which takes the mappings' lock. The write-back took its own hold of its file under the mappings'
lock, and `mmap` of a shared file its hold of the descriptor's, and each hold
took the descriptors' lock: such a thread and the fault's would each wait for
the other for good, and every `mmap`, `munmap`, `mprotect`, `madvise` and
descriptor of the app's after them -- an app standing still with the mappings'
lock held by its write-back. The write-back's hold is a count now, taken
without the descriptors' lock (the mapping's own hold keeps the file while the
mapping is there), and `mmap` holds the descriptor's file before it takes the
mappings' lock. Should the descriptors' lock be taken by a thread that holds the
mappings' lock again, the log says where at once ("io: the descriptors' lock is
taken in ... by a thread that holds the mappings' lock"). `mg_tests`' "the
mappings' lock is never held waiting for the descriptors' lock" reads a pipe
into a page given back, then holds the descriptors' lock while `msync` writes
back on another thread: the write-back lets the mappings' lock go before it
waits, another thread's `mmap` goes through, and a page given back, touched with
the descriptors' lock held, is mended; and `mmap` of a shared file, `msync`,
the 5-second pass, the suspension's and `munmap` take the descriptors' lock
under the mappings' lock no time. Before, the test found the mappings' lock
held by the write-back for as long as the descriptors' lock was.

**A file mapped shared shows what is written to it.** A shared mapping of a
file that is the layer's copy (any but a read-only view from a read-only
descriptor) holds the file, and what is written to the file through any
descriptor of it -- `write`, `pwrite`, by number or not -- is copied into the
mapping as it is written (`mg_mem_file_written`), its pages made writable
for the copy where they are not and given their access back; what a
writable mapping writes back reaches the file's other mappings the same way.
Before, the copy stayed as it was at `mmap`. Bolt, the database of Psiphon's
tunnel core in WhatsApp's `libgojni.so`, maps its file shared and read-only,
from a descriptor open for writing and past the file's end, writes it with
`pwrite`, and reads its new meta page through the mapping: each transaction
would have seen the one before it. LMDB writes its meta page through a second
descriptor, and SQLite with `mmap_size` reads through a mapping so too. Two
descriptors are of one file when they are of the same guest path. While no
such mapping exists a write looks for none. `test_mmap_file_written` maps a
file as Bolt does and writes it at an offset, across a page's edge, at the
descriptor's position, grown through another descriptor, with a page
unmapped, and through a writable mapping's `msync`; with the copies not kept
in step six of its checks fail.

**Names Windows refuses.** Android allows any character in a file's name but
'/', and apps use that: MX Player's Amazon SDK names a database after an ARN,
`arn:aws:remote-config:us-west-2:...:apiz0o87_ConfigurationCache.db`, and on
the phone SQLite could not open it, over and over, and each time a thread of
the app died of it. Windows refuses `< > : " \ | ? *`, the characters 1 to
31, a name ending in a dot or a space, and its device names (CON, NUL, AUX,
COM1 ...). Such names are now given to Windows with each of those characters
as the character U+F000 above it -- as Cygwin and WSL do -- and read back as
they were (`mg_name_to_host`, `mg_name_from_host`, `core/src/host/mg_host.h`):
by the Win32 host layer for every path it hands Windows and every name it
lists, where a guest's path is joined to a host folder on Windows (so that a
backslash stays part of a name), and for the phone's folders by the core
before the shell's storage host is asked, so that the shell gets names
Windows.Storage takes and maps nothing itself. A name that needed nothing
is as it was. The storage tests make fourteen such names, the ARN among
them, in the app's own folder and in a link: written, read back, listed
exactly as named, stat'ed, renamed (to the name with a dot more) and
removed, on both hosts and through the shell's own Windows.Storage host
(`mg_tests --storage-windows`).

Paths can be longer than Win32 takes as they are: 259 characters for a
file, 247 for a folder. Instagram's Superpack unpacks 344 libraries into
`lib-compressed` in the app's data folder, and on the phone one of them,
`libxplat_js_react-native-github_packages_react-native_ReactCommon_react_nativemodule_intersectionobserver_intersectionobserverAndroid.so`,
is 262 characters with that folder's path in front; Superpack stopped there
with "could not open file to write" and the app ended. The Win32 host layer
now hands Windows such a path, when it is a drive's absolute one, as `\\?\`
and the whole path, its `.` and `..` parts resolved, which Win32 takes up to
32767 characters (`long_path` in `mg_host_win32.c`); shorter paths go as
they were. The core's host test makes a folder, a file of more than 300
characters, reads it through a path with `.` and `..` in it, lists, renames
and removes them.

### The SIM

An app is told it runs on a phone: `TelephonyManager.getPhoneType()` is GSM,
it is voice and SMS capable with one modem, and `hasSystemFeature` has
`android.hardware.telephony` (and `.gsm`). Apps that saw no phone type and no
SIM took the phone for a tablet. The SIM is the shell's to say
(`megler_set_cellular`, megler.h; `player/PhoneState.cpp`), from the cellular
connection profile Windows keeps for it: the home operator's MCC and MNC
(`HomeProviderId`), the profile's name for the operator's, the data class for
the network's type (LTE, HSPA, UMTS, EDGE, GPRS), the registration for
roaming. It is said before each app and whenever the network changes. With a
SIM, `getSimState` is ready, `getSimOperator` and `getNetworkOperator` the MCC
and MNC, the operator names the profile's, the countries the MCC's (for the
countries the phone is likely in, else the phone's region), `getNetworkType`
the data class's. With none, the SIM is absent and the countries are the
region's, as before. `megler.Native.cellular` hands it to Java as one line.
The data app's step `telephony` reads it back from the harness's SIM, one of
the US in the first run and of Russia in the second.

### The battery (both hosts; the app under qemu-arm)

The core answers for the battery in one place, `mg_battery_read`
(`core/src/run/mg_run.h`): level and scale, status, what it is plugged into,
health, millivolts, tenths of a degree, whether there is one, and its kind --
the numbers of Android's `ACTION_BATTERY_CHANGED`. Until `mg_battery_set` is
called it is a full, healthy Li-ion battery on mains power (100 of 100, 4200
mV, 25.0 degrees). The shell sets the real one whenever it changes, from any
thread, with `megler_set_battery` (megler.h), whose `megler_battery` has what a
phone with Windows can say -- level, status, plug, whether there is one --
and two that it may leave unsaid: a voltage of 0 becomes one plausible for
the level (3.4 V empty to 4.2 V full), a temperature of `INT_MIN` 25.0
degrees. The scale is 100, the health good, the technology "Li-ion". The
core's own test (`test_battery`, 11 checks, both hosts) reads the default,
sets another, reads it back made safe (a scale of 0 is 100, the text ends),
sets none again; then the shell's battery with what is filled in, a level and
a status that cannot be brought to ones that can, and the count of changes
below.

An app reads it two ways, and the `battery` step of the data app reads both:
the sticky intent (`registerReceiver(null, filter)`) with its nine extras, and
`BatteryManager.getIntProperty` (the capacity is the level as a percentage,
the status the intent's) with `isCharging()`. A receiver registered for the
intent is called with it once, afterwards, on the main thread, as Android
delivers a sticky broadcast; apps that show the battery from their receiver,
which is the usual way, heard nothing before. The harness leaves the first
run on the default and sets another battery before the second (37 of 100,
discharging, unplugged, 3712 mV, 30.1 degrees, "Li-poly"), and checks the
line the app logs in each: what the app reads is what the core has.

**A change is broadcast.** The core counts the times the battery was set to
another level, status or plug (`mg_battery_changes`; set to what it is
already, or to another voltage alone, is not counted). The Java driver looks
at the count where it already looks for a stop asked from outside, in
`Native.pollInput`, which the main loop calls between messages: so it is on
the app's main thread, and no thread of the shell's runs Java. When what is
there differs from what the app was last told, it calls
`megler.Broadcasts.batteryChanged` (the overlay's), which sends Android's
broadcasts in Android's order: `ACTION_POWER_CONNECTED` or
`ACTION_POWER_DISCONNECTED` if the phone went on or off a charger, then
`ACTION_BATTERY_CHANGED` to the receivers registered for it while the app
runs (not to one in the manifest, as on Android). A battery that changed and
changed back between two looks is no news. Not sent: `ACTION_BATTERY_LOW`
and `ACTION_BATTERY_OKAY`; a change of voltage or temperature alone, which
Android does send and an app here has when it next asks.

The data app keeps a second receiver registered for the three actions, and
after its steps the harness does what a shell does: sets a battery on a USB
charger at 80, the same again, one off the charger at 79, one that is only
warmer, one at 78. The receiver must have heard the battery four times in
all -- once when it registered, then 80, 79 and 78, each with the voltage
and the temperature of its own setting -- the charger's going once, its
coming once in the run that started on the battery and not at all in the one
that started on mains, each before the battery's own news, and all of it on
the main thread (5 checks).

**The network's sticky broadcast.** Android keeps `CONNECTIVITY_ACTION` as
it keeps the battery's and gives it to whoever registers for it, the same
two ways; `megler.Broadcasts` does too now (`Broadcasts.connectivity`). The
intent says what `ConnectivityManager` says -- a connected Wi-Fi network --
with Android's extras: `networkInfo`, `networkType`, `inetCondition`, and
`noConnectivity` when there is no connection. Asphalt 9 registers a receiver
for it in its activity's `onResume` and tells its native code the type of
connection (`JNIBridge.SetConnectionType`) from that receiver and from
nowhere else. Never told, the native code took the phone to have no network,
opened no socket, and said "no network" over a loading screen that stood
still (package 275), while the game's Java side reached its server. The data
app's step "connectivity" reads the sticky intent and has a receiver called
with it once, afterwards, on the main thread. A change of network is not
broadcast: the framework says Wi-Fi throughout.

**Ordered broadcasts.** `sendOrderedBroadcast` with a result receiver (each of
its overloads, and `sendOrderedBroadcastAsUser`, from a context or a wrapper)
goes to the receivers one after another, the registered ones of the higher
priority first, each on its thread with the result the one before left
(`getResultCode`, `getResultData`, `getResultExtras`), which it may change or
end (`abortBroadcast`); one that goes asynchronous (`goAsync`) holds the next
back until its `PendingResult.finish()`. Then the result receiver, on its
handler's thread, gets the result as it ended (`megler.Broadcasts.sendOrdered`;
the result itself is kept in the overlay's `BroadcastReceiver`). Before, the
original's context did nothing with a result receiver and its receivers kept no
result: Play Store sends its own receivers one when it takes an install up. The
data app's step "ordered-broadcast" checks the order, the result passed on and
added to from another thread, and the result receiver's thread.

**Who sent a broadcast.** Android 14 tells a receiver who sent the broadcast
(`BroadcastReceiver.getSentFromUid` and `getSentFromPackage`, and its
`PendingResult`'s) when the sender shared it:
`BroadcastOptions.makeBasic().setShareIdentityEnabled(true)`, whose
`toBundle()` goes with the broadcast (`sendBroadcast(intent, permission,
options)`, the ordered ones' overloads with options). The options' bundle
came back empty here and both methods said no one, ever: Messenger sends its
own receivers its broadcasts with its identity shared, and refused each one
that came from no one ("InternalIntentScope: Access denied. ...
StellaContactBroadcastReceiver cannot receive broadcasts from null", 21 times
in a user's run of 673). The options now go into their bundle and come back
out of it (the overlay's `BroadcastOptions`), and every receiver hears a
broadcast with a `PendingResult` of its own (`megler.Broadcasts`), which has
the sender -- the sending context's uid and package (`sharedSender`) --
where the options share it, and no one (`Process.INVALID_UID`, null) where
they do not, as on Android. The data app's step "broadcast-identity" sends
with the identity shared, with options that do not share it, and with none.

### The time zone (the runtime on libcore and the app, under qemu-arm)

The shell names the phone's time zone before it starts an app
(`megler_set_timezone("Europe/Moscow")`, megler.h; `mg_timezone_set` in
`core/src/bionic`), and a Java app's default zone is that zone.

libcore asks for the name in three places (`TimeZone.getDefaultRef`): a
`TimezoneGetter` if the runtime has given it one, the file `/etc/timezone`,
and failing both it says GMT -- which is what every app had here. Android
gives it the getter: the framework's start installs one that reads the
property `persist.sys.timezone` (`RuntimeInit.commonInit`). The same is done
here, by the runtime and not by the framework, so that it holds for anything
that runs on libcore: the getter is a class of the runtime's own
(`megler.runtime.PhoneTimeZone`, `core/src/dvm/art/PhoneTimeZone.smali`),
given to libcore just before `java.util.TimeZone`'s static initialiser runs
(the hook the security providers use), and its one method, a native, answers
the name the shell gave. With none given it answers null and libcore goes on
to GMT as before. Nothing is written to `/etc/timezone` and nothing calls
`TimeZone.setDefault`: an app that sets a zone of its own and then sets
`null` gets the phone's back, as on Android. The property says the same name
(it was fixed at "UTC", and is "UTC" while no zone is named): that is where
ICU's own code and an app's `__system_property_get` look. The framework's
`android.os.SystemProperties` is a table of its own and still has nothing
under that name. Native code's `localtime` is not touched; it has the host's
offset already.

A name tzdata does not have is GMT too, since `TimeZone.getTimeZone` never
fails, and nothing would say so; so the first time the name is asked for the
runtime tries it, and the log has a line: `java: the time zone is
Europe/Moscow`, or a warning that the name is not in the Android root's
tzdata and the app's times are GMT.

Tested twice. In the core's tests on libcore (`test_timezone`, 10 checks,
where libcore's native libraries run: the zones are a file read through
them): none named is GMT; "Asia/Kolkata" is that zone, five and a half hours
on, and the property says it; a name that is no zone's is GMT with nothing
thrown; none again is GMT and "UTC". And in the data app, the `timezone`
step: two fixed instants, a winter's noon and a summer's by Greenwich,
formatted in the default zone, and `TimeZone`, the date format, a `Calendar`
and `java.time` must agree on the zone and the hour. The harness starts the
first run in `Europe/Berlin` and reads `13:00 +0100` and `14:00 +0200` off
the app's line -- the zone's rules, not an offset -- and the second run in
`Nowhere/At_All`, which must start all the same, say `12:00 +0000` twice,
and leave the warning in the log.

### The language and region (the runtime on libcore and the app, under qemu-arm)

The shell names the phone's language and region before it starts an app, as
a BCP 47 tag (`megler_set_locale("ru-RU")`, megler.h; `mg_locale_set` in
`core/src/bionic`): the first language in Windows' settings, with its script
if it names one, and the phone's home region (`player/PhoneState.cpp`), or
the tag chosen in Settings. With none named, and in place of a tag that does
not begin with a language, it is "en-US", which is what every app had before.

A Java app's `Locale.getDefault()` is that tag. libcore makes its default
locale first of the property `user.locale` (`Locale.initDefault`), and on
Android that property comes from the runtime's command line; here
`VMRuntime.properties()` gives it, and libcore applies it after its own
properties. (Not `System.specialProperties`: where libopenjdk.so runs, that
native is the library's and the runtime's fallback is never called -- the
first attempt put it there, and the apps still had en-US.) The framework's
`LocaleList` and the resources' configuration follow from the default
locale, and so do `TelephonyManager`'s network and SIM country when there is
no SIM: an app that takes the SIM's country as the default for a phone number
gets the phone's region (urent does, and had "us"). With a SIM they are its
country, from its MCC (below, "The SIM"). The properties `persist.sys.locale` and `ro.product.locale`
say the tag, a native app's `AConfiguration` has its language and region,
and the JNI world's `Locale` and Unity's tag are the same.

Tested twice. In the core's tests (`test_locale`): a tag's language and
region apart ("zh_Hant_tw" is "zh" and "TW", "es-419" has a region of three
digits, "de" none), and a tag that is not a language's, or an empty one,
leaves "en-US". And in the data app, the `locale` step: `Locale`,
`LocaleList`, the configuration and `TelephonyManager` must agree. The
harness starts the first run in "en-US" and the second in "ru-RU", and reads
the tag and the country off the app's line.

**Dates in the locale's words.** `android.text.format.DateFormat` and
`DateUtils` are Android's (10 and later), on the ICU of Android 9's
core-libart: a pattern for a skeleton is ICU's `DateTimePatternGenerator`'s
best, the hour as long as the skeleton asks (`MATCH_HOUR_FIELD_LENGTH`), which
is what Android 9 found through `libcore.icu.ICU`; 12 or 24 hours are the
setting `TIME_12_24` where it is set (Megler's settings say 24) and the
locale's own where it is not; `format()` is Android's own letters (`a`, `c`,
`E`, `d`, `h`, `k`, `L`, `M`, `y`, `z`...) with ICU's names, standalone and
narrow forms among them; `formatDateTime` is `formatDateRange` of the instant
(libcore's `DateIntervalFormat`), and the relative times are libcore's
`RelativeDateTimeFormatter`'s ("5 минут назад"), both by reflection, the
plain English of before only where libcore's cannot be had. The pattern had
been the skeleton itself: Messenger's chat list, which gives a medium date
format the pattern for "MMMd, yyyy", read "мая20, 2023" in Russian. The data
app's steps `date-patterns`, `date-format-letters` and `date-utils-locale`
check the patterns for en-US and ru-RU against ICU 60's data (CLDR 32) in
both runs ("d MMM y 'г'." for yMMMd in Russian), and the formats of the
run's own locale.

### The screen resized and turned (the app under qemu-arm)

`core/tests/app/screen`, run with the other test apps by `tools/build-tests.sh
arm` (`mg_tests --java screen`). The test calls `megler_set_surface` as the shell
does when a PC's window is resized or a phone turned, and reads what the app
says. `Main` takes no configuration change itself; `Handled` takes every change
of the screen (its manifest's `android:configChanges`), asks for the screen held
upright (`android:screenOrientation="sensorPortrait"`) and is exported;
`values-w700dp` is a size the app's resources are made for.

1. 480 x 800 at 160 dpi, upright: `Main` types into its field, its halves red
   and blue side by side in the picture; the shell heard -1 (nothing asked).
2. Turned to 800 x 480, 90 degrees: the Application, its `ComponentCallbacks`
   and a display listener hear it; `Main` is relaunched -- paused and stopped
   with `isChangingConfigurations`, its state saved with the field's text, its
   retained object kept, destroyed with `getChangingConfigurations` 0x480
   (orientation and screen size) -- and the one made again has the count, the
   retained object and the field's text back, with a configuration, metrics,
   `Display.getRotation` and the `w700dp` string of the new size; the picture
   is 800 x 480 with its halves where they belong.
3. Narrowed to 750 x 480: the screen's size alone changed and crosses no size
   of the app's resources, so `Main` is neither relaunched nor told (it takes
   no change itself); its views hear the new configuration and are laid out
   at the new size.
4. Narrowed to 600 x 480, across 700 dp: `Main` is relaunched (0x400) and
   says `narrow`.
5. `Handled`, started as another app would start it (`megler_start_activity`),
   covers `Main`; the shell heard 7. Turned upright, 480 x 800: `Handled` hears
   `onConfigurationChanged` and is not relaunched; `Main`, stopped, is left as
   it is.
6. A tap: `Handled` asks for landscape (`setRequestedOrientation`); the shell
   heard 0.
7. Back: `Handled` finishes and `Main` comes back to an upright screen it was
   not made for: it is relaunched now, from stopped (no pause, no stop), with
   its state; the shell heard -1 again.

63 checks. The pictures go to `build/java-pictures` (`screen-1-upright` to
`screen-4-back`).

### Real apps (qemu-arm; nothing here is run by default)

`mg_tests --java app <apk> <data dir> <android root> <picture dir> <verbose>
<WxH@DPI> [tap:X,Y | drag:X,Y,X2,Y2 | wait:MS | back | key:CODE]...` starts
any APK through `megler_run` with the stand-in for the shell's drawing (text is
boxes, PNG is the only picture format), writes a picture when the app is
still and one more after each step of the script, and checks nothing. Verbose
2 adds the framework's touch log, 3 its view dump. Three apps of the user's
(`build/apps`, not in the repository) were run this way at 720 x 1280 and 280
dpi; each starts in under half a minute. Given a bundle of split APKs (a name
that ends in `.apks`, `.xapk` or `.apkm`), it unpacks it first as the shell does, for
the screen's density, and starts the base with its splits ("An app in several
APKs" above says what came of BiP).

Two things for an app that does nothing where it should do something, which
is what a swallowed exception or a call that never happens looks like.
`MG_JAVA_TRACE=1` turns the runtime's trace on: every class as it is linked,
every static initialiser, and a line for every exception thrown, by bytecode
(`dvm: throw <class> in <class>.<method>`) or by the runtime, caught or not.
`MG_JAVA_PROFILE=start` profiles the whole run of `--java app`, from the
app's start to the end of the script, and `MG_JAVA_PROFILE_TOP=<n>` prints
that many methods instead of twenty: with a large number it is the list of
every method that ran and how often, in which a listener that should have
run is there or is not. `MG_JAVA_TIMEZONE=<name>` starts the app in that time
zone, as the shell does on a phone.

**QuickShortcutMaker 2.5.0** (no native code; ActionBarSherlock, Flurry).
Starts to its tabbed main screen; the overflow menu, Settings (a
`PreferenceActivity`, which scrolls) and About open; its database opens when
the Favourites tab is shown; a second start on the same data folder works.
Fixed on the way: `getActionBar()` before `setContentView` (the overlay's
`Activity`), and `Constructor.newInstanceFromSerialization`, which ended its
second start (Flurry reads back with `ObjectInputStream` what it saved).
The overflow menu's popup, which showed a row and a half of its two, shows
both (the overlay's `ListPopupWindow`). Its lists of other apps' activities
are empty because the framework's `PackageManager` knows no other app. Its
four tabs (ViewPagerIndicator's `TabPageIndicator`, a `HorizontalScrollView`
over a `LinearLayout` of tabs with no width and a weight each) are all the
same width, a quarter of what the four labels need together, and
"APPLICATIONS", the longest, is cut at both ends: that is Android's rule for
an app written for Android 7 or later, which this build of the app says it
is (targetSdkVersion 29), and it is left so. See the overlay's
`LinearLayout`.

**Apktool M 2.4.0** (AndroidX appcompat views, jadx and apktool inside;
`libpngquantandroid.so`, `librmdir.so`). Starts through its launcher alias,
shows its licence dialog with its three buttons (Translation, Do not accept,
Accept), and after Accept its main screen: a toolbar and, open over it, the
drawer that is its file manager -- two panes and a third tab, the path
`/storage/emulated/0` with a line about it underneath, the folders there, each
with its date and what it holds ("2026/10/14 01:10, dirs 0, files 0"; the
time is Greenwich's unless the run names a zone, and with
`MG_JAVA_TIMEZONE=Europe/Moscow` it is three hours on), and a
row of five tools at the bottom. Two more dialogs follow the licence (a
choice with a spinner; whether to exempt the app from the battery optimiser).
Settings opens: seven headers, each with its screen of preferences. Fixed on
the way: the launcher `<activity-alias>`, the height of a dialog's content
(the overlay's `LinearLayout`), the content and the headers of a
`PreferenceActivity`, and `Message.obtain(Message)`: until that one, no
button of the licence dialog did anything but close it, the licence was never
accepted, and the drawer, which the app fills on acceptance, stayed empty
(the overlay's `Message`). Both libraries load with nothing unresolved (the APK
report's "2 imports not in the system" are weak references of libgcc's
unwinder to the C++ runtime, null here as on Android without libc++); neither
is called until the app compresses a PNG or deletes a folder, which the test
did not reach. The grey strip 144 pixels wide down the right edge of the main
screen is the app's own: the drawer is 576 wide and open, and the strip is
`DrawerLayout`'s shade over what the drawer leaves uncovered.

**Super S22 Launcher 1.8** (minSdk 21, targetSdk 30; a Launcher3; ironSource,
umeng, the UC crash SDK's `libcrashsdk.so`). It ended on its first screen on
the phone with a `NullPointerException` on `Rect.left` in its device profile:
`AppWidgetHostView.getDefaultPaddingForWidget` answered null (the overlay's).
Under qemu it now shows its welcome screen, and after its button the home
screen: a clock, a weather icon, the search bar, Camera, Launcher setting and
Theme on the page, Phone, Message, Apps, Camera and Browser in the dock, under
a first-run tip whose OK closes it. Fixed on the way: `LauncherApps` and
`LauncherActivityInfo` (the app's own activity had no title, and the thread
that loads the home screen ended), and three things of libc's that the crash
SDK imports (`__page_size`, which it read as a null pointer, `setpgrp`,
`ptrace`). What it lists is what the framework's `PackageManager` knows: the
app itself, and since build 0.1.0.373 the apps of Megler's library ("Installing,
and the phone's other apps"). Its umeng library asks for one HTTPS address
over and over, about 150 times a second, when the name does not resolve.

**Nova Launcher 8.9.4** (armeabi-v7a; Kotlin, Jetpack Compose for its first
run, Firebase, Facebook's and Google's ad SDKs). On the phone (package 373, as
an app's own package) it ended at its start: its `LauncherProvider` stopped on
a `NullPointerException`, the rest of its start was never done, and its
launcher activity then failed on a `lateinit` property that start sets. Run
under qemu at the phone's 1440 x 2560 and 560 dpi, three things of the
framework's were behind it, one after another: an activity with no icon had
none (`PackageItemInfo.loadIcon` answered null; Android falls back to the
application's icon, then the platform's default), a bitmap's bytes were four a
pixel in every config (Nova copies an `ALPHA_8` icon into an array of one byte
a pixel), and `AdaptiveIconDrawable` was an empty stub whose mask was null.
With those fixed it starts under qemu to its welcome screen (Welcome to Nova
Launcher, Get started). Not yet tried on the phone again.

**AIDA64 2.22** (minSdk 19; a `GLSurfaceView` to ask the GPU, a night mode
of its own). On the phone it ran and its detail pages opened from taps on
the main list, but the list itself could not be seen. It was the
`GLSurfaceView`'s hole over it (the overlay's `SurfaceView`); under qemu the
main list shows, fourteen pages on its dark theme, as soon as it starts.
With OpenGL from Java (2026-10-14, on the stand-in that draws): its two
`GLSurfaceView`s (the main activity's and the page list's) get their renderers
started on `GLThread 1` and `2`, the main one reads the GPU's strings
(`OGLES_Vendor = Megler`, `OGLES_Renderer = stand-in for OpenGL ES`, its own
log says), the app recreates its activity as it does once it knows them, and
the list shows as before. Its Display page (tapped at 200,296) lists "GPU
Vendor: Megler", "GPU Renderer: stand-in for OpenGL ES", "GPU Version: OpenGL
ES 2.0 (stand-in)", the extensions one to a line, and "OpenGL ES Version 3.0"
(the device's, which said 3.2 before: the overlay's `ConfigurationInfo`).
On a phone those are ANGLE's.

**Device Info 3.4.3.4** (minSdk 24, targetSdk 37; views, not Compose: no
`androidx.compose` anywhere in its DEX; AndroidX appcompat, fragment,
recyclerview, viewpager2, constraintlayout, preference, work, datastore,
room, biometric, browser; Material 3; Kotlin and coroutines; Google's Mobile
Ads SDK, Play billing and in-app update, Firebase data transport, Facebook,
okhttp, Chromium classes; all of it renamed by R8 into the default package).
Its libraries, `libandroidx.graphics.path.so` and
`libdatastore_shared_counter.so`, are placed and not loaded before it ends.
Its splash screen and the creation of its main activity work; it ends when
the first tab is inflated. In the order met:

1. `megler.Sqlite` -- now there. (WorkManager had ended a worker thread with
   "The file system on the device is in a bad state".)
2. Resources chosen for API 34 against Android 9's platform resources: 6095
   frames of recursion. Fixed (the overlay's `Resources`).
3. **WebP**: `cannot decode res/drawable/android17.webp`, an
   `InflateException` nothing catches: the stand-in decodes PNG only, and
   Windows 10 Mobile's WIC has no WebP codec either. Now decoded in the core
   ("WebP pictures"); the app shows its Dashboard.
4. `Runtime.exec("getenforce")`: `IOException: error=38` (there are no
   processes to start). The app catches it. Now `error=2, No such file or
   directory`, what Android says of a program that is not on the phone.
5. `Class.forName("com.android.internal.os.PowerProfile")`: not there (the
   battery's capacity by reflection). The app catches it.

   Shown wrong on the phone and put right since: the selected tab's label
   all but invisible (the overlay's `GradientDrawable`); "Internal Storage
   -2147483648%, Total: 0.0 GB" (the overlay's `StorageStatsManager`).

6. `NoSuchMethodError: android/view/View.getDrawingTime()J` when the second
   tab was touched (a chart's animation), which ended the app. Added, with the
   other members the app names and the framework lacked ("members added",
   below). The five tabs open now.

and, not blockers: `megler.Sensors` answers nothing, billing, in-app update
and the ads SDK find no Play services, which they are written to survive.
Its GPU (2026-10-14, on the stand-in that draws): the CPU tab (tapped at
528,150) shows "GPU Renderer: stand-in for OpenGL ES", "GPU Vendor: Megler",
"GPU Version: OpenGL ES 2.0 (stand-in)", asked through EGL14 and GLES20 on a
pbuffer of its own (no GL thread is started); its System tab says "OpenGL ES
3.0". It also says "Vulkan: Supported (1.3)", which nothing here offers and
the framework should not let it think (not looked into).

**What stands between these and working, most apps first:**

| What | Blocks | Size |
|---|---|---|
| The app's shared storage as Android lays it out, and `Environment`, `StorageManager`, runtime permissions to match | Apktool M's lists; every file manager | the layer's mounts and the framework's Java: days |
| `PackageManager` knowing more than the app itself (installed APKs in the library) | QuickShortcutMaker's whole purpose | small in Java, a decision for the shell |
| EGL and OpenGL ES from Java (`megler.EGLNative`, `GLES20` over ANGLE) | Device Info's GPU page; any `GLSurfaceView` | done (2026-10-14): "OpenGL ES from Java" above; on a phone not yet tried |
| `megler.Web` (a `WebView`) | ads, licence and help pages in all three | done (2026-10-16): Android's System WebView, Chromium 128, in the app's process ("The WebView") |
| Processes (`Runtime.exec`) | root checkers and file managers probing `su`, `getprop`, `sh` | answered: "no such program" (`error=2`), for every program, `sh` included. An app that wants `getprop`'s output gets the exception, not an empty answer. `UNIXProcess.initIDs` is the runtime's too and does nothing: libcore's set SIGCHLD with `sigaction`, which through Facebook's sigmux hook failed, and the `InternalError` it threw ended Facebook at its first exec (package 487; the data app's "exec" step) |
| TLS (conscrypt's `libjavacrypto.so`) | anything that talks HTTPS | done (2026-10-14): the library is built like the other seven, over Android 9's own `libssl.so` ("The security providers"); TLS 1.2 on loopback and https through Android's HTTP client under qemu; on a phone not yet tried |
| `invoke-custom`, method handles | not met in these three (d8 desugared them) | large |

### The phone's package

`bash tools/build-appx.sh arm --libcore` puts the Android root
(`build/android9/root`) in the package as the folder `android`: 20 files,
58,892 KB, of which ICU's data is 24 MB, the platform's resources 12 (cut down;
Android's own file would be 44), the six DEX files of libcore 11, the
framework's 7.6, the libraries 4.5. `megler_run` finds the folder there for a
Java app, and the Self-test finds it and runs the runtime's tests on libcore
exactly as the second run under qemu does. (The package was 37,745 KB with the
whole resource file; it has not been built here since the file was cut.)
`--java` (2026-10-14: 0.1.0.141, 37,331 KB, built without a warning and not
handed out) also carries the test app `gl` beside `hello` and `demos`, and
"Add the Java Test Apps" adds all three: a `GLSurfaceView`, EGL14 and EGL10
to look at on a phone, where its log says what a swap costs with ANGLE.

**On the phone** none of it was run by whoever wrote this; what the project's
other documents say was seen there (the Self-test on libcore, both test apps
drawn by the shell's Direct2D host and answering touches) is theirs to say.
The Self-test: `bash
tools/build-appx.sh arm --libcore --auto selftest`, then `python tools/phone.py
auto`; its log should show `libcore (Android's boot class path in ...; its
native libraries loaded)`, the line `System.out.println wrote 19 bytes`, and end
in `0 failed`. A Java app: it starts and runs unseen until the shell implements
`present_pixels` and `megler_gfx_host` -- its log (`I/HelloTest: ...` in
`megler.log`) is what would show it alive. What could differ there from qemu: the
Win32 host layer under all of it (files mapped out of the package folder,
64-bit compare-and-swap, thread starts), and memory (below).

### What an AlertDialog cost

The first dialog an app showed took fourteen seconds to appear on the phone and
thirty under qemu, in an app that starts in one. The runtime has a profile for
such a thing (`mg_dvm_profile`: for every method, the instructions executed in
it and under it, its calls, the objects made in it, and for a native method
the time), and the test of `demos` puts a step under it when asked:

```
MG_JAVA_PROFILE=dialog qemu-arm ... mg_tests --java demos <apk> <data dir> <android root> -      (or start, or scroll)
```

What it showed, from the finger on the row to the dialog on the screen:

| | instructions | objects made | where |
|---|---|---|---|
| as it was | 709.9 million | 1,649,768 (141 MB) | 99.5 % under one call: `Resources.getIdentifier` -> `ResTable.indexNames` |
| names indexed a type at a time, each entry once | 5.5 million | 14,065 (1.2 MB) | 39 % in `Resources$Bag.put` |
| and a style's items found by hashing | 2.9 million | 14,103 (1.2 MB) | nothing above 11 % |

and in time under qemu, 30.6 s before and a quarter to two thirds of a second
after (the figure the test prints is larger by 300 ms, which the app waits
before it reports, and by what the test's own counting of instructions costs).

**What was wasteful, in the framework's own Java** (both are fixed in the
overlay, and the framework's files in `core/framework` still have both):

- `AlertDialog.build` asks for the id of one resource by name
  (`getIdentifier("alertTitle", "id", "android")`). `ResTable.identifier`
  answered the first such question about a package by indexing the names of
  the *whole package*: every entry of every configuration of every type, each
  put into one map twice. A name does not depend on the configuration, but
  the platform's table has a configuration for every language and density:
  161,000 entries for some 13,000 resources, 4.3 million reads of an entry's
  offset through a `ByteBuffer`, 322,000 strings built and hashed. Now the
  names are indexed for one type when a name of that type is first asked for,
  and each entry is read once, in the first configuration that has it.
- `Resources$Bag`, a style's items, was two parallel arrays searched from the
  start by `put` and `indexOf`; a style is built by putting its parent's items
  in one by one and then its own, and a theme has nine hundred items and half
  a dozen ancestors. That was 2.4 of the 6.3 million instructions of an app's
  start and 2.1 of the 5.5 million of its first dialog. Now a key's place is
  found by hashing; the arrays and their order are as they were.

Neither was the size of the platform's table as such, and cutting the table
down (above, "What a run costs") changes this step by nothing; it gives back
memory and a third of an app's start.

**An app's start** under the same profile (`demos`, to its first pictures):

| | instructions | objects made |
|---|---|---|
| as it was | 11.95 million | 35,048 (32.6 MB) |
| the platform's resources cut down | 6.35 million | 27,906 (4.6 MB) |
| and a style's items found by hashing | 4.00 million | 27,968 (4.7 MB) |

Before, 40 % of it was reading the headers of the platform table's 3865
configurations and 20 % was `Bag.put`. After, nothing stands out: the most any
method takes is 7 % (`String.hashCode`; then matching resource configurations,
and `ByteBuffer`'s accessors, through which the tables are read a byte at a
time).

**A scroll**: one drag of the list by half the screen and back is some 4300 to
4900 objects (330 to 380 KB) and 0.6 to 0.8 million instructions, in 11 to 20
pictures according to how many of the finger's moves fall into one: 250 to 390
objects, 19 to 31 KB and 39 to 53 thousand instructions a picture. A quarter of
the objects are copies of the touch event on its way down the views, a fifth
are clones of small arrays. No method stands out; under qemu the time of a
picture is the stand-in erasing the screen, which is not the phone's concern.
At sixty pictures a second that is 1.2 to 1.9 MB of garbage a second: **500 MB
in four to seven minutes of scrolling**, which is the number that makes the
collector urgent.

### What a run costs

**How long a start takes, on the phone** (Yandex Maps, package 490): the shell
1.4 s to "session: starting", then nothing in the log for 9.8 s, then 0.4 s for
libcore, 2 s for the app's sixteen DEX files, and from the app's `Application`
on its own code. The same 9 to 10 s were in every app's log (Facebook,
WhatsApp, Play Store, packages 484 to 491). They were the shell asking the
Device Portal, over the loopback, for the phone's whole list of packages, to see
whether Google Play services' own package was still installed
(`appPackage::serviceApk`), before any app could start. That is asked on a
thread of its own now, once a run, and a package found gone is forgotten for the
next start (Megler's own uninstall forgets it at once). And:
- The table of which DEX file defines each class had 16384 buckets; Yandex
  Maps defines 210000 classes and Google Play services 60000 more, so every
  lookup (each class of each DEX file as it was read, each class loaded) went
  through fifteen entries with `strncmp`. It grows now, two entries a bucket at
  most (`cdef_grow`), and a class's name is no longer copied to be looked up.
  The tables of loaded classes and of interned names have 65536 buckets (they
  had 4096 and 8192).
- Google Play services' resource table cut down (see Memory) is kept beside its
  APK in the services' shared folder, named for the table's CRC-32 and the
  languages, and read from there by every app after the first: its 84 MB were
  read and cut at each start, 0.7 s.

What is left is the interpreter running the app's own start, and Google Play
services starting in the same process beside it.

Measured by the runtime's own counts, in the 32-bit build under qemu
(`mg_mem_usage` gives nothing in a test program, so what the *process* takes was
not measured):

| | classes linked | initialised | objects | tables and classes |
|---|---|---|---|---|
| started, libraries loaded, their `JNI_OnLoad` run | 256 | 94 | 660 (81 KB) | 2046 KB |
| after `System.out.println` | 313 | 147 | 978 (131 KB) | 2162 KB |
| after both test programs | 966 | 695 | 63,240 (4751 KB) | 3474 KB |
| every class of the boot class path linked | 7869 | | | 14,578 KB |

A Java app, the same way (`hello`, when its first pictures have been shown
and it has been pressed twelve times):

| the platform's resources in the Android root | classes linked | objects | tables and classes | bitmaps |
|---|---|---|---|---|
| none | 788 | 12,663 (1230 KB) | 6324 KB | 2 (1501 KB) |
| Android 9's own file, whole | 822 | 26,745 (31,799 KB) | 6423 KB | 2 (1501 KB) |
| cut to two languages and two densities (what the root has now) | 903 | 29,180 (4764 KB) | 7376 KB | 2 (1501 KB) |

Android 9's `resources.arsc` is 29 MB, every language the platform speaks, and
the framework's `Resources` reads it whole into a `byte[]`: that one object was
the difference. `tools/dvm-android-root.sh` now has the SDK's `aapt2 optimize`
cut the file to English and Russian and to the densities round a phone's
(`xhdpi`, `xxhdpi`; both lists are the script's to change): the file goes from
43.5 MB to 11.7, its table from 29.1 MB to 1.8, the root from 89.9 MB to 58.9,
and an app holds 27 MB less from its first moment. (The last row also has the
two test apps' later selves, which link a few more classes.)

**Resources read where they lie.** Since package 665 `Resources` does not read
a `resources.arsc` that its APK stores (rather than deflates) into a `byte[]`
at all: `megler.Native.mapApkFile` maps the file's bytes where they lie in the
APK, read only, and gives them as a direct `ByteBuffer`, which `ResTable` reads
as it read the array (`ResTable(ByteBuffer, dynamicId)`; the buffer's reads are
intrinsics, "Calls done in place: intrinsics"). An app built for Android 11 or
later must store the file, and most do; one that deflates it is read into the
heap as before. Spotify's is 49.8 MB: in a `byte[]` it was a fifth of its heap
on a Lumia 650, memory the phone did not have, and the collector ran every
second or two to keep the rest within what was free (package 664). The log
says `java: resources.arsc of <apk> viewed where it is in the APK, <size> MB,
not read into the heap` for each table so read.

Linking is lazy and has to stay so: a class is linked when something first
needs it, and a start links 256 of 7617. Linking them all costs 12.5 MB more on
the phone's word size (21.8 MB on a 64-bit PC) and buys nothing. The 2 MB at the
start is mostly the resolution tables: a pointer for every string, type, method,
field and prototype index of the six files, which could be made on demand. Start-up takes 160 ms under qemu
and 9 ms on the PC (without the libraries). Beside these: the six DEX files are
mapped, not read (11.4 MB of address space, shared with the file cache); the
libraries are 4.5 MB; and **ICU's data, 23.5 MB, is in memory twice**, because
the layer's `mmap` of a file is a private copy and both `libicuuc` and ICU4J
(`android.icu`, on first use of `java.text`) map `icudt60l.dat`. That was
measured (the two mappings were logged) and is the first thing to fix before a
phone with 1 GB runs this: one shared read-only mapping in
`bionic/mg_libc_mem.c`.

### The frame meter (always on; the check under qemu-arm)

Where the main thread's time goes while a Java app draws, in the log, from the
core's side: a line every five seconds while the app draws (at the end of the
first frame after five seconds have passed; nothing while it draws nothing),
and when the app ends the window not said yet and one line for the whole run.
On the phone it is in the same log as everything else. One of `demos` under
qemu, the window with the app's start, the drag of its list by half the screen
and back, and the first dialog:

```
java: frames 6.6 s: 35 drawn, 79.9 ms of work each on average (the longest frame 184.2 without input and messages): Java
79.8 (input and messages 10.1, measure and layout 3.5, draw traversal 66.2), of it in the shell's drawing 64.0, in other
native methods 4.8 (2354 calls), allocating 0.4 (1063 objects), waiting for monitors 0.0, interpreting 10.6 at 20.2 M
instructions/s; 7.6 M instructions on all threads; present 0.1; idle 57%; heap 8 MB (objects 7, nothing collected, 3 more
in these 6.6 s; bitmaps 1); redrawn 100% of the screen a frame
```

How it is had. The overlay's `megler/ViewRoot.java` calls
`megler.Native.frameMark` three times in `doFrame` (a native added by
`megler/Native.java.members`): as a frame begins with its measure and layout,
as its draw traversal begins, and as it ends, with the pixels it drew. At each
mark the driver (`mg_java.c`, "the frame meter") reads the main thread's clock
and the sums the thread keeps all the time: its time in the shell's drawing
(every call of `megler_gfx_host` in `mg_java_gfx.c` is timed, two readings of the
clock a call, `mg_java_gfx_host_ns`), its time in `present`, its time waiting in
`Object.wait` (and so `Thread.sleep`, and the main loop's wait for the next
message), and from `mg_dvm_thread_costs`: its time in native methods and how
many (the outermost of nested ones, timed in `invoke_native`), its time
allocating for bytecode's own instructions and the objects it made
(`alloc_raw`), its time waiting to enter a monitor another thread held (timed
only when `mg_lock_try` fails), and its instructions executed (the
interpreter counts in a local and hands the count to the thread at each call
of a native method and each return to C). What passed since the mark before
is added to the part of the frame it was spent in. Nothing is logged per call
or per frame. The cost is:
- an addition per instruction;
- two readings of the clock per call of the shell's drawing;
- three native calls a frame;
- for native methods and allocations, almost nothing (see below).

**Native methods and allocations are not timed on every call.** At first they
were, each with two readings of `mg_time_monotonic_ns`, and that made the meter
expensive:
- On the PC two readings were over half of a call of `String.charAt` (64 ns,
  of which QueryPerformanceCounter took 57% by the benchmark's sampling
  profile).
- On the phone each reading also does three 64-bit divisions, which 32-bit
  ARM does in a library routine.

Now:
- The times are kept in the host's raw ticks (`mg_time_ticks`, no arithmetic;
  `mg_ticks_ns` converts when `mg_dvm_thread_costs` is read).
- Each native method's first 64 calls are timed, and every 64th after that.
  A method whose average stays under 4 µs has its other calls counted at that
  average. A longer one (drawing, files, the network) is timed every time.
- One allocation in 32 is timed and counts for 32. A collection that runs
  inside an allocation is timed whole.

Benchmark, x64 and then qemu, ns a turn: `String.charAt` 64.2 to 25.8 and
712 to 411; `String.equals` 2888 to 1114 and 28195 to 14966; a small object
97.8 to 68.8 and 1277 to 1236; `System.arraycopy` 62.9 to 30.3 and 726 to
467.

What each figure is, all in milliseconds per frame except where said:

- **frames**: the time the line covers; frames drawn (that presented
  something), and of how many when some drew nothing.
- **of work each on average**: the main thread's busy time per frame (not
  waiting), input and messages between frames included; **the longest
  frame**: from its layout to its end, the time between frames not included
  (so in a line of one frame with much input before it the average can be
  the larger).
- **Java**: that busy time less `present`, in its three parts: **input and
  messages** (between frames: the main loop, input, the app's messages and
  timers), **measure and layout**, **draw traversal**; and **of it in the
  shell's drawing**, the calls of `megler_gfx_host` made in any of the three;
  **in other native methods** (libcore's, the framework's own, an app's JNI:
  the natives' time less the shell's drawing, `present` and the waits, all of
  which happen inside natives) with how many native calls a frame;
  **allocating** (bytecode's own `new-instance`, `new-array` and the like;
  allocation inside a native is the native's) with how many objects a frame,
  made anywhere; **waiting for monitors** another thread held; and
  **interpreting**, the rest: bytecode, calls and returns, resolution, the
  interpreter's own work.
- **present**: handing the picture to the shell (`present_pixels`).
- **idle**: the share of the line's time the main thread spent waiting.
- **at N M instructions/s**: the main thread's instructions over its
  interpreting time, the interpreter's own pace; and then the instructions
  every thread executed in the time.
- **heap**: the runtime's objects -- every object made, since nothing is
  collected (and how much of it was made in the time the line covers) -- and
  the bitmaps' memory now; and the process's own use where the host knows it
  (the phone does, a test program does not).
- **redrawn**: the share of the screen drawn per frame drawn. Each window
  draws only its dirty region (below, "Drawing only what changed"); a frame
  that adds, removes, moves or resizes a window draws the whole screen.

What the demos run says. Under qemu the shell's drawing is the test program's
stand-in, in C, emulated: 62 of 78 ms, which says nothing about Direct2D on the
phone; the phone's own lines will. The interpreter's 14 to 17 million
instructions a second under qemu is qemu's figure. Once a list is on the
screen measure and layout costs almost nothing (0.1 ms a frame in the windows
after the first), and the draw traversal's own Java is 1 to 2 ms. The figures
are exact for the main thread at the marks; a thread in a long stretch of
bytecode without a native call is behind in "on all threads" until it
makes one. A read that blocks inside a native counts as that native's time.

What the phone said first (0.1.0.136, Apktool M, three and a half minutes):
measure and layout 30 to 150 ms a frame, the shell's drawing only 10 to 17,
present 2, never idle, and an interpreter pace of 5 to 8 M instructions/s
against 52 at the start -- so most of the main thread's time was not
bytecode, and the line then could not say what it was. The layout was a loop
in the framework's lists (`AbsListView`, in the overlay's list). The
breakdown into other natives, allocation, monitors and interpreting was added
after it so that the next log says where the rest goes. Under qemu with the
old lists, profiled (`MG_JAVA_PROFILE=after:3`): native methods other than
drawing and waiting were small (`String.charAt` 2.4 million calls, half a
second in all; listing directories 0.3 s); a fifth of all instructions were
`String.format` for each row's size text, which the loop asked for again in
every frame.

`mg_tests --java demos` checks that the meter's lines are there with the whole
run's last, that the run drew at least ten frames, and that the share is
under 100%. (The test apps' frames are checked against the screen drawn
whole, which draws every partial frame twice; the demos run's times are
inflated by that.)

### Drawing only what changed (the test apps under qemu-arm)

Each window keeps the rectangle its views asked to have drawn again, and a
frame draws only the union of the windows' rectangles, as Android's software
rendering does.
- `View.invalidate()`, `invalidate(Rect)`, `invalidate(l, t, r, b)`,
  `invalidateDrawable` and the moves and transforms that invalidate the
  parent pass a rectangle up (`View.meglerDirty`, in the overlay's `View`).
  It goes through each view's transform, position and its parent's
  scrolling, and is clipped where a parent clips its children. A rectangle
  clipped away asks for nothing, and so does a view that is not visible. A
  view with an `Animation` on the way asks for the whole window.
- `megler.ViewRoot` (overlaid) keeps it per window (`mDirty`). `drawAll` clips
  the screen's canvas to the union, clears it there and draws every window
  through the clip. `View.drawFromParent` skips a child that lies wholly
  outside it.
- It tells the shell the rectangle (`Native.presentRect`, then
  `megler_window_host.present_pixels_part`, an optional call added to
  `megler.h`; a shell without it is handed the picture by `present_pixels` as
  before).
- A frame in which nothing visible changed draws and presents nothing.
- The whole screen is drawn when a window is added, removed, moved or
  resized, when a window is invalidated without a rectangle, and at the
  first frame.

The test apps check it two ways:
- The test program's `present_pixels_part` copies only the rectangle, as a
  shell that keeps its last picture would, and counts pixels that changed
  outside it. That must be none: the clip held.
- `mg_java_check_partial` has every partial frame drawn again whole into a
  second bitmap (`ViewRoot.checkPart`), and compared outside the part
  (`Gfx.bmDiffer`, a native added by `megler/Gfx.java.members`). A difference
  is a line, "a frame drawn in part missed N pixels", and there must be
  none: the dirty region missed nothing.
- The focused text field is left out of that comparison. It blinks its
  cursor by the clock on a timer of its own, so drawn again a moment later
  it can be in the other phase. Inside the part both are drawn, and they
  differ only where drawing depends on the time (an overscroll glow).

The check's first runs found exactly those time-dependent differences, and
nothing else. demos: 85 pictures, 72 of them partial, redrawn 53% of the
screen a frame over the run. hello: 37 pictures, 36 partial, 16%.

Apktool M under qemu after its licence, with the old lists fixed: before,
16 frames every 5 s at 306 ms each and idle 12%, the whole screen each time.
That was a running `AnimationDrawable` in its closed drawer, off the screen.
After: 8 frames every 5.6 s at 16 ms each, idle 97%, redrawn 3% of the
screen a frame.

### What the nodes changed (package 663)

The views' rectangles are not enough for Compose. Its view
(`AndroidComposeView`) invalidates itself whole for a change of any of its
graphics layers, and Spotify drew a third of the screen again in every frame
for its progress bar. Android's renderer finds its damage node by node
(HWUI's `DamageAccumulator`). The frame walk does the same
(`core/src/drivers/mg_java_gfx.c`, `G_damage`; `megler.Gfx.damage`):
- `drawAll` records every window whole, by reference to the views' lists.
  Once the nodes have the frame's properties, it walks what the frame draws,
  with the matrix and clip each node is drawn with.
- Each node keeps where the walk last found it. A node moved, changed in its
  properties (`rnSet` counts real changes in `ver`) or new damages where it
  was and where it is. A View's node does too: a ListView's rows are moved
  by `offsetTopAndBottom`, which says only where a row now is.
- A node whose list is another is compared call by call. Each side keeps its
  own matrices and clips. A call that differs, or is drawn elsewhere, damages
  where it drew and where it draws. A list of another shape, or with more
  than 24 calls changed, damages the whole node. `rnEnd` keeps the old list
  when the new one is the same, bytes and all (a bitmap's call carries the
  bitmap's version).
- What changed in a View's own list is the View's to say, by its rectangles.
  But a View invalidated whole (`invalidate()`, a rectangle round all of it,
  `invalidateDrawable`) leaves that to the walk (`View.meglerDefer`,
  `Gfx.rnDefer`).
- The render thread clips the frame to the part and clears it there. It
  leaves out a node drawn once in the frame whose box, as the walk found it,
  is nowhere in the part.

The flag `no-node-damage` draws the views' rectangles alone, as before. The
log says every 5 s what the walk found (`gfx: the frame walk`) and how many
nodes the render thread left out. The natives' test `test_walk` checks the
rectangles of each kind of change, and that every frame drawn in part by
them equals the frame drawn whole. The hello app's `Layers` is a view drawn
as Compose's is: its frames drawn in part must miss nothing and must not
reach its own unchanged drawing.

### Measured, not tests

- **`invoke-polymorphic` is not needed yet.** `python tools/dexasm.py dump
  --code <file> | grep -c invoke-polymorphic` gives 25 for `core-oj.dex` and 0
  for the other five; all 25 are in `java.lang.invoke.Transformers$*` and
  `MethodHandle`, and there is no `invoke-custom`, `const-method-handle` or
  `const-method-type` in any of the six. Android 9's `java.util.concurrent`
  uses `sun.misc.Unsafe`, not `VarHandle`. So the instruction is still
  refused, and only code that uses method handles itself would meet that.
- **The DEX files the assembler writes are accepted by Android's own tool**:
  `dexdump -d` of build-tools 29.0.3 reads the fixture files and the built-in
  `java.lang` without complaint (tried by hand).
- The interpreter's speed: see "The interpreter's benchmark" below.

### The interpreter's benchmark

`core/tests/mg_dvm_bench.c` times the methods of `bench.Bench`
(`core/tests/dvm/bench/src/bench/Bench.java`, compiled by javac and d8 into
`bench.dex` by `tools/dvm-bench-fixture.sh` and kept in the repository like
`d8t.dex`). Each method does one kind of work `n` times: arithmetic, fields,
the four kinds of invoke, arrays, the hot methods of `String`, `StringBuilder`,
`HashMap`, `ArrayList`'s iterator, allocation, monitors, exceptions, `Math`,
`System.arraycopy`, boxing, `getClass` and casts. Each item gets an `n` that
makes it last about as long as asked. For each, the table gives nanoseconds a
turn, instructions a turn (`mg_dvm_thread_costs`), and millions of
instructions a second over all its time, native methods and allocation
included. It needs libcore. It is printed, never checked:

- `tools/dvm-bench.sh [win|arm|both] [ms]`: the test program built for x64
  with `/O2`, as the app is built (the interpreter's own speed, and the
  steadiest number), and the ARM test program under qemu-arm. 300 ms an item.
- The libcore tests run it too, 40 ms an item. The phone's Self-test prints
  the phone's own table, with the collector set as an app has it.
- Each item is timed as the best of three runs: other work on the machine
  only ever adds time.
- **Where the interpreter's loop falls in the program moves its speed by
  half.** With nothing in the interpreter changed, a change elsewhere took
  the arithmetic item from 8.4 to 14 ns on x64. Other code before the loop
  had shifted its alignment, so that its dispatch now straddled a 64-byte
  fetch window. The benchmark's build therefore puts the interpreter
  (`mg_dvm_interpret`, not static for this reason) first in the program's
  code with link `/ORDER` (`MEGLER_TESTS_ORDER`). Every build then
  measures it at the same place, and a change is measured, not where the
  loop happened to fall. That place happens to be a slow one (13.7 ns). The
  phone's package does not do this yet, so its speed can move from one build
  to the next for no reason in the code.
- `tools/dvm-bench.sh profile <item> [function]` shows where one item's time
  goes. A second thread samples where the running one is, about every 20 µs
  (`MG_BENCH_PROFILE`). `tools/dvm-profile.py` names the functions from the
  linker map and shows the hottest instructions of one of them.
- The real workload is an app's start under the Java test harness. After the
  first settle, `mg_tests --java` says "the start: N M instructions on all
  threads, S s to the last picture before the app rested".

Before the round that speeds the interpreter up (x64 `/O2` on a Ryzen 7
7800X3D; qemu-arm with gcc `-O1`), in ns a turn:

| item | insns | x64 | qemu |
| --- | ---: | ---: | ---: |
| int arithmetic | 7 | 8.6 | 136 |
| long and double arithmetic | 13 | 17.2 | 312 |
| iget and iput | 10 | 15.7 | 236 |
| sget and sput | 6 | 7.8 | 122 |
| invoke-static | 7 | 17.1 | 268 |
| invoke-virtual | 8 | 18.6 | 293 |
| invoke-virtual, final | 10 | 22.8 | 339 |
| invoke-interface | 9 | 21.4 | 316 |
| invoke-interface, two classes | 11.5 | 25.3 | 373 |
| aget and aput | 11 | 14.3 | 229 |
| String.charAt and length | 8 | 64.2 | 712 |
| String.equals, 23 characters | 262 | 2888 | 28195 |
| String.hashCode, kept | 16 | 41.0 | 605 |
| String.indexOf | 4 | 4.9 | 80 |
| StringBuilder, 3 appends | 276 | 1131 | 15130 |
| HashMap.get | 89 | 322 | 4495 |
| ArrayList iterator | 34 | 88.9 | 1165 |
| a small object made | 13 | 97.8 | 1277 |
| an int[16] made | 7 | 68.5 | 893 |
| synchronized block | 7 | 15.5 | 1118 |
| synchronized method | 10 | 29.2 | 1380 |
| exception made, thrown, caught | 38 | 435 | 7254 |
| exception thrown and caught | 6 | 16.1 | 243 |
| Math.min, max, abs | 23 | 51.5 | 862 |
| System.arraycopy, 16 ints | 6 | 62.9 | 726 |
| Integer.valueOf, intValue | 23 | 127 | 1772 |
| getClass, instance-of, check-cast | 20 | 53.9 | 814 |
| in all, M instructions a second | | 399 | 25.1 |

Starts under qemu: hello 4.7 M instructions in 0.64 s, demos 5.4 M in 0.74 s,
Apktool M 501 M in 26.2 s (to its splash screen at rest, its own threads
working on).

## Design

### Classes

A DEX file is used where it lies in memory; nothing is copied out of it but
what linking makes. Adding a file only notes which classes it defines (first
definition wins, as on Android's class path). A class is **linked** the first
time something needs it: its superclass and interfaces first, its instance fields
given slots after the superclass's, its methods read, its vtable made from the
superclass's with overrides in place (a package-private method only overrides
within its package). Names and signatures are interned, so that comparing two is
comparing two pointers.

What an index in bytecode names -- a type, a method, a field, a string -- is
resolved when an instruction first uses it and remembered per DEX file, so the
second execution of an instruction costs one array read. Fields are found by name
*and type*: obfuscators give fields of different types the same name.

There is one name space and one class path: two classes of one name cannot be,
and no class is loaded *through* a loader. Class loaders exist all the same, as
libcore's own objects, because apps hold and ask them:

- The system's classes (libcore, the framework) have a null `classLoader`
  field, which libcore reads as its `BootClassLoader`.
- An app's classes name the app's loader: the framework makes a real
  `PathClassLoader` over the APK and hands it to the runtime
  (`mg_dvm_set_app_loader`), which writes it into the `Class` object of every
  class of the DEX files added after `mg_dvm_app_follows`.
- `dalvik.system.DexFile`, through which a loader reaches the runtime, is
  answered: opening a file the app was started with reads nothing again;
  opening any other (a second DEX file an app unpacked) adds its classes to
  the one path; a class asked of a file by name is the class of that name
  wherever it came from, and null when there is none, so that the loader
  throws its own `ClassNotFoundException`.
- The loader finds the APK's other files itself (`getResourceAsStream`),
  since the APK is readable at the path the app knows it by,
  `/data/app/<package>-1/<file>`.

### Objects

An object is one allocation: the JNI layer's `mg_jobj`, then three words of the
interpreter's (its class, its monitor, its size), then its contents -- instance
fields as 8-byte slots, or an array's elements, or a string's fields and then
its UTF-16.

So **a pointer to an object is an `mg_jobj *` as native code and the JNI layer
already know it**: `kind` says what it is, `u.arr.data`/`u.arr.len` are an
array's elements (pointing into the same allocation), `u.oarr.v` an object
array's, `u.str.u16`/`len16` a string's text, and for a Class object `kind` is
`MG_J_CLASS`. `permanent` is set, so the JNI layer's reference counts leave these
objects alone, and `dvm` says the runtime made it. A string's `u.str.utf8` is
NULL until someone asks for it, since Java works in UTF-16.

The choice was between this and an object of the interpreter's own that the JNI
layer would wrap. Being the same object means no table between the two worlds, no
second identity for anything, and that everything already written against
`mg_jobj` -- `GetArrayLength`, `GetStringUTFChars`, the drivers' own reads --
goes on working on objects bytecode made. Its cost is the header: 48 bytes on the
phone (36 of them `mg_jobj`), 88 on a 64-bit PC, where Android's own is 8.

Slots are 8 bytes whatever the field's type. An `int` field wastes four; in
return a long or a double needs nothing special, and a field's place is the
header plus its slot number times eight for every type, which is what
`Unsafe.objectFieldOffset` hands libcore.

### Registers and frames

A register is one machine word (`mg_dvm_reg`, a `uintptr_t`): 32 bits on the
phone, 64 on a PC. An int, a float's bits and the small types sit in the low 32
bits with the rest zero; a reference is the pointer; **a long or a double is in
two registers, low half first, on both hosts**. On the phone that is Dalvik's own
frame layout and costs nothing. On a 64-bit host each half sits in a word twice
its size, which wastes room and means the PC's tests take the same path through
every wide instruction as the phone will. (A 64-bit word per register, with a
whole long in the first of its pair, would double every frame on a 32-bit
host.) The comment at `mg_dvm_reg` in `mg_dvm_internal.h` is
the authority.

Frames are not C frames. Each thread has a stack of its own (196,608 registers:
768 KB on the phone), and a call from bytecode to bytecode pushes a frame there
and carries on in the same C function; a return pops it. So Java recursion costs
words of that stack and none of the host's, a `StackOverflowError` is an ordinary
bounds check, and a thread that runs Java needs no more host stack than any
other. An interpreter that recurses in C needs thread stacks of 32, 64 or
256 MB for it, which a 2 GB address space could not afford.

C stands between two pieces of bytecode only where it must: a native method that
calls back, a static initialiser, the constructor of an exception the interpreter
throws. That is real recursion; it is bounded at 200 deep and by the host
stack's low end. A native method has a frame of its own on the thread's stack
(it is what a stack trace shows of it, and what `VMStack.getStackClass2` counts
from).

### Calls

`invoke-virtual` goes through the vtable of the object's class.
`invoke-interface`, and a virtual call of a method the named class only inherits
from an interface, look the method up by name on the object's class and remember
the answer in a small table on that class (one word an entry, checked by name
and signature when read, so it needs no lock). `invoke-super` takes the vtable of
the class above the caller's, or the interface's own method for
`Interface.super.m()`. `invoke-static` initialises the class first.

Arguments are copied register to register: the callee's last `ins` registers get
the caller's argument registers as they are, which is how the format means it.
Values become `mg_jvalue` only at the boundary with C.

`new String(...)` is done as ART does it: a string's length is fixed when it is
made, so the constructor's result replaces the object `new-instance` made in
every register that holds it. On libcore the constructor called is not
`String.<init>` (which only throws there) but the `StringFactory.newStringFrom...`
method of the same parameters.

### The loop, and the watched loop

The interpreter's loop is compiled twice from one source
(`mg_dvm_interp_loop.h`, included twice by `mg_dvm_interp.c`):

- `mg_dvm_interpret` runs code.
- `mg_dvm_interpret_watched` also does, for every instruction, what is asked
  while anything watches them (`slow_step`: the trace at level 3, the step
  limit, the profile).

The plain loop does not look at `g_dvm.slow` for each instruction. It looks
only where code can go on forever: at a jump back and at a call
(`SAFEPOINT`). When what it finds there differs from what it runs as, it
hands the frame on top to the other loop. `interpret` then runs the other loop
from that frame's pc until the frame entered from C returns or throws. Nothing
is in flight at those places: no call's result is waiting for its
`move-result`, and no exception for its `move-exception`. So a profile switched
on while the app runs (`MG_JAVA_PROFILE=after:N`) starts at the main loop's
next jump back.

`slow_step` itself is out of line, so the loop's head is only the fetch, the
count of instructions executed (for the meter) and the switch:
- MSVC for ARM (`tools/dvm-arm-disasm.sh`) makes it six instructions: add,
  ldrh, adds, str, uxtb, tbh.
- Before, it was eleven: it also had movw and movt of `g_dvm`, the load of
  `slow`, cbz, and a second store.

Benchmark on x64, interpreter at the same place, ns a turn:

| item | before both | `slow_step` out of line | then the two loops |
| --- | ---: | ---: | ---: |
| int arithmetic | 13.1 | 10.3 | 7.7 |
| iget and iput | 22.7 | 18.0 | 14.5 |
| aget and aput | 23.3 | 19.5 | 13.9 |
| `HashMap.get` | 307 | 256 | 242 |

### Calls done in place: intrinsics

On libcore, some of the library's methods are done by the interpreter itself
where an invoke names them, with no frame and no native call. Android's own
runtime does the same with these methods (its intrinsics).
`mg_dvm_intrinsics_start` marks them (`mg_dvm_method.intrinsic`) once libcore's
classes are there:

- `String.charAt`, a native call each time;
- `String.length` and `isEmpty`: a call that reads a field the runtime owns
  (`count`);
- `String.equals`: a loop of `charAt` calls in libcore;
- `String.hashCode`: the same loop, once; the result is kept in the string's
  `hash` field, as libcore keeps it;
- `System.arraycopy`;
- `Math.min`, `max` and `abs` of `int` and `long`, and `Math.sqrt`;
- `Object.getClass`;
- `Thread.currentThread`;
- `libcore.io.Memory`'s reads and writes of memory by its address (`peekByte`,
  `peekShortNative`, `peekIntNative`, `peekLongNative`, `peekByteArray` and the
  `poke` methods that match them): native calls of libjavacore's, each through
  the JNI layer, and what a direct `ByteBuffer`'s `get` and `put` come down to.
  `Resources` reads an app's `resources.arsc` through one ("Resources read
  where they lie", in "What a run costs");
- `Float.floatToRawIntBits` and `intBitsToFloat`, `Double.doubleToRawLongBits`
  and `longBitsToDouble`: native calls too, and `Math.round` calls the first
  for every float it rounds.

**An intrinsic only does the ordinary case.** For anything else it declines,
and the method is called as before:
- an index out of range;
- a null;
- arrays of two kinds, or references whose element types might not fit;
- a class whose `Class` object or a thread whose `Thread` is not made yet.

So every exception is still the method's own, thrown from its own frame. While
every instruction is watched (the trace at level 3, a step limit, the profile),
nothing is done in place.

`bench.Edges` (beside the benchmark) is the test. It runs each intrinsic at its
edges twice: once as the interpreter runs it, and once with every instruction
watched. The two reports must be the same text, and must match the JVM's
answers where those do not depend on the runtime's messages (`test_in_place`
in `mg_dvm_tests_libcore.c`).

Benchmark on x64 in ns a turn, before and after:
- `String.charAt` with `length`: 24.2 to 13.5;
- `String.equals` of 23 characters: 1045 to 15.2;
- `String.hashCode`: 36.8 to 11.6;
- `Math.min`, `max` and `abs`: 48.8 to 25.3;
- `System.arraycopy`: 28.6 to 14.8;
- `getClass` with a cast: 49.1 to 40.2;
- `StringBuilder`: 873 to 811.

### Calls done in place: trivial methods

A method whose whole body has one of a few trivial shapes is done by the call
itself, with no frame. Dalvik's dexopt did this for the same code, and ART's
compiler inlines it. The shapes:
- an empty body: `Object.<init>`, which every constructor ends in, and the
  empty methods listeners leave;
- a constant returned (`const/4`, `return`);
- a field of `this` returned (a getter);
- a field of `this` set from the one argument, not a long or a double (a
  setter);
- a static field returned.

`classify` finds the shape at a method's first call and writes it to the
method as one word: the shape, the instruction that does the work, and a
constant. It writes the field before that word, with a barrier between,
because other threads read both without a lock. A reader that sees the word
but not the field yet calls the method instead.

When the call cannot be done in place, the method runs as before:
- the static field's class is not initialised yet;
- the object's class has no such slot.
Synchronized methods, native ones and ones with try blocks are never done in
place. Neither is anything while every instruction is watched. A call done in
place counts the instructions the method would have executed (1 or 2), so
instruction counts stay comparable.

`bench.Edges` covers it too: getters and setters of each kind, constants,
empty methods, a static getter of another class that must be initialised
first, and a getter called on null.

Benchmark on x64 in ns a turn: a small object made (its constructor ends in
`Object.<init>`) 61.5 to 53.6; `ArrayList`'s iterator 88.8 to 76.4;
`Integer.valueOf` 91.8 to 77.6; `HashMap.get` 272 to 262.

### Methods an app hooks: Xposed's XposedBridge

Aliucord (Discord patched by Aliucord Manager) carries Aliuhook
(github.com/Aliucord/hook): Xposed's API in Java -- `XposedBridge.hookMethod`,
`XC_MethodHook`'s callbacks before and after a method,
`invokeOriginalMethod` -- over LSPlant, which hooks a method on ART by
rewriting ART's own structures (libart.so's symbols, the method's
`ArtMethod`) and a class it makes at run time. There is no ART here: on
0.1.0.669 Aliucord ended at its start, the library's `JNI_OnLoad` having
returned JNI_ERR ("failed to open /system/lib/libart.so") under
XposedBridge's static initialiser. The runtime hooks methods itself
(`mg_dvm_hook.c`), as the first Xposed did on Dalvik, and XposedBridge's nine
natives are its own: registered for the class by name, which comes before any
library's function, while the library -- known by the native it exports,
`Java_de_robv_android_xposed_XposedBridge_hook0` -- is loaded without its
`JNI_OnLoad` (`mg_dvm_jvm.c`).

The contract is LSPlant's, which Aliuhook's Java builds on.
`hook0(context, original, callback)` makes every call of `original` (a
`Method` or a `Constructor`) a call of `context.callback(Object[] args)`: the
arguments boxed, the object first for an instance method. What the callback
returns is the call's value -- unboxed for a primitive (null throws a
`NullPointerException`, an object that is no such box a
`ClassCastException`), checked against the class the method returns for an
object -- and what it throws, the call throws. `hook0` hands back the
backup: a `Method` whose `invoke` runs the original body on the object it is
given, past the hook; a `Method` even for a constructor, whose body it runs
on the object. Null when the method cannot be hooked (abstract, hooked
already), and XposedBridge says "Failed to hook method". `unhook0`,
`isHooked0`, `allocateInstance0` (JNI's `AllocObject`), `invokeConstructor0`
(a constructor run on an object there already) and `makeClassInheritable0`
(no `final` on the class, its constructors protected) do what Aliuhook's do;
`deoptimize0`, `disableProfileSaver` and `disableHiddenApiRestrictions` are
about ART, and say true.

A hooked method keeps its bytecode, so that a frame already running it runs
on, and is marked (`mg_dvm_method.hooked`). Every way into a method looks at
the mark before anything else and calls the hook (`mg_dvm_hook_call`): the
interpreter's invoke, before an intrinsic or a trivial body is done in place;
compiled code's calls (`mg_dvm_jit_invoke`; the quick way declines a hooked
method); the calls from C (`mg_dvm_invoke`: reflection, JNI, the runtime's
own). A static method's class is initialised first, as for any call. The
backup is a copy of the method -- a struct of its own, private so that
nothing dispatches it again, in no class's tables, never hooked -- with the
same body, made at the method's first hook and kept for good, as calls of it
may be running. In compiled code a call done in place looks at the mark once
the app has hooked anything; the code compiled before that, which does not,
is forgotten then, and a method hooked when a call of it is compiled is not
done in place ([JIT.md](JIT.md)). A frame inside such code at that moment
goes on in it until it hands the frame back or returns; in Aliucord the
first hook comes in `Application.onCreate`, before much is hot. The hooks'
contexts and their backups' `Method` objects are roots of the collector.

Where nothing is hooked it costs a byte read by each call, the interpreter's,
compiled code's (the quick way and the general one) and those from C, and
nothing in compiled code itself. A hooked call boxes its arguments, makes an
array, calls the callback, and Aliuhook's callback calls the backup through
`Method.invoke`, which unboxes them again: as on ART, a hook is for code that
is not hot.

`test_hooks` (`core/tests/mg_dvm_tests_hooks.c`, on libcore) runs `hooks.dex`
(`core/tests/dvm/hooks`, compiled by `tools/dvm-hooks-fixture.sh` and kept
in the repository): an instance method, static ones with an argument of each
kind, a constructor (called by `new`, `Constructor.newInstance`, a
subclass's constructor and `invokeConstructor0`), an interface's default
method, a native method; called from bytecode with every instruction watched
and not, through reflection and through JNI, by a subclass and by an
override's super call, from loops the JIT compiled before the first hook and
after it; arguments and results changed, exceptions each way, unhooked, on
two threads at once; and a hook that nothing but the runtime keeps, across
collections. Where guest code runs, `libt_xposed.so` (`t_xposed.c`), which
stands in for libaliuhook.so, is loaded and none of its functions is called.
It passes under qemu-arm and on the PC; no phone has run it, nor Aliucord.

### Exceptions

One exception can be pending on a thread, for bytecode and native code alike.
Throwing looks for a handler in the current method by the try table (the first
range that covers the instruction, its typed handlers in order, then its
catch-all), and failing that pops the frame and looks in the caller at the
instruction that called. A catch of a class that cannot be found catches
nothing. Where C stands in between, the C function returns false and the caller
goes on unwinding.

An exception the interpreter raises itself is made the way bytecode makes one:
the object, then its `<init>(String)`. `Throwable`'s constructors record where
they were called from (`nativeFillInStackTrace`): each frame's method and the
instruction it was at, which with the DEX file's line table gives
`mg_dvm_pending_stack` and libcore's `StackTraceElement`s.

### A DEX file in memory

`dalvik.system.InMemoryDexClassLoader` (Android 8 and later) hands the runtime
a DEX file's bytes in a `ByteBuffer` (`DexFile.createCookieWithArray`, and
`createCookieWithDirectBuffer` for a direct one, whose memory is at its
address). The bytes are copied -- the runtime keeps a DEX file's bytes for
good, and the buffer is the app's -- and its classes join the one class path
as an opened file's do. This is how a library carrying Java code inside its
native one puts it on the path: JUCE does, and comScore in MX Player is built
with it (`com.rmsl.juce.JuceInvocationHandler`; without it comScore went on
with a null and ended MX Player's main thread); Facebook's Audience Network
loads its own DEX so. The runtime's tests on libcore hand over `m.dex`, which
nothing puts on the path, and run its class (where libcore's native
libraries are: the loader goes through `java.io`).

### Static initialisation

A class is initialised at the first `new-instance`, static field access or static
call: the superclass first, then the values the file gives static fields, then
`<clinit>`. A second thread asking meanwhile waits; the same thread asking again
(an initialiser that reaches back to its own class) carries on with the class
half-made, as Java specifies. An initialiser that throws leaves the class
unusable: `ExceptionInInitializerError` the first time, `NoClassDefFoundError`
after. Apps catch the first and go on (kotlinx.coroutines does, for the
services it looks up), so the error that ends them names only the class: MX
Player ended on "Could not initialize class xc4" and nothing said why. So, as
Java 17 and ART do, each later `NoClassDefFoundError` has the first failure
for its cause, and the first failure is said in the log when it happens,
once for each class, with its causes and where it was thrown
(`dvm: the initialisation of X failed, and the class cannot be used from now
on: ...`). libcore's `ExceptionInInitializerError` keeps what the initialiser
threw in a field of its own, not as its cause, so the log says that one, and
where it was thrown. The runtime's tests check the cause on both runs,
through bytecode and through JNI.

### One Java world with the JNI layer

Before the runtime is started the JNI layer is what it was: classes, methods and
fields looked up by name, each method implemented in C by a driver or logged
once. The Unity and NativeActivity drivers and `mg_jni_android.c`,
`mg_jni_loop.c`, `mg_jni_prefs.c`, `mg_media.c` run that way and are untouched;
the core's 242 checks of them pass before the runtime starts in the same
process.

Once it is started (`mg_jni.c` asks `g_dvm.running`):

- **Every class the layer knows has a class of the runtime's.** One a DEX file
  defines is the runtime's own, and its methods and fields are the bytecode's.
  One no DEX file defines stays the layer's, implemented in C as before, and
  gets a stand-in in the runtime (a "foreign" class under `Object`) so that its
  objects can be locked, stored in arrays and held by bytecode. Bytecode cannot
  call such a class's C methods; nothing needs that yet.
- **Every object is the runtime's**: `NewObject`, `NewString`, `New<Type>Array`
  and the layer's own `mg_jni_new_*` make runtime objects, and a class's
  `jclass` is its `java.lang.Class` object.
- **One pending exception**, the runtime's; `Throw`, `ExceptionOccurred` and
  the rest work on it.
- `FindClass` links a class and does not initialise it, as Android's does
  (the specification says it initialises; libcore's libraries rely on Android's
  way, looking classes up before their natives are registered). Asking for a
  method or field id initialises.
- `Call...Method` on a method of a DEX class runs the bytecode, virtually or
  exactly as asked; fields are slots. A method called through JNI takes as many
  parameters as Java allows (`JARGS`, 256; it was 64, and WhatsApp's native
  code calls `JniBridge.jnidispatch` back with 78: the last 14 were whatever
  the stack held, and the interpreter ended on an int taken for an object). A
  native method takes 120 argument words (it was 22).
- `MonitorEnter`/`MonitorExit` are the runtime's monitors;
  `NewDirectByteBuffer`, `GetDirectBufferAddress` and `-Capacity` work on
  libcore's own `java.nio.DirectByteBuffer` when it is there.

**A native method is bound** when it is first called, in this order: a C function
registered with the runtime for its class (`mg_dvm_register`, the JNI layer's
own `mg_jhle` table shape: what the runtime implements itself); then a library's
function, registered with `RegisterNatives` or exported under its JNI name
(`Java_pkg_Class_method`, then with `__` and the mangled parameters for an
overloaded one; `_1`, `_2`, `_3` and `_0xxxx` as the specification has them);
then a stand-in (`mg_dvm_register_fallback`), used only where no library
answers. A `RegisterNatives` entry for a method the class does not declare is
said in the log and left out, where Android refuses the whole table. A method
annotated `@CriticalNative` in its DEX file is called with its arguments alone,
no `JNIEnv` and no class, as Android calls it (`java.lang.Math`'s are such).

### On libcore

What libcore expects of the runtime and how it is given (`mg_dvm_art.c`, each
function written against its counterpart in `art/runtime/native`):

- **`Object`'s two fields** (`shadow$_klass_`, `shadow$_monitor_`) and
  **`String.count`** are declared by libcore and owned by the runtime: they
  have no slot. A read of one (which libcore's own bytecode does: `length()` is
  `count >>> 1`) is answered from the object's header. `count` is the length
  shifted left once with the low bit set, ART's form for a string that is not
  compressed; **no string is compressed here** (ART keeps Latin-1 strings in
  bytes; libcore never looks, since every access to the characters is a native
  method). `hash` is a real slot, and the text follows it.
- **`Class`**: a class's mirror is a real `java.lang.Class` object with the
  fields libcore's bytecode reads filled in when it is made: `accessFlags`,
  `primitiveType`, `dexClassDefIndex`, `objectSize`, `superClass`,
  `componentType`, and `ifTable` as the flat list of interfaces
  `isAssignableFrom` walks. The rest stay zero (`dexCache`, the method and
  field arrays, `vtable`): what reads those on Android is the runtime itself.
- **Long and double fields, and `Unsafe`**: a field's offset is its distance in
  bytes from the object's address, an array's base offset is the header's size
  and its scale the element's size, so `Unsafe`'s native methods are reads,
  writes and compare-and-swaps at an address. The 64-bit compare-and-swap is
  new in the host layer (`mg_atomic_cas64`).
- **Threads**: `Thread.currentThread()` makes the calling thread's `Thread`
  object the first time it is asked (in `ThreadGroup.mainThreadGroup`, by the
  constructor libcore keeps for the runtime). `Thread.start` is
  `nativeCreate`: a host thread that is a guest thread from the start (native
  methods run on it), running `Thread.run`. `join`, `sleep`, `yield`,
  `holdsLock`, the name and the priority work; `interrupt` sets a flag that
  `interrupted` reads and does not wake a thread that waits. **The daemons are
  not started** (`java.lang.Daemons`: finalizers, reference queues, the heap
  task): nothing is collected, so there is nothing for them to do.
- **Reflection** (`mg_dvm_art_reflect.c`): `getDeclaredField(s)`,
  `getDeclaredMethod(s)`, `getDeclaredConstructor(s)`, their public
  counterparts through libcore's own Java code, `Field.get`/`set` in every
  width with widening and boxing, `Method.invoke` and
  `Constructor.newInstance` with boxing, unboxing and
  `InvocationTargetException`, `Class.forName`, `newInstance`,
  `Array.newInstance`. There are **no access checks**.
- **Proxies**: `Proxy.generateProxy` makes a class at run time, under `Proxy`
  and implementing the interfaces, with one native method for each `Method`
  libcore hands it; a call boxes its arguments and goes to the
  `InvocationHandler`, and the result is unboxed or widened to what the method
  returns. Up to 256 such classes.
- **Annotations** (`mg_dvm_art_annot.c`): an app's own with run-time
  visibility become objects the way Android's runtime makes them -- libcore's
  `AnnotationFactory` with a member for each element, which is itself a proxy
  -- on classes, fields, methods, constructors and parameters, with defaults.
  And the ones a DEX file keeps for itself answer what a class file has in
  attributes: `getSimpleName`, `getDeclaringClass`, `getEnclosingClass` and
  `-Method`, `isAnonymousClass`, `getDeclaredClasses`, generic signatures
  (`getGenericSuperclass` and the like), `getExceptionTypes`. Nothing is
  cached: each question reads the file again.
- **Who is calling**: `VMStack.getStackClass2` is the class three frames up
  from the native method's own, as ART counts. It matters:
  `BufferedInputStream` makes an atomic updater for a protected field of its
  own, and the updater refuses anyone but the class that made it.
- `VMRuntime`, `VMDebug`, `ZygoteHooks` and the debugger's classes are answered
  by tables that do nothing, and return zero, for any method not written out
  (so `is64Bit` is false); the ones written out name the instruction set, the
  boot class path and the system properties the runtime owes, and make the
  arrays libcore asks it for.

**The security providers.** Android's list is conscrypt first (its provider
is called "AndroidOpenSSL": digests, random numbers, ciphers, over BoringSSL in
its native library), then libcore's own for certificate paths, then
BouncyCastle, then conscrypt's for TLS. libcore makes all four when the first
thing asks for any, and takes one that cannot be made for a broken system: an
`AssertionError` in the static initialiser of `sun.security.jca.Providers`,
after which there is no `MessageDigest`, no `SecureRandom` and no
`UUID.randomUUID()` for the life of the process. That is what happened here
at every app's start (the framework adds a provider of its own first thing)
while conscrypt's library was not built.

It is built now (2026-10-14). `tools/android9-sources.sh` fetches
`external/conscrypt`'s `common/src/jni` and its `Android.bp`, and `libssl.so`
for ARMv7 from the same VNDK snapshot as `libcrypto.so`;
`tools/android9-natives.sh` compiles `libjavacrypto.so` from them as Soong
would for the platform (`JNI_JARJAR_PREFIX=com/android/`, so it answers
`com.android.org.conscrypt.*`), Thumb-2 and VFPv3-D16 like the others, and
links it against Android's own `libcrypto.so` and `libssl.so` (which alone are
NEON code; every Windows phone's processor has NEON). It needs what Android's
own does less `libc++.so`, and Megler's linker finds every import of it and of
`libssl.so` (`tools/android9-natives/loadtest.sh`). `tools/dvm-android-root.sh`
puts both in `lib/`, and the certificate authorities apps trust in
`etc/security/cacerts`, where conscrypt's `TrustedCertificateStore` reads them:
`core/cacerts`, Mozilla's root store laid out as Android's (one file a root,
named by `X509_NAME_hash_old`), made by `tools/make-cacerts.py` from curl's
extraction of it (`core/cacerts/README.md` says which and of when). Android 9's
137 of 2018 lacked the authorities of many sites' chains today (GTS Root R1 to
R4, ISRG Root X2); roots the tool's `DISTRUSTED` names are left out whatever
the bundle says (eMudhra's emSign ECC Root CA - G3, whose intermediates' keys
were compromised in October 2026).
With `lib/libjavacrypto.so` in the root, on a host that runs ARM code and
where libcore's libraries loaded (`mg_dvm.c`), the list is Android's own:
`System.loadLibrary("javacrypto")` loads it from the root when conscrypt's
`NativeCrypto` first starts, its `JNI_OnLoad` registers conscrypt's natives,
and digests, ciphers, key factories, certificates and TLS are conscrypt's over
BoringSSL. The data app's `tls`, `tls-loopback` and `tls-https` steps show it
under qemu: the platform's trust manager holds the 137 authorities and refuses
a self-signed certificate; a TLS server and client of the app's own on
127.0.0.1 shake hands (TLSv1.2, `TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256`
-- ChaCha20 because BoringSSL sees no AES instructions), a line goes each way,
and a client with the platform's trust is refused by the same server; and an
https URL by name ("localhost", which the test certificate names) through
Android's own HTTP client (`com.android.okhttp`, behind `URL.openConnection`)
gets 200 and the body.

An app's own network security configuration (its manifest's
`android:networkSecurityConfig`) is trusted as Android 9 trusts it, by
Android's package `android.security.net.config`
(`core/framework-overlay/android/security/net/config`;
`NetworkSecurityConfigProvider.java` says how). Before the app's code runs,
`megler.AppRunner` makes the app's configuration the default and puts the
provider `AndroidNSSP` first; its `TrustManagerFactory` gives a
`RootTrustManager` that checks each server by the configuration of its host:
conscrypt's `TrustManagerImpl` over that configuration's anchors (the root's
cacerts, the app's certificates in `res/raw`; there are no user ones here),
then its pins. Russian apps need it: Ozon and VTB's bank trust the Russian
Trusted Root CA, which Android 9 does not have, from their own `res/raw`. The
data app's steps "network-trust", "network-trust-domains" and
"network-trust-pins" show it with a test authority of the app's own: trusted
for localhost and the names its configuration gives it, 127.0.0.1 refused,
pins matched, missed, expired and overridden. A server's certificates the
`RootTrustManager` refuses are said once a host, with why and with the issuer
of the last of them ("W/Megler: TLS: <host>'s certificates refused (N of them,
the last issued by ...): ..."): the system's authorities are Android 9's 137
of 2018, and a server whose chain reaches only a newer authority is refused
here where Android 14 takes it. Chromium's own line ("cr_X509Util: Failed to
validate the certificate chain", the WebView's; Google Play services' Cronet
says "cn_X509Util") names no server: Transit's WebView was refused eight at
every start (0.1.0.675), whose they were not known.

What follows is what stands in where conscrypt's library is not in the root
(an Android root made before the library was built, or the PC's own build of
the tests, which cannot load ARM libraries).

So while `lib/libjavacrypto.so` is not in the root, the list is changed just
before that initialiser runs (a hook on class initialisation,
`g_dvm.before_init`): in conscrypt's place a provider of the runtime's own,
`megler.security.RuntimeProvider`, then libcore's, then BouncyCastle;
conscrypt's TLS provider and the two properties that name its socket
factories are taken out. The runtime's provider is two classes in smali
(`core/src/dvm/art`, assembled into `mg_dvm_art_dex.h` and put on the boot
class path after libcore's files). It goes by conscrypt's name, because the
name is asked for: BouncyCastle's own key derivation takes its digests from
"AndroidOpenSSL" and refuses to start on Android without it. Under that name
it offers the six digests -- which are BouncyCastle's own classes, in Java --
and `SHA1PRNG`, which is the system's random source (`mg_random`). MACs,
ciphers and key derivation are BouncyCastle's. There is no TLS, but there are
SSLContexts by every name Android 9's provider has (TLS, Default, SSL, TLSv1,
TLSv1.1, TLSv1.2; `NoTlsContext.smali`): `init` takes anything, and the
socket factory they give -- which is also `SSLSocketFactory.getDefault()`'s
and `HttpsURLConnection`'s -- answers every socket with an `SSLException`, "no
TLS on this phone yet", from the call that would connect. Without them
Android's own HTTP client, behind `URL.openConnection` for an https URL,
threw `AssertionError` ("the system has no TLS") and ended the thread that
asked; Firebase's threads in MX Player and an ad SDK's in Super S22 Launcher
died that way. Now the app sees a failed connection, as for a name that does
not resolve. And a `TrustManagerFactory` "PKIX" (also "X509") whose one trust
manager trusts no certificate (`NoTlsTrustManagerFactory.smali`): an app's
own HTTP client asks for the platform's trust managers first, and Amazon's
MAP library in MX Player failed its static initialiser on "PKIX
TrustManagerFactory not available". (An `SSLEngine` or a server socket
factory is `UnsupportedOperationException`; `KeyManagerFactory` is not
there.) The data app's `tls` step asks for the contexts and the defaults, has
the factory refuse a socket with that message, opens an https URL whose
connection must fail as an `IOException`, and has the trust manager refuse a
certificate; it logs "tls-is none" (with conscrypt, "tls-is conscrypt"), and
the test holds that against the root. A cipher asked of "AndroidOpenSSL"
by name is not there.

Key pairs are BouncyCastle's too, and that needed one thing more. Android 9
refuses the system's BouncyCastle to an app that names it for an RSA or EC key
pair, a key factory, an HMAC and the rest of a list
(`Providers.checkBouncyCastleDeprecation`, when the app's target SDK is above
27; `VMRuntime`'s is above that here, since nothing sets it), and only when it
is named: `KeyPairGenerator.getInstance("RSA")` without a provider gets
BouncyCastle's. The framework's `AndroidKeyStore` named every provider in turn,
so every key pair made in it ended in "no RSA key pair generator" -- Amazon's
sign-in storage in MX Player, three times a start. The overlay's
`megler/KeyStoreProvider.java` takes the generator from the provider's service
instead. The data app's `keystore` step makes an RSA pair of 2048 bits and an
EC pair on P-256 in `AndroidKeyStore` under aliases, wraps sixteen bytes with
`RSA/ECB/PKCS1Padding` and unwraps them, signs with `SHA256withECDSA` and
verifies, both keys found again through `KeyStore.getInstance
("AndroidKeyStore")`; the second run finds the RSA pair of the first before it
makes it again. The keys are ordinary ones in the app's data
(`.megler_keystore`), as the framework keeps them: nothing here is hardware-backed.

A key pair's certificate is X.509, as Android's: self-signed with the pair's
key, with the `KeyGenParameterSpec`'s subject, serial number and dates, or
Android's own (`CN=fake`, 1, from 1970 to 2048), and kept with the keys
(`.megler_keystore2.certs`, a file of its own, so a Megler of before it still
reads the keys); a key set with a chain keeps that chain. It was an object of
Megler's own class that carried only the public key, and Google Play services'
device registration casts the certificate to `X509Certificate`: on 573, with the
SIM list answered, the check-in ended there in a `ClassCastException` every
time, and Play Store had no Android ID ("Server error"). A pair asked for with
an attestation challenge is refused with a `ProviderException`, nothing kept
under its alias, as on a device whose key store cannot attest
(`AndroidKeyStoreKeyPairGeneratorSpi`): there is no keymaster to sign the chain
up to Google's root, and the check-in goes on without attestation ("Failed to
generate key attestation"), as it does on such a device. The data app's
`keystore-certificates` step checks both kinds of certificate, a chain set
with a key, the refusal, and, on the second run, the first run's certificate.

`android.util.Base64OutputStream` and `Base64InputStream` encode and decode
as they go, with `Base64`'s flags as `Base64.encode` and `decode` take them
(`MeglerBase64Coder`, in the overlay): lines of 76 and a line end, the last
too, unless `NO_WRAP`; `CRLF`, `NO_PADDING`, `URL_SAFE`, and `NO_CLOSE` for
the stream underneath. Both were the framework's stubs, which wrote and read
nothing: Sberbank compressed a request's body through a
`DeflaterOutputStream` over one, and the body went out empty (576). The data
app's `base64-streams` step checks the output stream against
`Base64.encode`, written a piece and a byte at a time, with the default flags
and with `URL_SAFE | NO_WRAP | NO_PADDING`, and a `DeflaterOutputStream` over
one read back through an `InflaterInputStream` over the other.

**The built-in `java.lang` stays** as the first way to start
(`mg_dvm_start(NULL, 0)`). It could be dropped now that every fixture passes on
libcore too; it is kept because libcore is 40 MB that the ordinary package does
not carry, and because the PC's tests must run where `build/android9` has not
been unpacked.

**Newer libcore classes** are in `megler-libcore.dex`, which is on the boot
path before `core-oj.dex` (`tools/build-libcore-overlay.sh` builds it from
`core/libcore-overlay`): a class there is added, or replaces Android 9's of the
same name, and a `.java.members` file adds members to Android 9's source.
Besides `DelegateLastClassLoader` and TLS's newer methods there is
`java.util.zip.CRC32C` (Android 14's; Yandex Maps ended with
`NoClassDefFoundError` when it saved a route, package 489), and `Checksum`'s
two default methods of Java 9 (`update(byte[])`, `update(ByteBuffer)`). The
data app's "crc32c" step checks the standard check value, the defaults, a
direct buffer and `Adler32` through the interface.

**Java 9 and 10's collection factories** are added that way to Android 9's
`List`, `Set` and `Map` (Android's `Map.java` says it took them out):
`List.of` and `Set.of` of 0 to 10 elements and of an array, `Map.of` of 0 to 10
pairs, `Map.ofEntries`, `Map.entry`, and the three `copyOf`. Android has them
from Android 11, and Megler says it is Android 14: Amazon Shopping ended at its
start with `NoSuchMethodError: java/util/Map.of(Object, Object)` (and
`Set.of(Object)`), Shazam with `Map.of()` and Firebase's `Map.entry`. What they
make is `java.util.ImmutableCollections`, Megler's own, as Java documents the
collections: nulls refused with `NullPointerException`, by the factories and by
`contains`, `indexOf`, `containsKey`, `containsValue` and `get` asked for null,
as OpenJDK's are; an element of `Set.of` or a key given twice refused with
`IllegalArgumentException` (`Set.copyOf` keeps one); every mutator refused with
`UnsupportedOperationException`, an empty collection's, the iterators', a map's
views' and entries' too; a list is `RandomAccess`; a set and a map go in the
order they were given; the lists, sets and maps are `Serializable`, through a
proxy that makes them again with the same checks (`Map.entry`'s entry is not,
as documented); `copyOf` of one of them is that one. Java 10's
`Collectors.toUnmodifiableList`, `toUnmodifiableSet` and `toUnmodifiableMap`
(Android 13) collect into them (a key mapped twice is an
`IllegalStateException`, unless a merge function is given), and Java 16's
`Stream.toList()` (Android 14), a default method as Java's is, gives an
unmodifiable list that keeps nulls.

Every class the overlay replaces keeps every member Android 9's has: the
runtime names none of them (nothing in `core/src/dvm` asks for `java.util`'s
collections or streams), and dexdump's lists of `core-oj.dex` and of the
overlay agree, but for five of `Collectors`' lambdas, which javac 8 names
otherwise and only `Collectors` calls. The overlay is compiled with JDK 8's own
classes for the platform (`javac -cp android.jar`), so what an overlaid class
calls of its package's hidden classes (`Stream`'s static methods, of `Streams`
and `StreamSpliterators`) is JDK 8's when it is compiled and core-oj's when it
runs, of the same names and types; every such reference was found in Android
9's boot class path. The data app's steps "collection-factories" and
"unmodifiable-collectors" call each of them by reflection (the app is built
against Android 10's `android.jar`): each arity, the nulls and duplicates
refused, every mutator, the copies, a round trip through `ObjectOutputStream`,
a collector's parts combined as a parallel stream combines them, a merge in the
stream's order.

**Methods core-libart's classes lack** cannot be added that way: core-libart
(ICU4J among it) is a file of DEX code here, without its sources. A reference
to such a method that a class does not have is looked for in the framework's
`megler.members.<the class's name>` (`core/framework-overlay/megler/members`;
`boot_member` in `mg_dvm_class.c`): a static method of the same name and
signature is the class's static method, and a static method that takes the
object first is the object's method -- a copy of it, not static and not
virtual, which the interpreter calls as it is, the registers of the call being
the same. The class's superclasses are asked in turn. There is
`MeasureUnit.getIdentifier()` and `MeasureUnit.forIdentifier(String)`
(Android 12's), ICU 60's names spelled as ICU 68 spells them
(`inch-ofhg`, `liter-per-100-kilometer`).

**`android.icu.number`** is two packages. Android 9's core-libart has ICU 60's,
which Android had not published, whose classes are other (`Rounder`, not
`Precision`) and fewer, and which its `android.icu.text.DecimalFormat` is made
of; apps of now call the one Android 11 published, with what Android 12 and 13
added (`usage()`, `FormattedNumber.getOutputUnit()`). The framework's is that
one (`core/framework-overlay/android/icu/number`), and the runtime keeps
core-libart's to core-libart: its classes in the package are registered under
`megler/libart/` names, and what core-libart names in it resolves to them
(`mg_dvm_own_desc`), so that `DecimalFormat` keeps the classes it was built
with and an app sees the framework's alone. (Until package 496 the framework's
few classes took the place of core-libart's of the same names: Yandex Maps
ended with `NoSuchMethodError: UnlocalizedNumberFormatter.usage`, Facebook's
compact counts had no `notation()`, and `android.icu.text.DecimalFormat` had
lost the classes it uses.) The framework's formats with ICU 60's
`android.icu.text` -- `DecimalFormat`, `CompactDecimalFormat`, `MeasureFormat`
given that `DecimalFormat` -- and has the settings: notation (simple,
scientific, engineering, compact), unit and per-unit, every precision
(fraction, significant, both with either priority, increment, currency), the
rounding mode, grouping, integer width, symbols and numbering systems, unit
width, every sign display, the decimal separator, scale, usage, and trailing
zeros. A usage's unit comes from CLDR's preferences of a few kinds of quantity
(`Units.java`: temperature, length, mass, speed, volume, area, fuel,
pressure), with the locale's `mu`, `ms` and `rg`; CLDR's mixed units are the
larger unit alone. Not there: skeletons and number ranges. The data app's step
"icu-number", compiled against the newer classes as an app is
(`core/tests/app/data/stubs`, which `tools/build-test-app.sh` compiles
against and leaves out of the DEX file), asks for AndroidX's unit of the
weather in the US, in Russia and with `-u-mu-celsius`, a road's unit,
Facebook's compact counts, Play Store's fixed digits, a currency, a unit's full
name, the sign, fraction rounding that keeps two significant digits, the value
as shown; and formats with ICU 60's `DecimalFormat` and `CompactDecimalFormat`,
and finds no `Rounder`.

### Starting on Android's libcore

`mg_dvm_start_android(root)`, with `root` a folder laid out like Android's
`/system` (`tools/dvm-android-root.sh` makes `build/android9/root`):

```
framework/core-oj.dex, core-libart.dex, conscrypt.dex, okhttp.dex, bouncycastle.dex, apache-xml.dex
framework/core-oj.jar, core-libart.jar       the jars as the device has them (no bytecode in them)
framework/megler-framework.dex                  the Java framework (tools/build-framework.sh); for an app, not for libcore
framework/megler-framework-classes2.dex ...     the rest of it, where it has outgrown one DEX file (65536 methods, fields or types)
framework/framework-res.apk                   the platform's own resources, Android 9's; for an app
lib/*.so                                      libcore's native libraries, 32-bit ARM, conscrypt's among them
lib/libc.so, libz.so ...                      the names of the system's libraries the core answers, files that say so
etc/security/cacerts/*                        the certificate authorities apps trust (core/cacerts), for conscrypt
usr/icu/icudt60l.dat                          ICU's data
usr/share/zoneinfo/tzdata, tzlookup.xml       the time zones
```

1. The six DEX files are mapped and their classes noted. Nothing is linked but
   what the runtime itself needs (`Object`, `Class`, `String`, the exceptions
   it throws).
2. The runtime's own natives are registered, and the stand-ins after them.
3. Where guest code can run and `lib/libjavacore.so` is there: the folder is
   mounted as `/system` in the layer's file system (the layer's environment
   already says `ANDROID_ROOT=/system`, `ANDROID_DATA=/data`);
   `libopenjdkjvm.so` becomes a system library answered by `mg_dvm_jvm.c` (the
   24 `JVM_*` and `jio_*` functions `libopenjdk.so` imports, written against
   `art/openjdkjvm/OpenjdkJvm.cc`); `libnativehelper.so` is loaded and its
   `JniConstants::init` called, which Android's runtime does before either
   library; then `libjavacore.so` and `libopenjdk.so`, each with its
   `JNI_OnLoad`.
4. Nothing more. `java.lang.System` initialises when something first touches
   it (in practice during step 3), and sets its properties and streams itself.

The two jars are there because ICU4J reads its configuration out of
`core-libart.jar` by the name `VMClassLoader.getBootClassPathEntries` gives it;
without them `android.icu.impl.ICUConfig` fails to initialise.

`System.loadLibrary` comes to `JVM_NativeLoad` (`Runtime.nativeLoad`): the
layer's linker, then the library's `JNI_OnLoad`. See "An app's native
libraries" below.

### The Java driver

`core/src/drivers/mg_java.c`. `mg_apk.c` names the engine `java` for an APK
with `classes.dex` and a launcher activity that no other engine was recognised
in, with native libraries or without, and such an APK is runnable when it has
no native code or has it for 32-bit ARM.

- **Where the files are**: `megler_run_config.android_root`, or the folder
  `android` in the package (`megler_config.package_dir`) when that is NULL. The
  folder is only read. A Java app started without it is refused with a
  sentence that says what is missing.
- **The start**, on a thread that stands for Android's main thread (4 MB of
  host stack; bytecode's frames are not on it): `mg_dvm_start_android`, the
  framework's DEX, the APK's (`mg_dvm_app_follows` between the two), the
  natives of `megler.Native` and `megler.Gfx`, and then one call:
  `megler.AppRunner.run(activity, application)`. From there it is the
  framework's own Java: it reads the manifest itself,
  prepares the main `Looper`, makes the app's class loader, the `Application`
  and its `Context`, starts the content providers, calls
  `Application.onCreate`, makes the launcher `Activity` and calls `onCreate`,
  `onStart`, `onResume`, and loops.
- **The platform's libraries an app asks for.** Android's legacy Apache HTTP
  client, `org.apache.http.legacy`, is not on Android 9's boot class path. An
  app gets it on its own class path, ahead of its APK, when its manifest asks
  for it (`<uses-library>`, required or not) or when it targets an Android
  before 9, as Android 9's package manager gives it. Here the Java driver adds
  `framework/org.apache.http.legacy.dex` from the Android root right after
  `mg_dvm_app_follows` (`add_shared_libraries`), so its classes are the app's
  class loader's. `mg_apk.c` reads the manifest's `<uses-library>` names into
  `uses_libraries`, and the APK report lists them as `usesLibraries`. The DEX
  file is the emulator image's `oat/arm64/org.apache.http.legacy.boot.vdex`,
  taken out by `tools/android9-unpack.sh` and turned into DEX by
  `tools/android9-boot.sh` like the boot jars. Instagram 333 asks for it: its
  network library `liger` finds `org.apache.http.Header` in its `JNI_OnLoad`,
  and without it the app ended with "Failed to call JNI_OnLoad from 'liger'".
  The data app asks for it too, and its step "apache-http-legacy" makes and
  reads a `BasicHeader` through it and finds the classes are its own loader's.
  Its client, `AndroidHttpClient.newInstance`, registers https with the
  platform's `SSLCertificateSocketFactory.getHttpSocketFactory`, which the SDK
  dropped at 23 and the platform kept for it: Megler's framework had it not,
  and Google's GSF, which makes its `GoogleHttpClient` so, could not start
  its `SubscribedFeedsProvider` (Gmail asks it at every start: a
  `NoSuchMethodError`, 576). The step "apache-http-client" makes the client
  as GSF does, with the app's context, and finds https on 443 through
  Apache's `SSLSocketFactory`.
- **Input is asked for, not delivered.** The framework's main loop calls
  `Native.pollInput` between messages, every 8 ms at most. `megler_touch`,
  `megler_key` and `megler_set_paused` only queue (256 events; a move may be
  dropped when the queue is full, what ends a gesture is not), so everything
  that happens to the app happens on its main thread. Back is one event, when
  the key comes up; other keys pass as they are. `megler_back` sends it
  numbered and waits for what the app did with it: `AppRunner.back`
  answers once the key has been through the app (it went to the back,
  ended its last activity, asked for a frame -- a page or a dialog went --
  or none of these), and `moveTaskToBack` (Android 12's Back on a task's
  root activity among its callers) says the app went to the back. `megler_set_focused` has no
  counterpart in the framework and is not passed on.
- **Moves are batched, as Android batches them** (since 667). A finger's
  move is kept, not dispatched (`AppRunner.moveBatched`). The moves after it
  join it as its history (`MotionEvent.getHistorySize`, `getHistoricalX`
  and the rest; `addBatch`), and the one event goes out at the next frame,
  first in it (`Choreographer.CALLBACK_INPUT`). Anything else that comes (a
  finger down or up, a key, the host's pause) has the kept moves go out
  first, so the order holds. `VelocityTracker` reads every sample of the
  history, so a fling is as fast as the finger was. Before, every move went
  out on its own: `pollInput` hands over all that came while the main thread
  was busy. Spotify on a Lumia 650 took 50 to 200 ms a move in its scroll
  listeners and worked through hundreds of them, one stretch of 25.6 s
  without a frame, and its list answered a finger seconds late.
- **Frames are the framework's business**: its `Choreographer` and `ViewRoot`
  draw when something was invalidated, into a bitmap the size of the screen,
  and call `Gfx.present`. That is `megler_window_host.present_pixels`: the
  picture as memory (B, G, R, A premultiplied, top row first), which the shell
  must copy before it returns. A picture comes
  when something changed, not at a steady rate. Once the app draws with
  OpenGL ES on a view, the picture is composed with the GL frames first
  (`mg_java_gl.c`) and a GL frame comes through the same call from the
  thread that swapped it (docs/ARCHITECTURE.md, "OpenGL ES from an app's
  Java"). A picture of which only part
  was drawn again is handed with its rectangle through `present_pixels_part`
  when the shell has that call ("Drawing only what changed", above). Where a
  frame's time goes is said in the log every five seconds while the app draws
  ("The frame meter", above).
- **The end**: when its last activity is gone the framework calls
  `Native.exit`, which has the main `Looper` quit; `AppRunner.run` returns and
  the app is reported ended with status 0. `megler_stop` sets a flag the next
  poll sees, which finishes every activity on the main thread, and the same
  follows. An exception that reaches the top of the main thread ends the app,
  as on Android (whose default handler logs "FATAL EXCEPTION: main" and kills
  the process), with status -1. The reason, in `on_exit` and in
  `megler_last_error`, is one sentence a dialog can show: "the app stopped
  because nothing caught <the exception>, caused by <its innermost cause> (in
  <the innermost method of the app's own on its stack>), which ends an app on
  Android too". The exception and the cause are cut to 150 bytes each, causes
  in between are left out; the whole of it, with the stack, is in the log.
- **The APK's files**: `Native.readApkFile` and the asset calls read the
  app's APK (0), the platform's resources (1: `framework/framework-res.apk` of
  the root, when it is there) and the app's split APKs (2 to 11:
  `megler_run_config.splits_dir` and `split_apks`; "Split APKs and bundles"
  below) through `zip/mg_zip.c`; a stored file can also be had as a
  descriptor and an offset. An asset is looked for in the base and then in
  the splits, and a folder of assets is listed from all of them.
- **What is not answered yet** answers "no" in a way the framework's Java
  already handles, said once in the log for each class, with the methods that
  first asked (so that it can be told whether the app wants it or the
  framework's own start-up touched it): `megler.Web` (an empty rectangle).
  (The sensors, `megler.Sensors`, are answered now: "Sensors", below.) Also not asked of the
  shell yet: the keyboard, the clipboard (it is the app's own), sharing,
  opening a URL, insets (zero), dark mode (no), vibration.
- **Which way up.** The surface is whatever `megler_run_config` says as the
  app starts, upright or on its side, and whatever `megler_set_surface` says
  after that (its size, density and rotation: "The screen resized and turned",
  below); nothing in the driver assumes either shape. What the app *asks* for
  is known twice: before it runs, from the manifest (`megler_apk_report`'s
  `"orientation"`: the launcher activity's `android:screenOrientation` as
  `landscape`, `portrait` or `unspecified`, an alias taking its target's), and
  while it runs: the activity in front's `android:screenOrientation` or
  `setRequestedOrientation`, which `megler.AppRunner.updateOrientation` works
  out as the activities come and go and `Native.setOrientation` hands to the
  shell (`megler_display_host.orientation`) when it changes.

### An app's native libraries

An APK with `classes.dex`, a launcher activity and native libraries is the
engine `java` like one without (`mg_apk.c`), provided the libraries include
32-bit ARM ones (`armeabi-v7a`, else `armeabi`); one with only `arm64-v8a` or
x86 libraries is refused with a sentence that says so. In an app in several
APKs the libraries are in its split for the processor
(`config.armeabi_v7a.apk`): the report looks for them in every APK of the
app, and the linker takes them from whichever has them ("Split APKs and
bundles" below). The engines that are
recognised by their libraries (Unity, a NativeActivity and the rest) are looked
for first and keep their drivers.

**Two kinds of library** (`ld/mg_ld.h`, "The runtime's libraries"). The
runtime's own -- Android's `libjavacore.so`, `libopenjdk.so` and what they
need, from the Android root -- and the app's, from its APK, are loaded by the
same linker and never see each other: a `DT_NEEDED` is looked for where the
library that needs it came from and nowhere else, an import that is neither in
the importing library's own scope nor the system's is looked for among
libraries of the same kind only, and `dlopen` and `dlsym(RTLD_DEFAULT)` are the
app's. So an app may carry a `libcrypto.so` or `libicuuc.so` of its own, as
many do; it gets its own and the runtime keeps Android's. This is what
Android's linker namespaces do. The system's libraries (`libc.so`,
`liblog.so`, `libandroid.so`, `libjnigraphics.so`: the shims) are one for both.
And one exception, Android's own: Android 7 to 9 list ICU's two libraries among
the public ones (`public.libraries.android.txt`), so an app that does not carry
them may `dlopen("libicuuc.so")` and `"libicui18n.so"` and find their
functions by ICU's versioned names (`ucnv_open_60`). `dlopen` gives such an app
the runtime's copies, which libcore's start has loaded
(`bionic/mg_libdl.c`, `root_public_lib`); without the root they are not found.
MX Player's player library (`libmxvp.so`) opens both for subtitles' character
sets, and its `native_init` threw an `UnsatisfiedLinkError` when it could not:
the app's "Component files are corrupted or Android internal modules are not
compatible" message (`error_load_components`, `ActivityMessenger`). Under qemu
it now starts to its main screen. Only `dlopen`: a library that names ICU in
its `DT_NEEDED` still looks only in its own APK.

**`System.loadLibrary`** is libcore's own Java all the way to
`Runtime.nativeLoad`. `VMStack.getCallingClassLoader` gives the loader of the
class that called -- the app's `PathClassLoader` for a class of the app's --
whose `findLibrary` looks in `ApplicationInfo.nativeLibraryDir`
(`/data/data/<package>/lib`) and opens the file to see that it is there. The
Java driver has put each of the APK's libraries there as a real file when the
app starts (`place_libraries` in `mg_java.c`; written only when absent or of
another size, so only an app's first start pays for it): the class loader
needs them to exist, and so do apps -- loaders that read a library's ELF header
to load what it needs first (Facebook's SoLoader, which every React Native app
has) and code that lists the folder. Real files are simpler than empty
stand-ins there with the APK's entry served when one is opened, and cost the
libraries' size on the phone's storage once.
`JVM_NativeLoad` (`mg_dvm_jvm.c`) then loads: with no class loader (one of
the runtime's classes asked) a library of the Android root's, otherwise the
APK's of that file name. A path that names a file with something in it that is
not in the APK -- a library the app unpacked or downloaded itself -- is loaded
from that file. A library is loaded once: a second request finds it and does
not run `JNI_OnLoad` again. What comes back when it fails is ART's sentence
(`dlopen failed: library "..." not found`, `JNI_ERR returned from JNI_OnLoad in
"..."`, `Bad JNI version returned from JNI_OnLoad in "...": N`); a library the
APK does not have at all never gets that far, and is libcore's own
`UnsatisfiedLinkError` with the class loader in it.

**The system's libraries as files.** The core answers `libc.so`, `libz.so`,
`libandroid.so` and the rest of `mg_bionic_is_system_lib`'s list itself, by
name, so on Android's they are files in `/system/lib` and here they were not.
Facebook's SoLoader looks for each system library an app library needs in
`/system/lib` (a `DirectorySoSource` on the library path) before loading the
app's, and Instagram 333 ended in `attachBaseContext` with "couldn't find DSO
to load: libz.so", a dependency of its `libdextricksmerged.so`.
`tools/dvm-android-root.sh` now puts a file of each name of that list in the
root's `lib` (read from `mg_bionic.c`, so the list is kept in one place), a
line of text that says what it is; `/system/lib` is that folder. Nothing reads
them as libraries: the linker, `dlopen` and `JVM_NativeLoad` take a system
library by its name before they look for a file, as they did. The data app's
step "system-libs" finds `libz.so`, `libandroid.so` and `liblog.so` there, no
file of a name that is not one, and `System.loadLibrary("z")`, which finds the
file through the class loader, loads.

**The linker's own entry points.** Android 8 and later's `libdl.so` is a thin
layer over the linker's `__loader_dlopen`, `__loader_dlsym` and the rest
(`bionic/linker/dlfcn.cpp`), each with the caller's address added, by which
the linker picks the caller's namespace. Facebook's `libforce_dlopen.so`
looks up `dl_iterate_phdr` and `__loader_dlopen` in `libdl.so` to open a
library as a library already loaded would; Instagram's app state logger
(`AppStateLoggerNative.registerSelfSigkillHandlers`) threw "Could not find
__loader_dlopen from libdl.so" and the app ended. `mg_libdl.c` now has them,
each the plain function with the caller's address ignored, since there are
no namespaces here. The guest test `t_loader_dl` looks them up in `libdl.so`,
opens `libc.so` and finds `strlen` through them, and is refused a library that
is not there, with an error to read.

**Binding.** A native method is looked for in this order: the C tables of the
runtime and the drivers (`mg_dvm_register`); what a library registered with
`RegisterNatives`; a function a library exports as `Java_<class>_<method>`,
then with `__<parameters>`; the fallback tables. Among libraries, a method of
a class of the app's is looked for in the app's libraries first and a method of
the runtime's classes in the runtime's; the other kind is a last resort that
ART does not have (ART looks only in the libraries of the class's own loader;
here "the app's classes" are only known where the Java driver has said so).
When two libraries of one kind export the same `Java_` name, the one loaded
first has it (ART 9: the one whose path sorts first; either way it is an
accident of the app's). A method nothing has is ART's
`UnsatisfiedLinkError: No implementation found for <method> (tried <name> and
<name>)`.

**`FindClass`** does what ART does (`GetClassLoader` in
`art/runtime/jni_internal.cc`). Inside `JNI_OnLoad` it is asked of the class
loader the library is being loaded for; inside a native method, of the loader
of that method's class; on a thread with no Java method under it -- one the
library made and attached -- of the system class loader, which knows Android's
classes and none of the app's. The last rule is a trap apps are written around
(they look their classes up in `JNI_OnLoad` and keep global references), and
keeping it is cheaper than explaining an app that works here and not on
Android. A class that is not there is `ClassNotFoundException` (through a
class loader) or `NoClassDefFoundError` (for a caller that is one of the
runtime's classes), and no class is made for the name; in every other engine a
name nothing defines is a class of the layer's own, made by being asked for,
as it always was.

**Threads.** A thread Java started, and every thread the layer runs the app
on, is attached from its first moment. A thread the app's native code made
with `pthread_create` is not: `GetEnv` answers `JNI_EDETACHED` for it until it
calls `AttachCurrentThread` (or `AsDaemon`), which gives it its
`java.lang.Thread` at once -- the name and group of its `JavaVMAttachArgs`,
or `Thread-N` in the main group -- by the constructor ART uses, and tells the
group. `DetachCurrentThread` ends that Thread as Java ends one (an exception
still pending goes to `dispatchUncaughtException`, the group forgets it,
`join()` returns). `GetEnv` and `AttachCurrentThread` take JNI versions 1.2,
1.4 and 1.6 and no other, as ART does. This holds for every engine, not only
for Java apps: it is what a library is written against.

**The NDK.** `AAssetManager_fromJava` is the APK's assets whatever object it
is given. `AndroidBitmap_getInfo`, `_lockPixels` and `_unlockPixels`
(`drivers/mg_java_ndk.c`) work on the framework's `Bitmap`: the memory is the
core's, B, G, R, A, and is turned to Android's R, G, B, A where it lies while
it is locked; every bitmap is 32 bits a pixel here and `RGBA_8888` is what is
reported, whatever its `Config` says. `ALooper_forThread` on a thread that
runs a Java `Looper` gives a looper made at that moment, and the Java message
loop runs its descriptors' callbacks between messages (`Native.
nativeLooperPoll`); a message loop nobody's native code asked about has none
and costs nothing. `AConfiguration` describes the surface the app was started
on -- density, size in dp, orientation -- and says "en", "US" for the locale
whatever Java's says. The API level is one number for the app's Java and its
native code: the framework's `Build.VERSION.SDK_INT` (34) is put into
`ro.build.version.sdk` when a Java app starts, and
`android_get_device_api_level` and `AConfiguration_getSdkVersion` read that.
`ANativeWindow_fromSurface` gives the window a `Surface` of the engine drivers'
carries, and for the framework's `Surface` of a `SurfaceView` a window of the
view's surface (of a `SurfaceTexture`, a window of the texture), which native
code draws on with EGL or in software: "A SurfaceView drawn by native code"
below.

**The JNI functions on the runtime's objects**, down the table. "app": a step
of the `natives` test app calls it from a library built by the NDK; "core":
the core's own JNI test does (`mg_dvm_tests_jni.c`); "libcore": Android's own
libraries call it all the time on the way to a running app, which is evidence
and not a test.

| Function | On the runtime's objects | Tried by |
|---|---|---|
| `GetVersion` | 1.6 | app |
| `DefineClass` | NULL and a line in the log, as on Android | nothing |
| `FindClass` | ART's rule for who asks (above); links, does not initialise | app, core, libcore |
| `FromReflectedMethod`, `FromReflectedField` | libcore's real `Method`, `Constructor`, `Field` | app |
| `ToReflectedMethod`, `ToReflectedField` | the same objects reflection gives | app |
| `GetSuperclass`, `IsAssignableFrom` | right; an interface has no superclass | app, core |
| `Throw`, `ThrowNew` | right; `ThrowNew` runs the `(String)` constructor | app, core, libcore |
| `ExceptionOccurred`, `ExceptionCheck`, `ExceptionClear` | right; one pending exception a thread, the runtime's | app, core, libcore |
| `ExceptionDescribe` | the exception and its stack as one entry of the log, left pending (Android prints through `Throwable.printStackTrace`) | app |
| `FatalError` | ends the process with the message | nothing |
| `PushLocalFrame`, `PopLocalFrame`, `EnsureLocalCapacity` | nothing to do: a reference is the object and nothing is collected; `PopLocalFrame` hands back what it is given | app |
| `NewGlobalRef`, `NewLocalRef`, `NewWeakGlobalRef` and their `Delete`s | the object itself. A weak reference never goes null, since nothing is collected | app, libcore |
| `GetObjectRefType` | global when native code holds a global reference to the object, weak when only a weak one, local otherwise, invalid for NULL. A count for each object, since a reference is the object's address; exact unless a library asks about a local reference to an object it also holds globally | app |
| `IsSameObject` | right | app |
| `AllocObject` | the object with no constructor run; for `java.lang.String` an empty string of its own, as ART gives, which a `String` constructor called on it through `Call(Nonvirtual)VoidMethod` then fills, as StringFactory's method with the same parameters (ART turns the call into that and the reference into the new string; here the object takes its contents) | app ("string-alloc") |
| `NewObject`, `V`, `A` | right; a `String` is made whole from the arguments | app (`...`), core (`A`), libcore (`V`) |
| `GetObjectClass`, `IsInstanceOf` | right | app, core |
| `GetMethodID`, `GetStaticMethodID` | initialises the class; `NoSuchMethodError` | app, core, libcore |
| `Call<Type>Method`, `V`, `A` (ten types) | virtual dispatch on the object's class; a `float` among `...` arrives as the `double` C makes of it | app: Int, Object, Void; core: Object, Int (`A`); the other types by libcore |
| `CallNonvirtual<Type>Method`, `V`, `A` | the method itself | core: Int (`A`) |
| `CallStatic<Type>Method`, `V`, `A` | right | app: Object, Int, Boolean; core: Object, Int, Long (`A`) |
| `GetFieldID`, `GetStaticFieldID` | by name and type, superclasses included; `NoSuchFieldError` | app, core, libcore |
| `Get/Set<Type>Field` (nine types) | the object's slot | app: Int, Object; core: Object, Int; libcore |
| `GetStatic/SetStatic<Type>Field` | the class's slot, the class initialised first | app: Long, Int, Object; core: Long |
| `NewString`, `GetStringLength`, `GetStringChars`, `ReleaseStringChars` | UTF-16 as the runtime keeps it; never a copy | app, core |
| `NewStringUTF`, `GetStringUTFLength`, `GetStringUTFChars`, `ReleaseStringUTFChars` | modified UTF-8, made once a string and kept | app, core, libcore |
| `GetStringRegion`, `GetStringUTFRegion` | right; a region outside the string copies nothing and is `StringIndexOutOfBoundsException` | app |
| `GetStringCritical`, `ReleaseStringCritical` | the same memory as `GetStringChars` | app |
| `GetArrayLength` | right | app, core |
| `NewObjectArray`, `Get/SetObjectArrayElement` | right; `ArrayIndexOutOfBoundsException` outside the array, `ArrayStoreException` for an element of the wrong class | app, core |
| `New<Type>Array` (eight) | right | app: Long; core: Int |
| `Get/Release<Type>ArrayElements` | the array's own memory, never a copy, so `JNI_ABORT` cannot undo | app: Int; core: Int |
| `Get/Set<Type>ArrayRegion` | right; a region outside the array copies nothing and is `ArrayIndexOutOfBoundsException` | app: all but Double; core: Int |
| `GetPrimitiveArrayCritical`, `Release` | the array's own memory | app |
| `RegisterNatives` | a name the class does not declare native is said in the log and the rest taken (Android refuses the table with `NoSuchMethodError`) | app, core, libcore (569 methods) |
| `UnregisterNatives` | does nothing | nothing |
| `MonitorEnter`, `MonitorExit` | the object's monitor, the one `synchronized` uses; `IllegalMonitorStateException` | app, core |
| `GetJavaVM` | the one VM | app |
| `NewDirectByteBuffer`, `GetDirectBufferAddress`, `GetDirectBufferCapacity` | libcore's real `DirectByteBuffer`, through the constructor it keeps for this | app, libcore |
| `JavaVM`: `GetEnv`, `AttachCurrentThread`, `AttachCurrentThreadAsDaemon`, `DetachCurrentThread` | above, "Threads" | app |
| `JavaVM`: `DestroyJavaVM` | does nothing | nothing |

`GetObjectRefType` answered "local" for everything until the table of
counts it now reads (`ref_counted` in `mg_jni.c`); the table forgets an
object once nothing references it globally, so it grows with what is
referenced, not with what ever was.

### A Flutter app

A Flutter app is a Java app: its activity (`FlutterActivity`, or the app's
own on top of it) loads Flutter's engine, `libflutter.so`, and the engine
runs the app's Dart code, compiled ahead of time into `libapp.so`, and draws
into the activity's `SurfaceView`. The APK's report took the engine's
library for an engine of its own with no driver ("there is no driver for that
engine yet"), so no Flutter app could be started. It is a Java app now, with
a note that it is built with Flutter (`classify` in `mg_apk.c`; the bundle
tests make one of the natives test app with the two libraries beside its own).

What the engine asks of the system was checked against what is here, on its
dynamic imports (a release engine of May 2026, Dart 3.12, 481 of them): an ARM library that
imports the same names, loaded with `mg_tests --load` under qemu, left eight
unresolved besides OpenGL ES and EGL (which are ANGLE's on the phone, and all
there): `readlinkat`, `symlinkat`, `utimensat`, `inotify_init1` and
`close_range` of dart:io, now there as the other `*at` calls are (the
directory descriptor not looked at), and BoringSSL's three weak
`OPENSSL_memory_*` hooks, which are meant to be absent. What it looks up at
run time: `libvulkan.so` is not there, and the engine draws with Impeller's
OpenGL ES backend instead; `AChoreographer_*` (API 29) are not, and it waits
for the display's frames through Java's `Choreographer`; `ASurfaceControl_*`
answer none. Each lookup in the system's libraries that finds nothing now
says so in the log, once (`dl: dlsym(NAME) found nothing`). The Dart VM
reads `/proc/cpuinfo` for ARMv7 and its features (vfp, neon, idiva), which
says so, as `getauxval(AT_HWCAP)` does. No Flutter app has been run here yet:
the next step is a phone's log of one.

### A SurfaceView drawn by native code

A game whose activity is Java and whose engine is native hands the engine the
`Surface` of a `SurfaceView` and lets it draw there from a thread of its own.
Asphalt 9 does, and stayed black after its splash while
`ANativeWindow_fromSurface` gave NULL. What its `libAsphalt9.so` calls, in
order (its dynamic imports, and the code that calls them, disassembled):

1. On the main thread, in its `MainActivity.surfaceChanged`:
   `JNIBridge.NativeSurfaceChanged(surface, width, height)` calls
   `ANativeWindow_fromSurface(env, surface)` and hands the window and the size
   to the engine. Nothing in `surfaceCreated`; `surfaceRedrawNeeded` only
   signals the engine.
2. On the engine's render thread: `eglGetDisplay(0)`, `eglInitialize`,
   `eglGetConfigs` twice and `eglGetConfigAttrib` on each (a window bit in
   `EGL_SURFACE_TYPE`, OpenGL ES 2 or 3 renderable, 8888 or 565, then depth,
   stencil and samples), `eglCreateContext` with `EGL_CONTEXT_CLIENT_VERSION`
   and `EGL_CONTEXT_MINOR_VERSION`, a 1 x 1 pbuffer and a shared context for
   its loading thread; then for the window `ANativeWindow_getFormat`,
   `ANativeWindow_setBuffersGeometry(window, width, height, that format)` with
   the size it renders at, `eglCreateWindowSurface(display, config, window,
   NULL)`, `eglMakeCurrent`, and `eglSwapBuffers` each frame. When its
   resolution changes it sets the geometry again on the window it has.
3. In `surfaceDestroyed`: `NativeSurfaceDestroyed` tells the engine,
   `ANativeWindow_release` at once, and the window set to none.

It never asks the window's width or height, never locks it, and loads only
OpenGL ES functions with `eglGetProcAddress`.

**The window** (`core/src/drivers/mg_java_ndk.c`). For a `Surface` whose
`meglerView` is a `SurfaceView`, `ANativeWindow_fromSurface` gives a window of
the view's surface while it has one (NULL before `surfaceCreated` and after
`surfaceDestroyed`, as Android's `Surface` is not valid then), the same one
for every call while the surface lasts, each call a reference of the
caller's. Its size is the surface's (the view's `mSurfaceW` and `mSurfaceH`,
which the view sets before calling `surfaceChanged`, a fixed size included)
unless `ANativeWindow_setBuffersGeometry` asked for another, and follows it;
its format is the holder's -- `OPAQUE`, the overlay's default, is
`RGBX_8888` as Android's compositor makes it, the translucent ones
`RGBA_8888` -- until the geometry names one. When the surface is destroyed --
`EGLNative.forget`, which the view now says for every surface after its
`surfaceDestroyed` callbacks -- the window is no longer valid, for good, as
an abandoned one is on Android: its width, height, format and geometry answer
`NO_INIT` (-19), a swap of an EGL surface on it fails with
`EGL_BAD_NATIVE_WINDOW` (what Android's own `GLSurfaceView` expects when
"the window manager has closed it"), a lock is refused. It is freed when
native code lets go of its last reference, whenever that is; a surface that
comes back is a new window. The view stays reachable for the collector while
a window of it is held. Sixteen windows at once at most.

**EGL on it** (`ndk/mg_egl.c`, `drivers/mg_java_gl.c`). An EGL window surface
on such a window is the window's own, and it is what a window surface made
from Java on the view is (`EGLNative.createWindowSurface`): a pbuffer of the
window's buffer size on the view's layer, made with an 8888 configuration
nearest the app's (its own first, as for the app's whole window), the view
told it is drawn with EGL (`meglerSetGL`: its hole, and where it is said with
every picture of the views), and each swap through the same `swap()` -- the
frame handed to the shell by its texture's shared handle and composed under
the views on the graphics processor, or read back and composed by the core
where the shell takes no frames, and paced at sixty a second. The context is
the app's own, made by its `eglCreateContext` in whatever version and current
on its thread; the swap asks for its version at each change of context, for
what a read-back must restore. The size: on Android a window's next buffer
has the window's size, so an EGL surface follows a `surfaceChanged` or a new
geometry at its next swap. ANGLE's pbuffers have one size each, so what the
app is given as its `EGLSurface` is an entry of the layer's, which stands for
whichever pbuffer the window has; a swap that finds the window's buffer size
changed makes a new pbuffer of it, makes it current in the old one's place
and destroys the old. `eglMakeCurrent`, `eglQuerySurface`,
`eglSurfaceAttrib`, `eglSwapBuffers` and `eglDestroySurface` are given the
pbuffer for the app's surface, and `eglGetCurrentSurface` gives the app's
back. A swap of a surface that is not current on the calling thread is
`EGL_BAD_SURFACE`, as EGL has it. A surface destroyed while it is current on
some thread ends only when the last thread that has it current lets it go, as
EGL defers it: the layer counts the threads each surface is current on. HERE
WeGo's UI thread destroys its map's surface and makes a new one while its
render thread is still drawing a frame in the old; ended at once, the
pbuffer and the shell's texture went from under that frame and ANGLE faulted
(`Framebuffer11::markAttachmentsDirty`, packages 498 and 499). The natives
app's `texture-egl-elsewhere` destroys a surface another thread has current,
which then queries, draws on and swaps it before it lets it go. A window with
an EGL surface on it cannot be locked, and a locked one gets no EGL surface,
as on Android.

**In software.** `ANativeWindow_lock` gives a buffer of the window's own
memory, `RGBA_8888`, `RGBX_8888` or `RGB_565` as the window's format says, the
stride its width, kept from one frame to the next (Android copies the last
frame back into the new buffer, so an app may draw only its dirty rectangle;
on a new buffer the rectangle given back is all of it). `unlockAndPost` turns
it into one of two `Bitmap`s in turn, B, G, R, A and opaque, and hands that to
the view as a whole frame (`SurfaceView.meglerPostBitmap`, which the camera's
preview uses as well), which the view draws in its place scaled to its size;
the other one is written next, so the view never draws one being written.
Posts are paced at sixty a second, as a swap is.

The threads: `ANativeWindow_fromSurface` is called on the app's main thread
(Asphalt's case) or any other; EGL and the swaps on the app's render thread,
which need not be attached to Java (the layer calls the view's `meglerSetGL`
from it, which only posts an invalidation); `surfaceDestroyed`, and the
window's end with it, on the main thread while the render thread may be in
the middle of a swap. A swap reads whether the window is still valid before
it hands the frame over, and the composition shows nothing of a layer whose
view has said its surface is gone, so a frame swapped in that moment is not
shown and the next swap is refused; the render thread then destroys its
surface when it likes. Nothing of this waits for the main thread.

**A `SurfaceTexture`'s `Surface`** (a `TextureView`'s, or one an app made on a
texture of its own) has a window of the texture: its size the texture's default
buffer size (a `TextureView` sets it to its own), `RGBA_8888`, valid until the
texture is released. Its frames go to the texture through the framework's way
in for a native producer's frame (`SurfaceTexture.meglerPostRgba`), as `MeglerEGL`'s
read-back for a `TextureView` goes in through the texture's canvas: an EGL
surface on it is a pbuffer on no view, whose frame is read back at each swap as
R, G, B, A and handed over the right way up; a software buffer is handed over as R, G, B, A (565 and RGBX
turned to it, opaque). The `TextureView` draws the texture's front buffer
scaled to its size. Read back from the graphics processor at each frame and
drawn again by the shell, this is the slow way, as it is for Java's EGL on a
`TextureView`.

On the phone (package 270) Asphalt 9 had its window and swapped 28 frames a
second, every one handed to the shell, and showed nothing: ANGLE's pbuffers
are shared with keyed mutexes nobody held, so Direct3D dropped the game's
drawing and the shell's copies (docs/ARCHITECTURE.md, "Keyed mutexes"); ANGLE's
device now holds each pbuffer's mutex but while the shell copies.

What tells such cases apart on the phone without guessing: the flag
`trace-surfaces` (docs/STATUS.md, where the flags are), which every three
seconds reads back a window surface's frame just before it is handed over
and says its centre and a 4 x 4 grid of pixels, where its view last said it
is and who composes it, and what alpha the views' picture has at the same
grid over it (`00`: the hole); and `gl-compose-here`, which has the core
compose the window from frames read back instead of the shell. Without them
the log says each view's place as it is said (`java: OpenGL layer 0
(android/view/SurfaceView) is at ...`), the EGL calls for these surfaces EGL
refused (`egl: ... was refused (EGL error 0x...)`, and the swaps refused for a
surface gone or not current), and the OpenGL frames' minute lines end with who
composes and where each layer is.

Pausing the app here (the shell's `megler_set_paused`) is `onPause` alone and
destroys no surface; the surface goes when the view leaves its window (the
activity stopped, finished, or the view taken out of its layout), which is
when Android destroys it too.

Not there: attributes set with `eglSurfaceAttrib` are lost when a resize
makes the next pbuffer;
`ANativeWindow_setBuffersTransform` and `_setFrameRate` change nothing. (A
surface above the window, `setZOrderOnTop`, was composed nowhere up to package
681; it is over the views now, translucent where its format has alpha:
docs/ARCHITECTURE.md, "Surfaces over the window".)

*Tested* by the app `natives` under qemu, the window steps, twice (the shell
composing, and the core), with `trace-surfaces` on and its lines checked (the
frames' grids the quarters drawn, the views' alpha `00` over the hole): "A Java
app with native libraries" above.

### Split APKs and bundles

An app built as an Android App Bundle is installed by Google Play as a base
APK and splits: config splits for what depends on the phone -- the processor
(`config.armeabi_v7a`: the native libraries), the screen's density
(`config.xxhdpi`: the pictures) and the languages (`config.ru`: the strings)
-- and feature splits and asset packs with more code, resources and assets.
Outside Google Play they come as one file: an `.apks` (SAI's, bundletool's),
an `.xapk` (APKPure's, which may carry expansion files) or an `.apkm`
(APKMirror's: `base.apk` and `split_*.apk` with an `info.json`). Here such an app
is kept as its base and a folder of its splits, and runs as one app, as
Android runs an installed app with its splits.

**Unpacking** (`megler_bundle_unpack`, `core/src/apk/mg_bundle.c`). The shell
copies the picked bundle into its library and has the core unpack it into the
base's path and a folder beside it, then removes the copy. What is kept is
what an installer for this phone would install: the split for 32-bit ARM
(never another processor's: Windows 10 Mobile runs no other), the density
splits nearest the density a Java app is told the screen has (one at or
above it, one below), the splits of the languages the shell names (English
and the phone's own), and every split of another kind. The APKs are told
apart by their manifests (the base names no split; a split names itself),
and the unwanted are left out by their file names, which keep bundletool's
`config.<qualifier>` in all three layouts, so they are never unpacked. The
splits are written as `split_<name>.apk`, Android's names for installed
splits; at most ten, which is what the framework reads. An .xapk's expansion
files are written into the app's own `Android/obb/<package>/` (or
`Android/data`), where the app finds them when it first runs; anything that
would go elsewhere is left out. The folder's `bundle.json` records what was
kept, what was left out and why; the report reads it for the ABIs of the
splits left out. The answer to the shell is the same JSON.

**The report** (`megler_apk_report_splits`) is the base's manifest with the
native libraries of every APK, the engine decided on all of them, the label
and icon looked up in every APK's resources, and the record's left-out ABIs
among `abis`: a bundle whose only native code was for arm64 is refused with
the sentence that says so, as a plain APK with only 64-bit code is. A base
added alone whose manifest says it needs splits (`isSplitRequired`,
Google Play's `com.android.vending.splits.required`) gets a note; one whose
`requiredSplitTypes` names the ABI and has no native code is refused; a
split added alone is refused as no app.

**Running** (`megler_run_config.splits_dir`, gathered with `split_apks` into
one list in `run/mg_run.c`; a split of another app is dropped). The linker
takes native libraries out of whichever APK has them, so `place_libraries`
writes the split's into `nativeLibraryDir` as it does the base's. The Java
driver opens the splits as APKs 2 to 11, mounts each file beside the base as
`/data/app/<package>-1/split_<name>.apk` and answers those paths to
`Native.splitPaths`; the framework makes `ApplicationInfo.splitNames` (sorted)
and `splitSourceDirs` of them, and the app's `PathClassLoader` path. A
feature split's DEX files are added to the class path after the base's, and
the class loader finds them under the split's path (`DexFile`); a split
without code throws the `IOException` a resource-only APK throws on Android,
which `DexPathList` keeps aside. `PackageInfo.splitNames` is
`ApplicationInfo`'s. The resources are the framework's: `Resources` reads the
base's `resources.arsc` and merges every split's into the same package
(`ResTable.merge`: each configuration of a type joins the type's list,
reading its own table's bytes and strings), as Android's `ResTable` makes one
package group; a file is read from the base, else from the splits. Assets
are found in the base and then the splits, and listed from all
(`app_apk_with` in `mg_java.c`); native code's `AAssetManager` already looked
in every APK the linker has.

Not done: the components a feature split declares in its own manifest are not
known (the framework reads the base's manifest alone; bundletool merges an
install-time feature's components into the base's, which is the usual case);
isolated split loading and `<uses-split>` dependencies (one class loader has
everything); `PackageInfo` of splits' revision codes (zeros); more than ten
splits.

### Drawing: `megler.Gfx` and the shell

`android.graphics` in the framework is Java over the native methods of
`megler.Gfx`: the framework's own 54, and 11 more for display lists (below). Here they are `core/src/drivers/mg_java_gfx.c`, and the drawing
itself is the shell's, behind `megler_gfx_host` in `core/include/megler.h`, whose
comment is the contract: every encoding that crosses it, and what is expected
where the framework's code, written for iOS, leaned on CoreGraphics without saying.

- **A bitmap's pixels are the core's memory**: 32 bits a pixel, premultiplied,
  bytes B, G, R, A, top row first, at an address that does not change. An
  Android `Bitmap` is plain memory that Java reads in loops and native code
  holds the address of, and B, G, R, A premultiplied is what Direct2D and WIC
  draw into and what the phone's swap chain takes. Where Android's own order
  (R, G, B, A) shows -- `copyPixelsToBuffer`, texture uploads, a native lock
  on the bitmap -- the core converts.
- **The shell draws into memory it is given** (`canvas_new(pixels, ...)`), and
  may batch or draw on a surface of its own: the core calls `flush` before
  anyone reads the pixels and `touched` after it wrote them itself.
- **The core does what needs no drawing**: making, erasing, single pixels,
  copies and plain scaling of bitmaps; save counts and the matrix (the shell is
  always handed the whole matrix); the clip's bounding box; a paint unpacked
  into a struct; a path turned from the framework's fifteen operations into
  move, line, quad, cubic and close; arcs and round corners as cubics;
  underlines; path bounds, containment and length.
- **Text as outlines.** `Paint.getTextPath` asks the shell for the outlines
  of the run's glyphs where `drawText` would draw them, alignment, scale,
  skew and letter spacing included (`megler.Gfx.txPath`, the host's
  `text_path`: DirectWrite's own outlines, `IDWriteFontFace::GetGlyphRunOutline`,
  in `player/GfxHost.cpp`), and the path holds them alone afterwards, as
  Android's does. Google Maps' legacy renderer draws every name of its map
  that way into a bitmap -- one path for label after label, stroked in the
  halo's colour, then filled -- and draws the bitmap with OpenGL ES 1. Up to
  0.1.0.684 the framework added the run's box to the path at each call,
  which no graphics processor draws as letters: the names on Transit's map
  were grey boxes (a Lumia 650).
- **The stand-in** (`core/tests/mg_gfx_standin.c`) is for tests and is not a
  renderer to look at: a pixel is in when its centre is, curves are straight
  pieces, strokes have no joins, a gradient is its first colour, text is a box
  for each letter with fixed metrics (advance 0.6 of the size, ascent 0.8,
  descent 0.2), images are PNG only. It is deterministic on every machine,
  which is what the tests want of it.
- **Display lists.** A `RenderNode`'s canvas and a `Picture`'s record what
  they are given instead of drawing it, as Android's do, and the list is
  drawn again on whatever canvas draws the node or the picture. The core
  does the recording, at the level of these natives: a recording canvas is
  a canvas of `mg_java_gfx.c` with no bitmap (`cvRecord`), which keeps its
  save count, matrix and clip as any canvas does and writes every call that
  would reach the shell into its list, with the paint unpacked, the path
  built, the matrix and the text copied. What a call refers to is referred
  to: a bitmap is drawn with the pixels it has when the list is drawn, a
  shader with the local matrix it had when it was recorded, a node drawn
  into another's list is that node (`cvDrawNode`), drawn with its list and
  properties as they are when the outer list is drawn. Each is kept alive
  by the lists that refer to it. A node's properties (place, matrix, alpha,
  clips, outline, layer) are the Java's, handed over whole (`rnSet`) before
  anything is drawn that may draw the node. The framework as it came drew
  each node into a bitmap of its size, and kept the bitmap: Compose has a node for
  every graphics layer, dozens, many as large as the screen, and Spotify
  ended with `OutOfMemoryError: Bitmap 1650x2560` (`megler/Gfx.java.members`
  declares the natives; `android/graphics/RenderNode.java` below).
  Recorded in the core and not in Java because a Java recorder would copy a
  `Paint` for every call into a heap whose collector is the slow part on
  the phone, and drawing a node would run the interpreter over every call
  of every list at every frame; here drawing a node is one native call.
  Compose records a node for each graphics layer here too, where no
  window's canvas is hardware-accelerated, but draws a layer's content
  straight onto such a canvas (its `GraphicsLayer`'s software path): under
  qemu no node or list of Spotify's reached the window, and what the
  bitmaps had cost was the recordings'. `mg_gfx_natives_tests.c` draws
  every part of a `Paint`, and Spotify's play button (a white disc and a
  glyph of a face from a file's bytes, 224 px high under a scale of 0.46),
  directly, through a list and in a node, on the stand-in and, in the
  Windows build, on the shell: the same pixels every way.

### The screen resized and turned

The app's screen changes under it: a phone turned, a PC's window resized or
maximized. Android makes that a configuration change, and so does the
framework here. The contract is in `megler.h` (`megler_display_host`,
`megler_set_surface`).

- **The core.** `megler_set_surface(width, height, density_dpi, rotation)`
  keeps the surface as it now is (`mg_run_surface`, `run/mg_run.c`: the run
  config's until then); every driver reads its size, density and rotation
  there. The Java driver's `screenWidth`, `screenHeight`, `density` and
  `displayRotation` say it as it is now, and its main loop finds a change at
  its next poll, as it finds the battery's (`surface_news`): the NDK's
  `AConfiguration` is made the new size's and the framework is handed an
  event (`EV_SURFACE`, 11).
- **The configuration.** `megler.Screen` (new) does with the event what
  `ActivityThread` does with a new configuration. Every `Resources` of the
  app's (the app's, the system's, those of configuration contexts and of the
  packages it hosts: each implementation of them once, `ResourcesImpl.sAll`,
  as Android's `ResourcesManager` updates every `ResourcesImpl`) takes the screen's orientation,
  `screenWidthDp`, `screenHeightDp`, `smallestScreenWidthDp`, `screenLayout`
  and `densityDpi`, and `DisplayMetrics` the new size; the rest of their
  configuration stays, an app's own locale with it. The Application, the
  callbacks registered with it or with a context, the running services and
  the content providers hear `onConfigurationChanged`.
- **The activities.** Each activity on the screen meets the change now, a
  stopped one when it comes back (`AppRunner.ensureConfiguration`, as
  `ActivityRecord.ensureActivityConfiguration`): Back to it, a `CLEAR_TOP`
  that brings it back, the host's window shown again. One whose
  `android:configChanges` takes all that changed (and, for an app made for
  Android 3.1 or before, the screen's sizes always) hears
  `onConfigurationChanged`, which goes on to its fragments, its window and its
  action bar. Any other is relaunched (`AppRunner.relaunch`, as
  `handleRelaunchActivity`): paused, stopped, its state saved -- its views'
  too, each with an id (`Activity.onSaveInstanceState` puts the window's
  hierarchy in, as Android's does) -- its retained object kept
  (`onRetainNonConfigurationInstance`: androidx keeps its ViewModels in it),
  destroyed with `isChangingConfigurations` and `getChangingConfigurations`
  saying why; then the same component made again at its place in the stack,
  with the state in `onCreate` and `onRestoreInstanceState`, the retained
  object in `getLastNonConfigurationInstance` until it resumes, and its
  lifecycle state back (resumed; paused when it was under a translucent one;
  started for a result to come). Activities it had asked for results have the
  new one as their caller. `Activity.recreate` goes the same way; it had made
  the new activity on top of the stack, resumed, whatever the old one had
  been, and dropped what it retained.
- **Sizes the resources are made for.** As Android's
  `SizeConfigurationBuckets`, a change of the screen's size, smallest size or
  layout relaunches an activity only when it crosses a size the app's
  resources are made for (`w600dp`, `h480dp`, `sw720dp`, a screen layout's
  size or length: `ResTable.meglerSizeBuckets`; `getSizeConfigurations` says
  them too). Otherwise the activity is not relaunched and, unless it takes
  the change, not told: its views are laid out again. So dragging a PC's
  window does not restart an app that has no such resources. A change of
  orientation or density always counts.
- **The windows.** Every window is laid out again at the new size with its
  insets worked out again, the views of the windows that were there before
  the change hear it (`View.dispatchConfigurationChanged`), and the whole
  screen is drawn again into a bitmap of the new size
  (`ViewRoot.screenChanged`). The pictures that follow are of the new size; the
  window of views and OpenGL frames composed in the core follows them
  (`mg_java_gl.c`, `window_resized`). An activity's window added under one
  above it in the stack goes under that one's windows.
- **The display.** `Display.getRotation` says the rotation (`Display`,
  overlaid), and `DisplayManager`'s listeners, which were dropped, hear
  `onDisplayChanged` on their handlers. A screen turned upside down changes
  no configuration: only these say it.
- **Which way up the app wants it.** The activity in front's
  `android:screenOrientation` or `setRequestedOrientation`, "behind" and an
  activity that does not fill the screen and asks nothing (translucent or
  floating) leaving it to the one below (`AppRunner.updateOrientation`), goes
  to the shell when it changes (`Native.setOrientation`,
  `megler_display_host.orientation`). It was only logged, from the window's
  first `getWindow`.

The shell's side (`ui/PlayerScreen.cpp`, `player/DisplayHost.cpp`): the wish
becomes Windows' `DisplayOrientations` for `AutoRotationPreferences` -- an app
that asks for nothing turns with the phone; a game, whose engine says nothing,
lies down as before -- and the surface follows the window once its size has
held for a poll (at once when the display turned), with the start's density
for a Java app, as Android keeps a window's density when it is resized. A
window that does not turn by itself (a PC's) and is not of the shape the app
asks for is turned, and otherwise (an Xbox's television) the app gets a
surface of its shape in its middle, letterboxed. The core's NDK drivers
follow it too (docs/ARCHITECTURE.md, "Graphics"). Tested by the screen app
("The screen resized and turned", above).

### The phone's bars

A Java app's status bar and navigation bar are the phone's own, as on
Android, where they are a window's insets. The app's surface stays the whole
screen, and the bars lie over it. The contract is in `megler.h`
(`megler_system_bars_host` and `megler_set_insets`).

- **The app asks.** At every frame `megler.ViewRoot` (overlaid, `updateBars`)
  works out which bars the top activity window wants hidden and tells the
  shell when that changes (`Native.systemBars` →
  `bars.wanted(bars)`, on the app's main thread):
  - the status bar for the window's `FLAG_FULLSCREEN` (the theme's
    `windowFullscreen` included), `SYSTEM_UI_FLAG_FULLSCREEN`, or
    `WindowInsetsController.hide(statusBars())`;
  - the navigation bar for `SYSTEM_UI_FLAG_HIDE_NAVIGATION` or
    `hide(navigationBars())`;
  - and whether the status bar is light (`SYSTEM_UI_FLAG_LIGHT_STATUS_BAR`,
    `APPEARANCE_LIGHT_STATUS_BARS`), so that its icons are to be dark.

  What the controller hides is kept by the window's decor (`megler.DecorView`,
  overlaid), which also answers before the window is attached, as Android's
  `PendingInsetsController` does. So what an app asks in `onCreate` counts.
- **The shell does it.** The player hides the phone's bars only when the app
  asks: full-screen mode (`TryEnterFullScreenMode`) for the navigation bar,
  which takes the status bar with it, or the status bar alone. A game, whose
  engine is not Java, has the whole screen as before. Over an app the status
  bar is see-through, so what the app's decor paints there (its status bar's
  colour) shows, with light or dark icons as the app's bar is
  (`ui/AppShell.cpp`, `applyFullScreen`). The player lies under the bars
  (`UseCoreWindow`). While the keyboard is up, the navigation bar comes back
  for its Back key, as before.
- **The shell tells.** `megler_set_insets(l, t, r, b)` says how many pixels
  of each edge of the surface the bars cover now. The player says it whenever
  the window's visible part changes: the bars hidden or shown at the app's
  word, the user's swipe that hides or brings back the navigation bar, a turn
  (`PlayerScreen.cpp`, `tellInsets`). The core keeps it, also for an app
  that has not started yet. `Native.insets()` gives it to the framework,
  whose decor keeps the content off the bars, and the app lays itself out
  again. Until then nothing was said, and the shell kept every app in full
  screen whatever it asked.

- **What the decor keeps off.** As Android decides a window's
  `fitInsetsTypes`, each bar by itself (`Window.meglerFitsStatusBar` and
  `meglerFitsNavigationBar`, overlaid members). The decor does not keep the
  content off a bar the app lays out under:
  - `setDecorFitsSystemWindows(false)` or `FLAG_LAYOUT_NO_LIMITS`, for both
    bars;
  - `SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN` or `FLAG_TRANSLUCENT_STATUS` for the
    status bar;
  - `_LAYOUT_HIDE_NAVIGATION` or `FLAG_TRANSLUCENT_NAVIGATION` for the
    navigation bar.

  A flag counts whether it is set on the decor, in the window's attributes,
  or on any view of the window. The other bar's inset is passed on to the
  app's views. The log says what the decor decided and why ("decor of ...:
  keeps the content off the status bar ..."). The phone has no display
  cutout: the framework was written for an iPhone, with a notch at the top,
  and the top inset reached apps as a cutout that outlived the decor's
  consuming the insets. An
  app that keeps off a cutout itself, as Instagram does, then kept off the
  status bar twice.

  A view marked by `makeOptionalFitsSystemWindows` (ViewGroup's marks its
  children too) that fits system windows applies no insets while its window
  is laid out under the bars (`SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN`,
  `_LAYOUT_HIDE_NAVIGATION`, or the decor not fitting), as Android's default
  content insets applier decides. AppCompat marks its own content root so
  (`FitWindowsLinearLayout`); it was padded by both bars inside Instagram's
  window, which keeps off them itself, and the feed had an empty band at the
  top and the bottom. The flag `trace-views` shows such padding.

Tested in the data app (both runs). Told that the bars cover 60 pixels at the
top and 90 at the bottom, its content moves down by 60 and shrinks by 150.
Then it hides the status bar by its window's `WindowInsetsController` (by
reflection: it is built against API 29), shows it, hides both bars by its
decor's system UI flags, and clears them. The test hears 0 at the first
frame, then 1, 0, 3, 0.

### The keyboard

A Java app's text fields are edited with the shell's touch keyboard. The
contract is in `megler.h` (`megler_keyboard_host` and the four `megler_text_*` /
`megler_keyboard_*` functions); the core's half is in `mg_java.c`, onto the
framework's own input-method glue (`megler.InputMethods`, which talks to the
focused view's `InputConnection`).

- **The app asks.** `megler.Native.showKeyboard(show, inputType, imeOptions)`
  becomes `keyboard.show(kind, flags, enter)` or `keyboard.hide()`. Android's
  two words about the field are turned into three the shell can use: what is
  typed (text, number, phone, e-mail, URL, password, numeric password, date),
  flags (several lines, no suggestions, capitals, signed, decimal), and what
  Enter does (a new line, Done, Go, Search, Send, Next, Previous). **Both are
  called on the app's main thread**, from inside the framework, and must
  return at once. `show` comes again for the same field (the framework asks
  when a field takes the focus and again when it is touched): ten times for
  five touched fields in the test.
- **The shell tells**, from any thread: `megler_text_commit` (typed text),
  `megler_text_key` (Backspace, Enter, Delete, the arrows, Home, End),
  `megler_keyboard_closed`, `megler_keyboard_height`. Each is queued and reaches
  the app on its main thread with the next turn of its message loop
  (`Native.pollInput`, 8 ms at most), in the order of the calls. Text,
  Backspace and Enter go to the framework as the strings its
  `InputMethods.deliverPending` reads (`t<text>`, `d`, `e`, `h` for "put
  away"), one for each "text is waiting" event, so that they stay in order
  with the keys that go as key events (Delete, arrows, Home, End: Android's
  `FORWARD_DEL`, `DPAD_*`, `MOVE_HOME`, `MOVE_END`, which the framework's
  `ArrowKeyMovementMethod` and key listener handle). The height is the fifth
  number of `Native.insets()`, the framework's keyboard inset.
- **Enter in a field of several lines** is sent as the text "\n". The
  framework's own `e` decides it the wrong way round: `InputMethods` makes a
  new line only where `IME_FLAG_NO_ENTER_ACTION` is *not* set, and a
  `TextView` of several lines always sets it, as Android's does (the flag
  means "Enter is no action here"). The core knows from `showKeyboard` what
  Enter is for the field and does not ask the framework twice. That fault is
  the framework's own (`core/framework`); the iOS shell it was written for
  sends line ends as text too.
- **Typed text goes where the field's own code leaves the cursor.** The
  framework's `BaseInputConnection` puts the cursor at the end of what is
  replaced before the text goes in, as Android's `replaceText` does, so that
  it moves with the edit and a `TextWatcher` that rewrites the text and puts
  the cursor itself has the last word. It used to put the cursor after the
  edit, at the replaced range's start plus the text's length: WhatsApp's phone
  number and code fields, which put dashes in as one types, had the cursor
  put back before the dash just added, and the next digit went in there, the
  digits out of order (package 481). The data app's "input-format" step types
  seven digits into a field that puts in two dashes, and a Backspace.

Tested in the demos app (`Typing`: four fields; 28 checks): a touch of a field
asks for the keyboard with the right kind, flags and Enter for each of the
four, on the app's main thread; "Hello" arrives; Backspace takes one letter
from the text and a fifth of the text-coloured pixels from the field (585 to
468: the stand-in draws a box a letter); two arrows left, then "X": "HeXll"
with the cursor at 3; Home and two Cyrillic letters; End, Delete (nothing),
"!"; Home, Delete; Enter is the field's action 6 (`IME_ACTION_DONE`); "4" and
"2.5" in the number field; a password; "one", Enter, "two" in the note is
"one\ntwo"; the keyboard's height and "put away" leave the app running and a
touch asks again; Back takes the screen away and `hide` is called.

The test also covers the order a phone's shell uses: the keyboard's height
comes in before anything is typed, and 0 after Done has put the keyboard
away.

**Where typed text went, in the log.** On the phone typed text has not yet
been seen to reach a field. The shell's log says whether its hidden box took
the focus and whether typing reached the box and was sent on. The core
(`push_text` in `mg_java.c`) says once that the first thing the keyboard sent
reached it and was queued for the app's main thread, or that it was dropped
because no Java app runs or the queue is full. The overlay's
`megler/InputMethods.java` says what happened to it next, without the text
itself. Once per run, the first thing the keyboard sends gets a line that
names the view with the focus (or none), says whether that view gave an input
connection, and for a `TextView` gives its text's length before and after. If
the window's focused view (the one keys go to, `ViewRoot.keyFocus`) is a
different view, the line names it too. After that a line is written only
when something changes: text arriving while no view has the focus, or while
the focused view has no input connection, and text reaching a field again.
For example:

    java: the first thing the keyboard sent (text) reached the core and waits for the app's main thread
    I/Megler: keyboard: the first thing the keyboard sent, typed text (1 character), finds no view with the focus: it goes nowhere
    I/Megler: keyboard: typed text (5 characters) now goes to android.widget.EditText, which has the focus and an input connection; its text was 0 characters long and is 5 now

The demos test checks all three lines. The app runner's script (`mg_tests
--java app`) can type too: `type:TEXT`, `textkey:N` and `kbd:PIXELS`. In
QuickShortcutMaker 2.5.0 under qemu, a tap on its search field, the
keyboard's height, "abc", Backspace and "Z" leave "abZ" in the field, and the
log says the text went to `android.widget.EditText` (0 characters, then 3).
No reason was found in the core or the framework why the phone would lose
typed text that this path delivers under qemu. The next phone log will show
at which of the three steps it goes missing.

**What a password field got, in numbers.** Facebook said a password was
wrong that the user had typed right (package 484), and its field shows dots.
Two lines say, without a character of it, whether the field got what was
typed. The shell, when a field is left or the keyboard put away, counts what
the keyboard did there: `player: in that field (kind 5) the keyboard typed N
characters and took back K by Backspace keys and T by changes to its text`,
and how often a key and a change came within 0.2 s of each other (one
Backspace told twice would take back two characters). And
`InputMethods` keeps, for a password field, what the keyboard's text and
Backspaces make of the field's text, typed at its end, and when the field is
left or its Enter pressed says whether the field holds that: `keyboard: the
password field ... holds what the keyboard typed (N characters)`, or holds a
different number and not what was typed. The demos test types "pw", a
Backspace and "w" into its password field and checks the first.

**What the keyboard does to a window is Android's.** The window's
`softInputMode` decides (`adjustMode` in the overlay's `megler/ViewRoot.java`):

- **adjustResize.** An activity's window gets the keyboard in its insets, and
  the decor keeps the content above it. A dialog is measured to fit the
  screen above the keyboard and placed there by its gravity (centred, as a
  rule). Its views get no keyboard inset, because none of the dialog is under
  the keyboard.
- **adjustPan.** The window keeps its size and place and is moved up (`mPan`).
  It moves only as far as brings the focused view's bottom edge to the
  keyboard's top, and no further than brings that view's top edge to the
  screen's top. The pan is worked out at every frame, since the focus can
  move without a layout.
- **adjustUnspecified.** Resize when a view of the window is a scroll
  container (a `ScrollView`, a list, or a view set so with
  `setScrollContainer`, which the overlay's `View` now keeps); pan when none
  is.
- **adjustNothing.** Nothing.

A dialog's theme (`Theme.Material.Dialog`) says adjustPan. An activity's
window now takes the manifest's `android:windowSoftInputMode` (the overlay's
`Activity.getWindow`, as `Activity.attach` does on Android).

What was wrong before, on the phone: Apktool M's search dialog had its field
above the screen's top edge while the user typed into it (0.1.0.145, a
keyboard over 513 of 1280 rows).
- Every dialog was laid out against the whole screen and then placed by
  gravity in the part above the keyboard. A dialog taller than that part
  went off the top.
- A dialog (`TYPE_APPLICATION`) also got the keyboard as an inset. A view
  that fits system windows turned it into padding, and that made the dialog
  taller still.
- Every activity was resized, whatever it asked.

Tested in demos ("dialogs and the keyboard", `KeyboardDialogs`; 39 checks),
with a keyboard over 500 of the 800 rows:

| dialog | mode | before | after |
| --- | --- | --- | --- |
| field at the top | resize | window 24 to 276, centred in 300 | the same |
| field at the bottom, 492 rows high | pan | centred anyway: window -96 to 396, field 320 to 380, under the keyboard | moved up 330: field 240 to 300, window -176 to 316 |
| field in a ScrollView | unspecified | resized | resized: window 24 to 276 |
| the theme's own mode | pan | resized | moved up 50: field 240 to 300 |

Once the keyboard is put away, each dialog is back where it was.

**From one field to the next.** An app that moves the focus to another field
often puts the keyboard away for the one it leaves and asks for it again for
the next at once (React Native's text inputs do), or the next field is of
another kind, for which the shell leaves the text service's focus and takes
it again. Windows tells of the keyboard's going only after the asking, and
the shell took that for the user's putting it away: the app was told the
keyboard was gone, and the keyboard's coming back was put away again "though
the app had not asked for it" -- Discord's password field, after its e-mail
field, never had a keyboard (0.1.0.573). A going Windows tells of within 0.7 s
of the shell's own (`ownHide_`, `ui/PlayerScreen.cpp`), while the app wants
the keyboard, is now taken for that: the keyboard is shown again for the field
that asked, and the log says so once ("player: the keyboard went as the app
moved from one field to another, and comes back for the field that asked for
it"). Not tried on a phone yet.

**The keyboard's coming and going is an inset animation** of
`WindowInsets.Type.ime()`, run through the views' callbacks
(`View.setWindowInsetsAnimationCallback`) as Android 11's `InsetsController`
runs it (`megler.ViewRoot`, `ImeAnimation`): `onPrepare` with the insets as
they were; the window's new insets (`onApplyWindowInsets`) and its layout;
`onStart`, at the end of that frame, with the bounds -- none and the
keyboard's height; `onProgress` at each frame of its 250 ms with the insets at
that point (decelerating); `onEnd`. A `ViewGroup` hands each call on to its
children unless its own callback's dispatch mode is `DISPATCH_MODE_STOP`, and
a new change ends the one running. `WindowInsetsAnimation` and its `Bounds`
were stubs that kept nothing (an overlay now), and the callback was never
kept: AndroidX's `WindowInsetsAnimationCompat` uses these on Android 11 and
later (Megler says 14), and React Native's keyboard controller
(`react-native-keyboard-controller`, which Discord's login and registration
screens are built on) moves what is above the keyboard only as such an
animation tells it. The log says the first one: "MeglerViews: the keyboard
comes as an inset animation of 250 ms to the views' callbacks". The demos
app's typing screen has a callback that says each call, and the test checks
them when the keyboard comes up.

**Not there:** composing text (a keyboard that composes sends each piece when
it is final), the selection handles and the copy and paste bar (the
framework's), and telling the shell where the cursor is on the screen.

### Permissions

An app's permissions are Android's runtime permissions, as a phone of Android 6
and later has them (`megler.Permissions`; before, every permission was granted
whatever its name and nothing was asked of anyone). For one permission:

- not in the manifest, or asked for only up to an Android before the one Megler
  says it is (`android:maxSdkVersion` below 34: `megler.Manifest` leaves it out,
  as Android's package parser does): denied;
- not dangerous -- normal, signature, the app's own, the special ones
  (`SYSTEM_ALERT_WINDOW`, `REQUEST_INSTALL_PACKAGES`), whose Settings screens are
  Megler's to answer: granted with the install;
- dangerous, of an app that targets Android 5.1 or older (or has no
  `<uses-sdk>`), or of Google's own packages (Play Store, Google Play services,
  its services framework), which a phone has as system apps: granted with the
  install -- unless the shell says the group is denied for good, the user's
  Don't allow in the app's App Permissions, as Android's settings revoke an old
  app's permission;
- any other dangerous one: its group's (Android 14's groups), as the shell keeps
  it (`megler_system_host.permission`, `Native.permission`): granted, denied
  (`shouldShowRequestPermissionRationale` true, the app may ask again), denied
  for good (Windows' own no, a second no, or the user's Don't allow in the
  app's App Permissions), or undecided (never asked, or asked every time and
  not yet in this run).

`requestPermissions` (an activity's or a platform `Fragment`'s) settles what
those rules settle and asks the shell, on a thread of its own, for each group
that is undecided or denied once, one after another; the answers come to
`onRequestPermissionsResult` on the main thread, the activity paused around
them and resumed after where an answer changed (Android's permission dialog
is an activity over the app's), not where none did, so that an app that asks
again in `onResume` when it is told no does not ask for ever. A second request
while one is answered is told at once that it was cancelled, as on Android.
`checkPermission` and its kind (`ContextImpl`), `PackageManager.checkPermission`,
`getPackageInfo`'s `requestedPermissionsFlags` and `getPermissionInfo`'s
protection level say the same. What the shell said of a group is kept until the
app comes back to the front (`AppRunner.hostPause`), where the user may have
changed Windows' settings meanwhile.

Google Play services' and its services framework's own contexts (`MicroG`,
`Gsf`) hold every permission, as their system apps do on a phone: their checks
of themselves are theirs, not the app's. `megler.Phone` gives the phone's
messages and calls to the app as it holds them. The location (`LocationService`)
has Windows ask the user only for an app that holds a location permission:
`isProviderEnabled` asked Windows in the background, and Google Play services in
an app that never wanted the location asks it of every provider, so Windows
asked such an app's user for the location. Nothing else is refused where an app
did not request a permission it was not given: the camera, the microphone and
the files are Windows' to refuse, as before (an app that opens the camera
without requesting it has Windows ask, the first time).

The shell (`player/PermissionHost.h`) decides a group three ways: the camera,
the microphone and the location by Windows' privacy settings of the app's own
package, and Windows' own question when the app requests one; the storage, the
media, the text messages, the phone and the call history by Megler's own
question (Allow, Don't Allow), kept in the app's settings, a second no for
good; and the contacts, the calendar, the body sensors, notifications and
physical activity granted with no one asked, since Megler gives an app none of
those of the phone's, and nearby devices likewise, which are Bluetooth LE's
("Bluetooth LE" below), Windows asking nothing for them. One group is no
permission of Android's: Google Location Accuracy (`location_accuracy`),
Megler's question, put once an app that holds the location binds Google Play
services' location API, its answer written into Google's settings for every
app ("Wi-Fi, and Google's network location" below). The location refused by the
system -- the phone's location switched off, which Windows' consent says as
`DeniedBySystem` -- is no refusal of the app's: Android grants or refuses an
app's location permission whether the location is on or off, and the app,
told the permission is its own and the location off (`LocationService`:
`isLocationEnabled` and `isProviderEnabled` false, its listeners' providers
disabled), asks the user to switch it on -- Android's location settings
(`ACTION_LOCATION_SOURCE_SETTINGS`) open Windows' page of them
(`ms-settings:privacy-location`, `AppRunner`), a caller that asked for a
result hearing `RESULT_CANCELED` once it is in front again. Taken for a
refusal for good, it left an app nothing to ask: VTB's request was "denied;
no one asked", and VTB said that the geolocation was not available
(0.1.0.671). What Windows says of
the location (allowed, the user's no for this app, the system's no, not
decided) is said in the log as it is first read and as it changes, by the
permissions and by the location (`player/LocationHost.h`, which follows the
consent while the app runs), and so is why no fix is handed while an app has
requests (`LocationService`: the location off, Windows' no, no location
device, no permission held and so nothing asked, Windows not answering yet,
nothing found in 30 s of following).

The user's own say over every group is the app's **App Permissions**
(`ui/PermissionsScreen.cpp`), as Android 11 and later's App info > Permissions
has them: reached from the app's page in Megler (Permissions), from its App
Settings, and in the app's own package from its Settings (the player's menu),
where it opens in their place and Back goes back to them. Each group the
manifest asks for is in the list of what the app may have -- Allowed, Ask
every time where there is any, Not allowed -- and a tap gives its choices,
kept in the app's settings as `megler.permission.<group>`: **Allow** (Megler's
yes; for a group Windows decides, Windows' own answer, which is said where it
is known -- in the app's own package, Megler's process being another package
-- with the way to its privacy settings), **Ask every time** (`ask`: Megler's
question at the app's first request of the group in each run, its answer that
run's alone, held in the process; for a group Windows decides, Windows' own
question after Megler's yes) and **Don't allow** (`denied-fixed`: the app is
told no and not asked -- over Windows' yes for the location, the camera and the
microphone, and for the groups Megler has nothing behind, which it grants with
no one asked otherwise). An app that has its permissions with its install is
offered Allow and Don't allow, with Android's warning that refusing one may
make it misbehave. What the shell decides of a group from what the user said,
what Windows says and this run's answer (`permissionHost::decide`) is tested
on a PC (`mg_shell_tests`), and the data app is refused its contacts so. What
Megler says is the app's to hear: what an app does without asking (opening the
camera Windows lets it have) is as it was. Below, read only, every other
permission the manifest asks for, as Android names them ("Have full network
access", "Display over other apps (special app access)"), granted with the
install. Clearing an app's data (Clear Data on its page in Megler, or Clear
storage on Android's app info screen in the app) forgets Megler's answers too,
as Android's Clear storage resets an app's permissions: the shell does it as
the clear is asked and as the app next starts with one waiting
(`megler_clear_data_asked`). Clearing its cache (Clear Cache, Clear cache:
`megler_ask_clear_cache`, the app's cache, code_cache and
Android/data/<package>/cache emptied as it next starts) keeps them. Windows'
own answers stay; they are changed in its privacy settings.

**Google Play services' own use of the location.** Their features in the app's
process (location accuracy, personal safety, semantic location: PersonalSafety
and the like in the log) ask for the location as Google Play services itself,
and GmsCore refuses such a request when the attribution tag of the identity it
is made with is null or its location module's own ("internal location usage
has a forbidden attribution tag", `gerz.h`); the identity is made of the
context's tag (`ClientIdentity.c(Context)`), and on a phone such a request in
an app's process would be the app's, another uid. Google Play services'
package context (`MicroG.PackageContext.getAttributionTag`) answers
`megler.app` only where the user has allowed it, and null otherwise, as before:
one answer for every app (`megler.gmsLocation` in Megler's settings, read as the
file has it now: `megler_system_host.permission`'s `gms_location`). The user is
asked once, over the app, the first time such an identity is made with no
answer (the context's caller has `ClientIdentity` on its stack): on a thread
of the framework's (`Permissions.askGmsLocation`), the thread that wanted the
tag told null meanwhile; later calls get the tag once allowed. Megler's
Settings (Google Play Services' Location) and every Java app's App Permissions
change it. The test app `gms-host` checks it with a stand-in of
`ClientIdentity` in its Google Play services: no tag asked for plainly and no
one asked, none at once for an identity and the user asked, then the tag once
allowed -- or none, the user refusing, and nobody asked again.

### The phone's text messages and calls

What comes to the phone while an app runs is the app's as Android gives it,
nothing older (`megler_sms_received`, `megler_call_logged` in `megler.h`; the
framework's `megler.Phone`; the shell's `player/PhoneEvents.cpp`). WhatsApp
confirms a number by a message, which Google Play services' SMS Retriever
reads through `SMS_RECEIVED`, or by a flash call, a missed call whose number
is the code, which its `FlashCallReceiver` hears as `PHONE_STATE` RINGING with
the caller's number. Under Megler neither came (package 482).

- **The shell** watches Windows' chat message store (the `chat` capability),
  for an app whose manifest asks for `RECEIVE_SMS` or `READ_SMS`, or that uses
  Google Play services' SMS Retriever -- a receiver of it in its manifest, or
  its names in its code (a bank's app registers that receiver at run time and
  asks for no SMS permission: the report reads the DEX files of an app that
  carries Google Play services' client, the meta-data
  `com.google.android.gms.version`, for the SMS Retriever's action, its sender
  permission and its class) -- and the call
  history for one that asks for `READ_PHONE_STATE` or `READ_CALL_LOG`: the store of what an
  app does not ask for is never opened, and its own package does not declare
  the capability (docs/PER-APP-PACKAGES.md, "What a package declares").
  A message the store says was made, incoming and after the app's start, is
  told with its sender, text and time. Windows says when a call's state changes
  but not who calls, so when one is over the call history is read
  (`phoneCallHistoryPublic`), a moment later and once more, and the calls that
  began since the app started are told, oldest first. Both are read again when
  Megler resumes, since a call takes the screen and Megler may be suspended
  under it.
- **The core** queues them for the main thread (`EV_PHONE`, `Native.pollPhone`)
  and writes in the log only that there was one, never a number or a text.
- **A message** becomes SMS-DELIVER PDUs (3GPP TS 23.040: the sender as a
  number or GSM 7-bit letters, the time, the text in UCS-2, in parts of 67
  characters with a concatenation header past 70) in `SMS_RECEIVED` with
  "format" 3gpp, to the receivers that hold `RECEIVE_SMS`; an app-specific SMS
  token of `SmsManager` (`createAppSpecificSmsToken`) in its text sends that
  token's PendingIntent, once; and it is in `content://sms/inbox` for an app
  that holds `READ_SMS`. `SmsMessage.createFromPdu` reads such a PDU (the
  overlay's: the stub's was empty), and `Telephony.Sms.Intents.
  getMessagesFromIntent` the intent's.
- **A call** is in `content://call_log/calls` for an app that holds
  `READ_CALL_LOG`; one that came in rings: `PHONE_STATE` RINGING, OFFHOOK when
  it was answered, then IDLE, with the number to the receivers that hold
  `READ_CALL_LOG` and without it to the others that hold `READ_PHONE_STATE`, as
  Android 9 sends it, and `TelephonyManager`'s listeners (`listen`,
  `registerTelephonyCallback` with a `CallStateListener`) hear the same.
  `TelephonyManager.EXTRA_STATE_*` were null in the framework and are Android's
  strings now.
- Permissions are the app's manifest's (by `uses-permission` and by
  `uses-permission-sdk-23`, which Android 6 and later honour: WhatsApp asks for
  `READ_CALL_LOG` so, and was given `PHONE_STATE` without the number until
  `megler.Manifest` read that tag too, package 483), as the user let the app
  have them: all four are dangerous ("Permissions", in Design), and an app of
  Android 6 and later hears nothing until it has requested them and been let
  have them. Google Play services, which the app hosts, holds them as on a
  phone. The store is
  an SQLite database in memory with Android's columns, so an app's projection,
  selection and order work as they would.

The data app's `phoneEvents` registers for all of it (its step `permissions`
has had the four permissions requested and allowed by then) and the test's C side
gives it a message from a number, a longer one from a sender with a name in
Russian that holds the token, and a missed call: the sender, time and text read
back from the PDUs, the inbox row, the token's PendingIntent, RINGING with the
number to the receiver and the listener, IDLE, and the call log's row.

### The phone's accent and theme

The phone's accent colour (Windows' `UISettings` Accent) and a light or dark
theme are told to the core before an app starts (`megler_set_theme`;
`player/PhoneState.cpp`, `tellTheme`). The theme is Windows' own, or what
Megler's Settings choose (Theme: As Windows, Light, Dark). It is also
Megler's own look.

- Android's DeviceDefault and Material themes give the accent as their
  `colorAccent`. So do AppCompat's when the app names no accent of its own
  (`Resources`, overlaid: `accent_device_default_light`/`_dark`,
  `accent_material_light`/`_dark`).
- Material You's system colours (`android.R.color.system_accent1_*` to
  `system_neutral2_*`, `SystemColors`) are made from the accent as Android 12
  makes them from a wallpaper. `megler_palette_tone`
  (`core/src/run/mg_palette.c`) is a port of material-color-utilities' HCT
  (CAM16 hue and chroma, L* tone) and its core palette: accent 1 at the
  seed's hue with a chroma of at least 48, accent 2 at 16, accent 3 at the hue
  plus 60 at 24, neutrals at 4 and 8. A blue accent stays blue. A plain
  CIE LCh palette turned the phone's #0000FF lavender.
- The night bit of `Configuration.uiMode` is the theme. An app's DayNight
  theme follows it.

The test app `data` (`theme-accent`) is told a cobalt accent, #0063B1, and a
light theme. DeviceDefault's `colorAccent` is #0063B1, `system_accent1_600` is
#005FAD within one step, and the configuration is not night. The core's tests
check the palettes against Material 3's baseline colours of #6750A4.

### Sound in the background

A Java app is heard while Megler is minimised, as on Android, where a music
app plays on in the background. A game's engine is silenced when hidden, as
before. Windows allows this to a UWP app only under three conditions:

- the package has the capability `backgroundMediaPlayback`
  (`Package.appxmanifest`);
- the sound is in the Media category (`player/AudioHost.cpp`, `setMedia`);
- play and pause are on in the system media controls and their presses are
  answered (`player/MediaControls.cpp`).

So the controls are on while a Java app runs. They show what the app's media
session says (`megler_media_session_host`: its state, title, artist, album,
art; the overlay of `android.media.session.MediaSession`). An app that has no
session is shown playing while it is heard. The controls' buttons come to the
app as Android's media keys, which go to its active session's
`Callback.onMediaButtonEvent` as Android 9 sends them, and to the window when
there is no session. The test app `data` (`media-session`) checks both ways.

### Installing, and the phone's other apps

What a Java app asks of the phone beyond its window goes to the shell through
`megler_system_host` (`megler.h`; `player/SystemHost.cpp`):

- **Links and sharing.** A link that none of the app's activities takes opens
  in Windows (`Launcher.LaunchUriAsync`). Shared text goes to Windows' share
  sheet.
- **Installing.** An intent to install an APK goes to Megler's installer, as
  Android hands it to its package installer, before AppRunner looks for an
  activity of the app's own (`megler.Installs`). Such an intent is
  `ACTION_INSTALL_PACKAGE`, or `ACTION_VIEW` of an APK (by its type or by a
  name ending .apk, .apks or .xapk) with no activity named; a file manager, a
  browser's download or an app store sends one.
  - A file:// APK is handed over as it is. A content:// one, such as an
    androidx `FileProvider`'s, is first copied into the app's cache.
  - A `PackageInstaller` session is real as far as the app sees it. The files
    written into it are kept in a folder of the app's cache. Committed, it
    hands over its one APK under the app's package name (`<package>.apk`; the
    names Google Play writes, `stream..<package>`, are refused by Megler's
    link for their `..`). Then it waits for the outcome and sends it to the
    `IntentSender` it was given (a `PendingIntent`'s sends through it), as
    Android does once the install is over:
    - `STATUS_SUCCESS` once the phone's apps (`phone-apps\apps.txt`) have the
      app at the APK's version, then `ACTION_PACKAGE_ADDED` (and
      `ACTION_PACKAGE_REPLACED` for an update). Google Play checks that version
      as soon as it hears of success; told at once, while Megler still asked
      the user, it reported a failed install.
    - What Megler wrote of the name in `phone-apps\installs\<name>.txt`
      (`Native.appInstall`): the user's no (`STATUS_FAILURE_ABORTED`), an app
      that cannot run here (`STATUS_FAILURE_INCOMPATIBLE`, with Megler's
      reason), or a failed install (`STATUS_FAILURE`).
    - `STATUS_FAILURE` after 30 minutes with neither.

    The session is then gone, with its files.
  - Uninstalling is handled the same way: `PackageInstaller.uninstall` (Google
    Play's Uninstall), or `ACTION_DELETE` / `ACTION_UNINSTALL_PACKAGE` of
    `package:<name>`, asks Megler (`megler_system_host.uninstall_app`). An
    app's own package opens Megler with `megler:uninstall?package=`. Megler
    asks the user. On a yes, the app's own package is uninstalled and
    Megler's copy of it leaves the library, so the phone's apps no longer
    have it. The app hears `ACTION_PACKAGE_REMOVED`, then `STATUS_SUCCESS`.
    On a no, `installs\<package>.uninstall.txt` says so, and the app hears
    `STATUS_FAILURE_ABORTED`. Before 0.1.0.463 the call did nothing, and
    Google Play showed "Uninstalling..." for ever.
  - Megler asks the user, then installs the APK as one picked in the Library.
    In an app's own package, the APK is copied into the shared folder's
    `incoming\`, and Megler is opened with `megler:install?name=` (its package
    family name as the target). The scheme of these links is the protocol the
    package declares (`appPackage::linkScheme`, read from its manifest; an
    app's package has its Megler's, `megler` for the release's), and Megler's
    family name is the one the app's package was made by (`megler=` in its
    `MeglerApp.ini`; the release's, `Megler_<publisher id>`, for a package
    made before Megler wrote it there): every app's package declares the
    protocol too, and a test
    identity's Play Store (MeglerTest, `meglertest:`) that sent `megler:` to
    `Megler_<publisher id>`, which was not there, had Windows ask which of the
    release Megler and its apps should open it.
  - A session of several APKs (a base and its splits, as Google Play installs
    an app built as a bundle) is handed over as one `.apks` of them, a ZIP of
    the APKs stored as they are, which Megler's installer unpacks as it does a
    bundle from the file manager.
- **Play Store's own updates.** Play Store asks Google's server for its own
  binary, downloads it, and installs it through a session of its own package,
  but only once none of its activities has been resumed for ten seconds while
  its process runs ("Self-update ready to be installed, waiting for market to
  close"; it checks every ten seconds). An app of the phone's own is suspended
  within a second of being left, Back on its first screen included, so such an update never
  goes in, and Play Store starts over at its next start. At the same version
  it compares the variants by their derived APK id
  (`com.android.vending.derived.apk.id`, which Google's bundletool writes into
  each APK it makes of a bundle): Megler brings the standalone APK (id 1), and
  the server named the split APKs for a Lumia 650 (id 5). So every start
  downloaded them again, about 70 MB, and the tries left 291 MB of files
  there (package 662). Play Store's own `ApplicationInfo.metaData` now has its
  derived APK id as the newest there is (`megler.GooglePackages.ownMetaData`),
  and it skips the variant ("Local Derived Apk Id ... > Server Derived Apk
  Id"). With that id the server offers it no binary at all, a newer version
  neither. Tried on a Lumia 650 with package 669: Play Store told it was
  52.9.21 (the flag `older-self` of its own package, which
  `megler.GooglePackages.ownVersionCode` reads) got "Server Version
  [version=-1...]"; with the flag `real-derived-id` as well (its APK's own id,
  1), the same Play Store was offered 53.7.23, whole, with no patch from the
  53.7.23 it has, so the server had read the 52.9.21 of its user agent. So
  Play Store does not update itself under Megler, and no newer version brings
  the downloads back: it gets newer through Megler's package (the Phonesky.apk
  it brings) or an APK added to the library. Installing its own updates would
  need its process kept running in the background for the ten seconds and
  more, and the install handed to Megler from there, where an app of the
  phone's own cannot open Megler; Play Store had 450 MB of private memory
  committed a minute after its start on that 1 GB phone.
  On the Lumia 650 every patch was refused besides ("bad base file hash"):
  Play Store's own APK there had two bits of its own, which Megler mends since
  0.1.0.668 (docs/PER-APP-PACKAGES.md, "The Google APKs Megler brings").
- **Other apps: launchers.** The shell lists the apps of Megler's library that
  can run in the shared folder (`library/PhoneAppsList.cpp`):
  `phone-apps\apps.txt` has each app's package, launcher activity, label and
  version, and `<package>.png` is its icon. Every package of Megler's gives
  that folder to the app it runs (`megler_run_config.apps_dir`), and
  `megler.PhoneApps` reads it. So `PackageManager` knows those apps:
  `queryIntentActivities` of the launcher's intent, `getLaunchIntentForPackage`,
  `getPackageInfo`, `getApplicationInfo`, `getActivityInfo`,
  `getInstalledPackages`. `LauncherApps` lists them, and `loadIcon` gives
  their icons. An activity of theirs that is started goes to the shell
  (`launch_app`). The shell opens that app's own package with
  `megler:run?package=`. An app with no package of its own is opened in
  Megler, which plays it if it has not run an app yet. A launcher therefore
  works best as an app's own package: from Megler, Megler is busy running the
  launcher.
- **Other apps: their activities.** Beside the list Megler describes each
  app's activities from its APK and splits, in `phone-apps\<package>\`
  (`megler_apk_describe_activities`): `activities.json`, every activity and
  alias with its label in the phone's language, its icon, whether it is
  enabled and exported, and its intent filters; and a picture of each icon
  they name, `<id>.png`. `source.txt` there says which APK they are of, so
  that they are made again only for another. `megler.PhoneApps` reads them
  (`Native.appActivities`, `Native.appResIcon`): `getPackageInfo` with
  `GET_ACTIVITIES` and `getActivityInfo` have every activity, the launcher's
  intent every entry (an alias's too), an intent that names the package or
  asks for `ACTION_CREATE_SHORTCUT` the activities whose filters take it, and
  an activity's own icon is its picture. An intent with no package that any
  app could take (a link) is not given them and goes to Windows, as before.
  An alias started is the activity it stands for. An app with no description
  has its launcher activity alone, as before. The phone's other packages'
  install folders cannot be read by an app container on Windows 10 Mobile
  (Megler's own check fails after each install), which is why Megler, which
  has the APKs, writes this for them.
- **Other apps: their signatures, uids, comings and goings.**
  - `activities.json` also has the certificates the APK is signed with
    (`"certificates"`, each DER in hex): the APK Signing Block's v3 signer's,
    else its v2 one's, else those of the v1 PKCS#7 file under `META-INF`.
    `getPackageInfo` gives them as the app's `signatures` with
    `GET_SIGNATURES` and as its `signingInfo` with `GET_SIGNING_CERTIFICATES`,
    and neither otherwise, as on Android. Before 0.1.0.460 another app's
    `PackageInfo` carried the running app's own signatures. Google Play
    compares an app's with its record, so every app it had installed was
    "installed but certificate mismatch" to it: not from Google Play.
  - Each app has a uid of its own, made from its package name (10200 and up),
    and `getNameForUid` and `getPackagesForUid` name it. Before, the first app
    listed had the running app's uid, 10100.
  - The running app's uid, 10100, is the uid of the Google packages it hosts as
    well (GmsCore and Google Services Framework run in its process, and their
    `ApplicationInfo` says so), and `getPackagesForUid` names them with the app,
    as Android names the packages of a shared user id. Google Play services
    checks that a package a caller names is one of the calling uid's: its sign-in
    refused its own backup module a token ("Package not matching uid 10100
    com.google.android.gms"), and WhatsApp's search for its Google Drive backup
    failed until it said it took too long (package 485); Play Store warned of
    the same.
  - Once the app has asked for the list, it is read again every three seconds.
    What changes is told as Android tells it: `ACTION_PACKAGE_ADDED`; for a new
    version `ACTION_PACKAGE_REMOVED` and `ACTION_PACKAGE_ADDED` (both
    replacing) and `ACTION_PACKAGE_REPLACED`; for an app gone
    `ACTION_PACKAGE_REMOVED` with `EXTRA_DATA_REMOVED` and
    `ACTION_PACKAGE_FULLY_REMOVED`. Each has the package as its data and its
    `EXTRA_UID`. Before, Google Play went on showing an app removed in Megler
    as installed.
- **A package the phone does not have.** `getPackageInfo`,
  `getApplicationInfo` and the like are `NameNotFoundException` for it, and
  the enabled state of one of its components or of the application
  (`getComponentEnabledSetting`, `getApplicationEnabledSetting`) is
  `IllegalArgumentException` ("Unknown component: ComponentInfo{...}",
  "Unknown package: ..."), as Android's package manager answers; the log says
  so once a package ("the enabled state of a component of ..., which the phone
  does not have"). The app, the platform (`android`), the Google packages and
  the phone's other apps are the packages it has. Before, every name had
  `COMPONENT_ENABLED_STATE_DEFAULT`. Facebook asks so whether Meta's App
  Manager (`com.facebook.appmanager`, the updater of phones that came with
  Meta's apps) is there: told the default, it took it to be there, found none
  of its receivers, went on to the SIM (a European or Canadian one, or none),
  and opened the terms of such phones ("Meta uses a preinstalled service to
  update apps automatically", the screen `oxygen_preloads_terms_of_service`)
  each time its sign-in began: at every start signed out on the HP Elite x3
  (0.1.0.671), and on an Italian user's phone, which went from the sign-in
  back to that screen again and again. The data app's step `component-state`.
  And `checkSignatures` of such a package (or of a uid with none) is
  `SIGNATURE_UNKNOWN_PACKAGE`, as Android answers; two packages the phone has
  are said to match, their certificates not compared. Every pair matched
  before: Google Play services 26.41 takes a match of
  `com.google.android.pixelsystemservice` with `android` for a Pixel's system
  service on the phone, asks for its `ApplicationInfo` next, and logged
  "E/BusinessScope: Unhandled exception in #1 block ...
  NameNotFoundException: com.google.android.pixelsystemservice" in the apps
  that host it (Transit, 0.1.0.675). The data app's step `check-signatures`.

APKs, .apks and .xapk files also open in Megler from the phone's file
manager (a file type association of Megler's package, taken out of an app's
own package by `core/src/appx/mg_appx_manifest.c`). Megler asks the user,
then installs the file.

The test app `data` checks the core's side. Its apps folder has one other app
(`phone-apps`): the launcher's intent lists it with its label and icon,
`LauncherApps` has it, and starting its launch intent reaches `launch_app`.
It opens an APK by `ACTION_VIEW` (`install-intent`) and commits a session of
one file (`install-session`) and one of a base and a split (`install-splits`).
The harness hears them on `install_apk`, by their apps' names. It says no to
the first in `installs\`, and the app hears `STATUS_FAILURE_ABORTED`. It adds
the second to `apps.txt`; the app hears `ACTION_PACKAGE_ADDED`, then
`STATUS_SUCCESS`. Then the app uninstalls it through `PackageInstaller.uninstall`; the
harness takes it out of `apps.txt`, and the app hears
`ACTION_PACKAGE_REMOVED`, `ACTION_PACKAGE_FULLY_REMOVED` and
`STATUS_SUCCESS`. The other app's uninstall the harness says no to, and the
app hears `STATUS_FAILURE_ABORTED`. The other app's
signatures are the fixture's certificate, given only when asked for, and its
uid names it (`phone-apps-signatures`).

### Jobs, and an app's other processes

`android.app.job` (overlaid): the app's jobs are run, once for the app whichever
instance of `JobScheduler` is asked. A job runs on the main thread once its
minimum latency has passed; a network it asks for is taken to be there, and
`JobParameters.getNetwork` is the phone's. Its service is made as the app's
other services are (`megler.Services.service`), and `onStartJob` is called;
the job ends when that answers false, at `jobFinished`, or when `dequeueWork`
has no more work; then again after its backoff when it asks to be
rescheduled, after its interval when it repeats, at once when work was
enqueued meanwhile. Scheduling it again or cancelling it stops it if it runs
(`onStopJob`). `JobInfo` keeps what its Builder is told (its service,
transient extras, network type, latency, period, backoff, ...), where it kept
the id and extras only. Not done: jobs persisted across runs, constraints
other than the time, content triggers.

`megler.Processes`: a component the manifest puts in another process
(`android:process`) runs in a class scope of the app's own APK
(`megler.ServiceClassLoader`; its DEX files are the bytes already read for
the app's own scope, shared, not read again: `dvm/mg_dvm_apk.c`), with an
Application of its own made from the manifest's class, whose context is the app's but for its class loader, its
Application and its `ActivityManager`, which names that process for this pid.
The code there has statics of its own. Play Store's Application builds an
object graph for each of its processes by the name of the process it runs
in, and its download service (`:download_service`) asked for its own part of
it and had the main process's; the services of such a process are made
there now (`megler.Services`). Their binders reach the rest of the app
through `transact`, as AIDL's `asInterface` takes another scope's object for
a remote one. The process's receivers, providers and activities still run in
the app's own scope.

The scope is over the app's base APK, and its native libraries are the app's
library folder's (`ApplicationInfo.nativeLibraryDir`, where the Java driver
puts the libraries of every APK of the app): `ServiceClassLoader.findLibrary`
names the file there when there is one, else the base's own
(`<apk>!/lib/armeabi-v7a/...`). Play Store from a bundle has its libraries in
its split for 32-bit ARM, which the scope is not over: its background process,
which installs apps, could not load `libmappedcountercacheversionjni.so`, and
every install from Play Store ended in "Can't install the app" (its error 1000,
package 574). The test app `splits` has a service in a process of its own
(`.Other`, `:other`) that loads the library of its split and calls it.

### Downloads

`megler.Downloads` is the system's download provider
(`content://downloads/my_downloads`), into which `DownloadManager` and Play
Store's old downloader (its self-update) insert a row. Each download runs on a
thread of its own over HTTP, with the row's headers, cookies, user agent and
referer, redirects followed, into the file the row's hint names or one of the
app's cache. A query gives the rows' status and bytes, a change is told to the
row's and the provider's URI, and at the end the receiver the row names is told
`DOWNLOAD_COMPLETE` with the download's id; `openFile` gives the file. The rows
are the app's while it runs. A cursor's content observers hear of a change of
its notification URI (`AbstractCursor.setNotificationUri` registers with the
resolver, `ContentObservable.registerObserver` registers at all: both were
stubs that did nothing, and unregistering an observer threw).

### Notification channels

`NotificationChannel` and `NotificationChannelGroup` are Android's (11 to 14):
a channel keeps what the app sets on it and has Android's defaults where it
sets nothing -- the default notification sound
(`Settings.System.DEFAULT_NOTIFICATION_URI`) played as
`Notification.AUDIO_ATTRIBUTES_DEFAULT`, no lights, no vibration, a badge,
`VISIBILITY_NO_OVERRIDE` -- with Android's `copy()`, `equals`, Parcel layout
and elements of the notification policy file (`writeXml`, `populateFromXml`);
`AudioAttributes` are equal by what they hold. The channel was a stub whose
setters kept nothing: Messenger copies a channel of its own through the
getters and asserts that its audio attributes are there, and ended fifteen
seconds after each start on a Lumia 650 (0.1.0.685, "INVOKE_RETURN must not be
null").

`NotificationManager` keeps the app's channels and groups as Android's system
keeps them for an app: one set for the process, whichever `NotificationManager`
is asked; each handed out as a copy (through a Parcel); a channel made again
changes only what Android lets an app change (its name, description, its group
where it had none, whether it may be blocked, its importance downwards), and a
new one has the system's own fields as an app's new channel has them (no Do Not
Disturb, no lock screen visibility of its own, bubbles as the user has them);
a deleted channel is gone and comes back as it was when it is made again
(forgotten after 30 days); deleting a group deletes its channels; a
conversation's channel answers `getNotificationChannel(id, conversationId)`;
a group is handed out with its channels. They are kept across the app's runs
in its data folder, `.megler_notification_channels.xml` (Android's elements,
written on a thread of their own after each change; "notification channels:
N kept from the app's last run" in the log when they are read): Messenger
looks its channels up by id and found them missing at each start ("Channel
messenger_orca_800_live_location was missing, will reinitialize channels").
Clearing the app's data clears them, as on Android. Not there: the
exceptions Android's system throws for a channel it refuses (an empty name, a
group that is not there), the default channel ("miscellaneous") of an app made
for Android 7.1 or earlier; and Google Play services in the app's process
share the app's channels, where on Android they have channels of their own.

The data app checks them: `notification-channel` (the defaults, what is set
kept, Messenger's copy through the getters), `notification-channel-copy`
(`copy()`, `equals`, a Parcel, XML), `notification-channel-group`,
`notification-channels-shared` (a channel made through one
`NotificationManager` found through another), `notification-channels-rules`,
and `notification-channels-kept`: what the first run made, the second finds,
a silent channel's sound and attributes still null, a deleted one deleted.

### WebP pictures

An app's pictures are WebP as often as PNG (Android's build tools convert
them), and the phone's picture decoder, WIC on Windows 10 Mobile, has no WebP
in it. So a WebP file never reaches the shell's `image_size` and
`image_decode`: `megler.Gfx.bmDecode` (`mg_java_gfx.c`) sees "RIFF....WEBP" and
the core decodes it itself (`core/src/webp/mg_webp.c`) into the memory the
shell would have filled, B, G, R, A premultiplied.

The decoder is libwebp 1.6.0's (`build/webp/src`, fetched by
`tools/webp-fetch.sh`; not part of the repository). It has no amalgamation and
its files do not bear being compiled as one, so `core/src/webp/mg_webp_*.c`
are 28 files of four lines, each compiling one of libwebp's: nine of `dec`
(not the incremental decoder), nine of `dsp`, ten of `utils`. The build
scripts define `MG_HAVE_WEBP=1` and add `build/webp` to the include path when
the sources are there, as for SQLite; nothing else is needed, also for the
phone's package.

- **Plain C everywhere.** libwebp switches its NEON or SSE code on by itself
  (for MSVC on ARM without being asked), which would need those files compiled
  too, with options of their own for each compiler, and a question to the
  processor at run time. `mg_webp_config.h`, included first by every wrapper,
  reads libwebp's `src/dsp/cpu.h` and takes its `WEBP_USE_*` and `WEBP_HAVE_*`
  switches back, and sets `WEBP_NEON_OMIT_C_CODE` to 0 so that the C code NEON
  would have replaced is compiled. No threads (`WEBP_USE_THREAD` is not
  defined).
- **What is decoded:** lossy (VP8), lossless (VP8L), either with alpha; and
  the first frame of an animated file. libwebp's decoder does not read
  animations (that is its demux library, which is not here), but a frame is an
  ordinary picture's chunks inside an `ANMF` chunk: they are copied out behind
  a header of their own and decoded as a still, onto a transparent canvas when
  the frame does not fill it.
- **Reduced size:** `BitmapFactory`'s `inSampleSize` arrives as the size
  wanted; libwebp scales while it decodes (`use_scaling`).
- In a build without the decoder a WebP file goes to the shell like any other
  picture and the log says once why it will not decode.

Tested through `bmDecode` on both hosts with five files of 32 x 32 kept in the
repository as bytes (`core/tests/mg_webp_fixtures.inc`, written by
`tools/webp-fixtures.py` with ffmpeg's encoder), 40 checks: lossless with
alpha exactly, also at half and a quarter of the size; lossy within 12 of each
colour; lossy with an alpha plane; an animation's first frame; a first frame
of 16 x 16 at (8, 8) on a 32 x 32 canvas, whole and at half size; and three
files that only begin like WebP. The MSVC ARM build of the 29 files is clean
at `/W3`. *Not measured:* how long a large lossy picture takes without NEON on
the phone.

### A decode into a bitmap of the app's (inBitmap), and reconfigure

`BitmapFactory` decodes into the bitmap an app gives it in `Options.inBitmap`
and hands that same bitmap back, as Android's does: the decoded picture takes
the place of the bitmap's own (`Bitmap.meglerTakeOver`, its native picture
replaced; a display list that drew the old one keeps its reference), with the
picture's size and config, while what the bitmap was made with
(`getAllocationByteCount`) stays. A picture that needs more than that, or
that cannot be decoded, is `IllegalArgumentException("Problem decoding into
existing bitmap")`; a recycled bitmap is refused; an immutable one is passed
over, said once, for a new bitmap. `Bitmap.reconfigure` gives a mutable bitmap
another size and config within what it was made with (a new, empty picture of
that size: Android leaves the old bytes, of no use in the new shape), and
`setWidth`, `setHeight` and `setConfig` go through it. A JPEG asked for as
`RGB_565` is `RGB_565` (two bytes a pixel in what an app reads of it), as
Android's decoder gives it; everything else decodes to `ARGB_8888`.

Both were missing (inBitmap ignored, reconfigure doing nothing), and every
still picture of Fresco's failed: its decoder (`DefaultDecoder`) decodes into a
bitmap of its pool and throws an `IllegalStateException` when the bitmap that
comes back is another one -- Facebook's log on 0.1.0.671 had
`onProducerFinishWithFailure ... stage: DecodeProducer ... IllegalStateException`
for each picture, and Aliucord (Discord's old app, Fresco 2) showed no avatars.
Fresco's pools make their bitmaps 1 x N in `RGB_565` and reconfigure them to a
picture's or an animation frame's size (`ArtBitmapFactory`), so its animated
pictures were drawn into a buffer one pixel wide. Glide reconfigures what its
pool gives too, and retries without inBitmap on the `IllegalArgumentException`.
The data app's step `bitmap-reuse` decodes a PNG into a pool's bitmap made as
Fresco makes one, reconfigures it within and past what it was made with, and
gives a bitmap too small and an immutable one.

### Databases: `megler.Sqlite` and the SQLite in the core

The framework's `android.database.sqlite` is Java over 29 native methods of
`megler.Sqlite` that are SQLite's C API almost one for one: a database is its
`sqlite3 *`, a statement its `sqlite3_stmt *`, held by Java as longs.
`core/src/drivers/mg_java_sqlite.c` is those methods, as the framework's
code, written for iOS and the SQLite that iOS carries, expects them: `sqlite3_open_v2`
with Android's two open flags, `sqlite3_extended_result_codes`, a busy timeout
of 2.5 s, the collations `LOCALIZED` (ASCII case ignored) and `UNICODE` (byte
order) that Android registers on every connection, strings bound and read as
the UTF-16 Java keeps them in. An app's path
(`/data/data/<package>/databases/x.db`) is turned into the host's.

**Which SQLite.** These natives, as written for the framework on iOS, take
whatever SQLite the system has, and use 35 functions of its API, none of them new: `open_v2`,
`close`, `close_v2`, `errmsg`, `errstr` (the newest: SQLite 3.7.15, of 2012),
`extended_errcode`, `extended_result_codes`, `exec`, `changes`,
`last_insert_rowid`, `get_autocommit`, `busy_timeout`, `create_collation`,
`prepare_v2`, `finalize`, `reset`, `clear_bindings`, `bind_parameter_count`,
`stmt_readonly`, five `bind_*`, nine `column_*`, `step`, `libversion`. Any
SQLite of the last ten years has them, so Windows' own `winsqlite3.dll`
would do as far as the API goes (*its exports were not looked at*). It is
still not used: whether an app container on Windows 10 Mobile may load it was
never verified, its version is whatever the phone's Windows has, and it would
open files by itself, past the host layer. The amalgamation is compiled in
instead, by `core/src/sqlite/mg_sqlite3.c`, which is nothing but its build
options and `#include "sqlite3.c"`:

- `SQLITE_OS_OTHER=1`: no operating system layer of SQLite's own, neither
  Unix's nor Windows' -- so no `windows.h`, no `LoadLibrary`, nothing an app
  container forbids. What SQLite then asks for is in
  `core/src/sqlite/mg_sqlite.c`: **a VFS over the host layer's files**
  (`mg_file`: positioned reads and writes, truncate, sync, size, delete,
  exists), the same code on the phone, on the PC and under qemu; **its locks
  in the process and on the file**, SQLite's own protocol (SHARED, RESERVED,
  PENDING, EXCLUSIVE) kept in a table by path for the process's connections,
  and taken by each connection as byte-range locks where SQLite's own layers
  take them (PENDING's byte at 1 GB, RESERVED's after it, 510 bytes for the
  readers), through `mg_file_lock`: Windows' `LockFileEx` on the phone and the
  PC, Linux's open-file locks (`F_OFD_SETLK`) under qemu, each handle's own
  and binding every other. Other processes are bound by them: every app here
  runs Google Play services, whose databases are every app's
  (`/data/megler-services`), open in several apps at once, and an app
  suspended in the middle of a transaction keeps its locks, so that another
  waits (Android's 2.5 s of `busy_timeout`, then "database is locked")
  instead of taking its journal for one left behind and rolling it back, or
  writing beside it. With the table alone it did, and Phenotype's database had
  no table Packages in several apps' logs (576). As Windows suspends the app,
  the shell waits up to a second for its connections to let their locks go
  (`megler_settle_databases`), since Windows freezes it as it is and a lock
  it kept would hold the other apps up until it came back; the log says when
  one was kept all the same. The core tests lock the file
  through a handle of their own as another process would (its writer, its
  writer waiting, its reader), and on the PC start a second process
  (`mg_tests --sqlite-hold`) that holds a write with its journal while the
  first reads beside it and cannot write; **its mutexes on
  the host layer's locks** (`SQLITE_THREADSAFE=1`, serialized, named to SQLite
  with `SQLITE_CONFIG_MUTEX` before its first use); the clock and random
  numbers from the host layer.
- `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_TEMP_STORE=3` (temporary tables in
  memory), and what Android's own build has where a schema or a query would
  notice: `SQLITE_ENABLE_FTS3`, `_FTS3_PARENTHESIS`, `_FTS4`, `_FTS5`,
  `SQLITE_DEFAULT_AUTOVACUUM=1`.
- No write-ahead log. It needs memory shared between connections
  (`xShmMap`), which the VFS does not give; a database asked for WAL
  (`enableWriteAheadLogging`) keeps its rollback journal, which is what SQLite
  does on any file system without shared memory.
- Not `SQLITE_DEFAULT_MEMSTATUS=0`, common as it is: with it, this version's
  full-text search corrupted the heap on 32-bit ARM the second time a
  connection used it (glibc: "double free or corruption"; x64 with the address
  sanitizer was clean). Found by building SQLite with one option at a time.

The build scripts define `MG_HAVE_SQLITE=1` and add `build/sqlite` to the
include path when `build/sqlite/sqlite3.c` is there; without that everything
compiles to nothing, `megler.Sqlite.open` answers 0 with
`SQLiteCantOpenDatabaseException` in the app, and the log says once, in a
sentence, that this build has no SQLite and what would give it one. The same
two flags are all `tools/build-appx.sh` needs; the three files compile with
MSVC for ARM at `/W3` without a warning (SQLite's own are silenced inside
`mg_sqlite3.c`), an object of 3.4 MB before linking.

### The WebView

`android.webkit.WebView` is Android's own: AOSP's `android.webkit` of
android-15.0.0_r36 in the overlay, and behind it Android's System WebView,
Chromium, loaded from the Android root into the app as a phone loads it. The
framework's own WebView, written for iOS, sat over a WKWebView (`megler.Web`),
whose natives answer nothing on Windows: a WebView was a blank rectangle.

**The default User-Agent without Chromium.** `WebSettings.getDefaultUserAgent`
asked the WebView's provider, which is all of Chromium loaded: its library and
heaps, some 200 MB of a 32-bit process's 2 GB of addresses. Apps and their SDKs
ask for the string at their start, for an ad's or a statistics' request, and
Yandex Maps ran out of addresses as the WebView was loading (package 490).
Until the WebView is loaded the string is made as Chromium makes a WebView's,
of the same `Build` fields and the WebView package's version
(`WebViewFactory.meglerDefaultUserAgent`); once it is loaded it is asked. The
WebView test app asks for it before it makes a WebView, and the test checks that
it came before Chromium was loaded and that the WebView's own string is the
same. And the log's "Loading com.android.webview ..." says, after "for", which
of the app's methods wanted the WebView.

**Which WebView.** Two could be had from Google's own servers:

- The Android 9 image's own, `/system/app/webview/webview.apk`: Chromium
  66.0.3359.158, of 2018, with a library for 64-bit ARM alone (the image is
  arm64's), and its bytecode taken out of the APK into the image's quickened
  `.vdex`. Nothing of it runs on a 32-bit phone, and its JavaScript is older
  than what login pages are written in now.
- AOSP's prebuilt, `platform/external/chromium-webview`, whose newest is
  **128.0.6613.88** (August 2024; it is what android-15.0.0_r36, Android 15's
  release of March 2025, builds into its image). It has `armeabi-v7a`; its
  `minSdkVersion` is 26, so what it uses of Java's library is Android 8's,
  which Android 9's libcore has; and its glue still asks
  `WebViewDelegate.isMultiProcessEnabled()` whether to sandbox its renderer.
  This one. `tools/webview-fetch.sh` fetches it (86,760,206 bytes, its SHA-1
  checked) and the `android.webkit` sources it was taken with;
  `tools/dvm-android-root.sh` puts it where Android's image has it:
  `app/webview/webview.apk` in the root, `/system/app/webview/webview.apk` to
  the app. 85 MB more in the root and in the phone's package.

**How it is loaded** (the overlay's `WebViewFactory`,
`WebViewLibraryLoader`, `WebViewDelegate`; their tops say what differs from
AOSP's). There is no update service to name the package and no package
manager that has it installed: the package is the root's APK, and its
`PackageInfo` is read from its manifest (name, version, target SDK, the
meta-data `com.android.webview.WebViewLibrary`). Its resources and files are
added to the app's `AssetManager` as a shared library (below). Its class
loader is a `PathClassLoader` over the APK with the APK's `lib/armeabi-v7a`
as its native path, as Android's is for a package whose libraries are not
extracted. `WebViewLibraryLoader` asks for 130 MB of address space to be
reserved, as the zygote does for a 32-bit process so that every app can load
the library at one address with one RELRO; here there is one process and
nothing to share, so its two natives (`core/src/drivers/mg_java_webview.c`)
reserve nothing and load `libwebviewchromium.so` wherever its own size fits,
reading it out of the APK: 72 MB of address space with the code the patcher
adds, its constructors run (the test checks the size). It went into the 130
MB at first, as `android_dlopen_ext` with
`ANDROID_DLEXT_RESERVED_ADDRESS_HINT` does, and the other 58 MB stayed
reserved for nothing. Its `JNI_OnLoad` runs when Chromium's own `LibraryLoader`
calls `System.loadLibrary("webviewchromium")`, as on Android: the runtime now
runs a library's `JNI_OnLoad` the first time Java loads it even when native
code or the framework loaded it before (`dvm/mg_dvm_jvm.c`). Then
`com.android.webview.chromium.WebViewChromiumFactoryProviderForT.create` is
given a `WebViewDelegate`, and every `WebView` is that provider's
`WebViewProvider`, as on Android.

**One process.** Windows 10 Mobile lets an app start no process.
`WebViewDelegate.isMultiProcessEnabled()` answers false, so the glue adds no
`--webview-sandboxed-renderer`, and Chromium's `AwMainDelegate` adds
`--single-process`: the renderer is threads of the app's process, as
WebView's was before Android 8. Its log says `multiprocess=false`. Its GPU
work is in the process in any case.

**One class path.** This runtime has one name space for classes, not one per
class loader (`dvm/mg_dvm_art.c`, `DexFile`). The WebView's 3478 classes join
it after the app's, and say they come from the app's loader; so
`WebViewFactory` gives the app's loader the WebView's native path too
(`BaseDexClassLoader.addNativePath`, which Android 9's libcore has), and
Chromium's `System.loadLibrary` finds the library. Most of the WebView's
classes are renamed by R8 (`WV.*`), but some keep their names: `org.chromium.*`
for JNI, `androidx.*`, `J.N`. An app that bundles classes of those names --
Cronet's `org.chromium.net`, a Chromium-based app's own, other versions of
androidx -- would have its own taken by the WebView's code, and the WebView's
by its own where the app's come first. No app tried has done that yet.

**Drawing on the graphics processor.** Where the shell keeps pictures on the
graphics processor (`megler_gfx_host.texture_frame` and `draw_texture`, and
EGL shares pbuffers' textures) and the flag `no-webview-gpu` is not set (the
flag `trace-webview` says what each of a functor's first draws left in its
pbuffer and whether Chromium had a frame), a
WebView's canvas in a view list says it is hardware-accelerated
(`View.drawFromParent`, `megler.WebFunctors.on`), and Chromium draws its page
as on Android: through the platform's draw functor, AwDrawFn
(`android_webview/public/browser/draw_fn.h`, version 4), whose table
`DrawFunctor.nativeGetFunctionTable` hands it (`mg_java_webview.c`). Chromium
makes a functor for each WebView (`create_functor_v3`) and, in `onDraw`, asks
the platform to draw it (`WebViewDelegate.drawWebViewFunctor`). The overlay's
`WebView.meglerDrawFunctor` has it drawn at once on the WebView's render
thread, the UI thread waiting as HWUI's does for a frame it cannot let go of:
`on_sync`, then `draw_gl`, with an OpenGL ES 3 context current (2 on a display
of feature level 9_3) on a pbuffer of the part of the view the window shows
(rounded up to 64 pixels, kept while the part changes a little; of a
configuration whose texture the display shares, `wv_pick`), of the WebView's
own display (the one Chromium's own `eglGetDisplay` is given: `ndk/mg_egl.c`,
`webview_display`). The pbuffer is
cleared and set up as HWUI leaves its surface for a functor: the framebuffer
0, the viewport the surface, the scissor the part, no stencil test (with one
Chromium draws into a texture of its own and that through the stencil). The
parameters are HWUI's: the clip the part from the surface's top left, the
transform a translation by the part's corner, sRGB. Chromium draws with
whatever context is current (its `GLNonOwnedContext` never makes one current)
into the framebuffer bound, the right way up for OpenGL; it restores none of
the state it drew with. Its first draw sets up its GL first, which leaves a
context of its own current: the frame is then drawn again in the render
thread's. The pbuffer's texture is then the shell's picture of the functor
(`mg_java_gl_share_frame`, its keyed mutex let go for the copy), which the
view's list draws where the part is (`Canvas.meglerDrawTexture`, a
`DL_TEXTURE`, turned as a TextureView's frame is), nothing read back. A
released functor (`release_functor`) has `on_context_destroyed` (with the
context current) and `on_destroyed` on the render thread; its pbuffer and
picture go with it. The log says "webview: pages are drawn on the graphics
processor" and "draw functor N drawn" with the time a draw takes. A draw that
fails three times in a row, or one that takes more than four seconds (fifteen
on a display of OpenGL ES 2, below), has the
pages drawn in software from then on, as below. `mg_gl_tests.c`'s draw functor
test checks the table, the threads, the order of the callbacks, the
parameters, the picture handed over and the release with the tests' stand-in.

**Drawing in software.** A canvas that is not hardware-accelerated -- a
`Bitmap`'s, the window's with `no-view-lists`, or any canvas where the draw
functor is not there -- has the WebView draw in software, as it does on Android
into a `Bitmap`'s canvas: `AwContents.onDraw` with `isHardwareAccelerated()`
false.

Only there, as on Android. A WebView put straight into the root view of a
window of the app's own -- King's popups in Candy Crush Saga: a panel window
whose `PopupDialog$PopupDecorView` holds the WebView -- drew on the window's
frame, which said it was not hardware-accelerated, so its pages were drawn in
software while every other WebView's were on the graphics processor. Chromium's
software draw cannot take a page's view transition (`startViewTransition`, a
CSS `@view-transition`): it composes on a frame sink manager of its own
(`SynchronousLayerTreeFrameSink`, Blink's
`widget/compositing/android_webview`), made with no shared image interface
provider, and the transition's save directive asks for one
(`CompositorFrameSinkSupport::ProcessCompositorFrameTransitionDirective`,
`BlitRequestsForViewTransition` on by default, then
`FrameSinkManagerImpl::GetSharedImageInterface`, which only DCHECKs its
provider: libwebviewchromium.so+0x3879a04 reads the provider's vtable through
null, called from +0x3874010). The popup's page made one, and the app ended
(package 673). A window's frame (`megler.ViewRoot.ScreenCanvas`) now says it is
hardware-accelerated while the views draw through lists of their own, as a
window's canvas does on Android, and not while a view without a list that is
not drawn on the graphics processor draws into it, as a view list's does: the
data app's `panel-root` checks both. A WebView drawn in software on Android
(into a `Bitmap`) would end the same way on such a page. The overlay's `WebView.onDraw` draws every frame through one bitmap of
what the window shows of the view, as the functor draws on Android, and puts
that bitmap in the canvas it was given: a WebView sized to its page is many
screens tall (a check: a WebView 20000 rows tall is drawn through 744), and a
view out of the window gets at most a screen's worth from its scroll position.
Two such bitmaps are kept and drawn through in turn, each cleared in its
memory, and let go while the pages are drawn on the graphics processor. A
frame's list draws the bitmap itself, as its pixels are when the render thread
draws the frame, while the main thread records the next one, which erases the
page and draws it again; through one bitmap a frame that drew it then showed
it half written, and now the bitmap the last frame was drawn through is left
alone until the frame after. The render thread's line in the log counts the
bitmaps a frame drew whose pixels had been written after the frame was
recorded (and says the first at once): on this PC a page whose animation had
Chromium draw again every frame, 52 frames a second with the main thread
waiting 4.6 s of every 5 for the render thread, counted one in 9865 frames
(the WebView's 720x1168 bitmap) through one bitmap, and none in 9749
through two.

Chromium draws into that bitmap directly. The platform's
`libwebviewchromium_plat_support.so` gives a WebView tables of functions;
here the software one (`GraphicsUtils.nativeGetDrawSWFunctionTable`,
`mg_java_webview.c`) is there and the GL functor's are null (the library is a
system library's name to the linker, `bionic/mg_bionic.c`, and a file that
says so in the root's `lib`). The table is Chromium's
`AwDrawSWFunctionTable` (`android_webview/public/browser/draw_sw.h`): its
version (1) first, then `access_pixels` and `release_pixels`. Without the
version Chromium called `release_pixels` for `access_pixels` and had it free
the `JNIEnv`, which corrupted the heap of Play Store's process at the first
frame of Google's sign-in page (0xc0000374, no report of the core's: Windows
ended the process). Its `access_pixels` hands Chromium the pixels
under a canvas of a core `Bitmap` as Skia's `SkCanvasState` (version 1, a
stable ABI of Skia's), in an `AwPixelInfo` of version 3: one raster layer,
the bitmap locked in Android's order (R, G, B, A), the canvas's matrix and its
clip's box (`mg_java_gfx_canvas_raster`); `release_pixels` unlocks it. The log
says "webview: Chromium draws straight into a canvas's pixels" the first
time. Chromium asks for a canvas's pixels only where its GPU can share
textures across threads (`can_support_threaded_texture_mailbox`: EGL's
`EGL_KHR_fence_sync`, which the layer gives, `EGL_KHR_image_base`,
`EGL_KHR_gl_texture_2D_image` and `GL_OES_EGL_image`, which ANGLE over
Direct3D 11 has, and the tests' stand-in names too); where they are missing
`AwContents` forces its own bitmap, as below. The data's side of it is checked
in `mg_gfx_natives_tests.c` (a canvas's pixels lent out), Chromium's in
`mg_java_tests.c`'s `webview`. Before, with no table, Chromium drew each frame into a new `Bitmap` of
the WebView's whole size of its own (its `JavaBrowserViewRendererHelper`'s
"auxiliary bitmap", 12 to 15 MB at 1440 pixels wide, ten a second), which only
the collector took back: Google's sign-in page in Play Store filled the
process's two gigabytes with them, Bitmaps could no longer be made ("Error
allocating bitmap"), frames went undrawn and the app ended. A canvas whose
pixels are not the core's (a recording one) still gets that, as before.

All the same, Chromium makes its GPU service at the first WebView and ends the
process if it cannot (`GpuServiceWebView`, "gpu::InitializeGLThreadSafe()
failed"): its ANGLE runs its OpenGL ES back end over the system's `libEGL.so`
and `libGLESv2.so`, which are the layer's EGL here -- on a phone ANGLE's
Direct3D 11, under qemu the tests' stand-in, which for this has a complete
mode (`core/tests/mg_gl_standin.c`: every function of EGL 1.4 and OpenGL ES
2.0 there, those it does not draw with doing nothing, an OpenGL ES 2.0
device's limits). Chromium's raster of tiles goes there too; under qemu that
draws nothing (its log says "Error: 1 for Command Noop"), and the page is
drawn in software regardless.

**At Direct3D feature level 9_3 too, pages are drawn on the graphics
processor.** They were drawn in software there: ANGLE as gecko-w10m builds it
gives a surface's texture no shared handle on a device of feature level 9_3
or below (`Renderer11::getShareHandleSupport`: such a device "doesn't support
share handles on RGBA8 textures/swapchains"), so its EGL did not name
`EGL_ANGLE_surface_d3d_texture_2d_share_handle`, no frame of OpenGL ES could
be a picture the shell keeps (`mg_java_gl_shares_frames`), and Chromium was
given no draw functor -- a Lumia 650's logs said "webview: pages are drawn in
software: no draw functor" in every app -- while its GPU service rastered its
layers' tiles for draws on the graphics processor that never came (its "tile
memory limits exceeded", which this PC's software runs at 9_3 say too).
Direct3D 11 does share textures at 9_3, in one format: on this PC a device of
9_3 makes a `B8G8R8A8_UNORM` texture shared, plainly and with a keyed mutex,
and a second device of 9_3 or 11_1 opens it by its handle and reads what the
first drew; `R8G8B8A8_UNORM` and the typeless formats it refuses
(`E_INVALIDARG`). Megler's ANGLE shares such a pbuffer's texture at 9_3
(`angle/0005-share-handles-feature-level-9.patch`); the render thread's
pbuffers are of the first configuration whose pbuffer has a shared handle
(`wv_pick`, EGL putting B8G8R8A8 first; where none has one, pages are drawn in
software as before); and the shell takes each picture as on an HP Elite x3: by
its handle, under its keyed mutex, into a copy of its own drawn where the view
is, nothing read back. The win suite holds both sides to it: the shell's
drawing takes a 9_3 device's shared picture (`mg_gfx_windows.cpp`,
`check_texture_9_3`), and this PC's own ANGLE at 9_3 shares a pbuffer and gives
back through its handle what was drawn into it (`mg_angle_windows.c`,
`gl1_shared`). Chromium's contexts there are of OpenGL ES 2.0.

Chromium's own ANGLE (its OpenGL ES back end over this layer's EGL) is not told
of instanced arrays on a display of OpenGL ES 2 at most (`ndk/mg_egl.c`,
`w_get_string`, for the WebView's code alone, told by the library the call
comes from). It takes `GL_EXT_instanced_arrays`, which ANGLE over Direct3D 11
names at 9_3, for instanced draws, but loads the functions that draw so only
with OpenGL ES 3 or `GL_EXT_draw_instanced` (`DispatchTableGL`), neither of
which such a display has: Skia's first instanced draw went through a null
pointer (this PC at 9_3, the WebView test app's first page). And what Skia
draws with instances does not fit 9_3: its tessellating path renderers' vertex
shaders take some 466 instruction slots where `vs_4_0_level_9_3` has 256, which
D3DCompile takes seconds on a Lumia 650's processor to refuse (venetium-w10m,
the owner's Chromium for these phones, turns them off there). Without them
Skia draws paths and round rectangles as on any OpenGL ES 2 phone. Megler's
ANGLE also has two of venetium-w10m's fixes for 9_3: no dual-source blending
below feature level 10_0, whose blend states Direct3D refuses there
(`angle/0006`), and an EGL image's texture sampled at 9_3 (`angle/0007`):
without the second, the WebView test app's page drawn through a `RenderNode`
lost its yellow bar on this PC at 9_3, ANGLE refusing each draw from one of
Chromium's images. venetium-w10m's third, that a shader over the target's
limits is not compiled again with ANGLE's other configurations, is not taken:
here the eight lights of OpenGL ES 1 (`mg_angle_windows.c`) compile at 9_3
only with a later configuration than the first, and without it they were
refused and ANGLE ended the test at the next draw.

On this PC at 9_3 (the desktop build's test program on the x86_64 WebView with
Megler's ANGLE for this PC instead of the tests' stand-in, the shell's pictures
taken through their handles: `MG_JAVA_ANGLE=build/angle/x64/dist mg_tests.exe
--java webview ... root-x86_64 ...` with the flag `egl-level-9_3` in the data
folder's flags) the WebView test app passes every step through the draw
functor -- the page's bar and body their exact colours, the layers' page whole
-- as it does at this PC's own level. A page of text (9 to 30 pixels, bold,
italic, Cyrillic, shadowed, turned) and paths (SVG curves, dashes, joins,
even-odd, arcs, a 60-pointed star, canvas arcs and curves) comes out as
Chromium's software drawing of it but for the edges' anti-aliasing: 96% of its
pixels the same, 0.02% different by more than 48 of 255. WebGL is not offered
at 9_3 (Chromium's command buffer wants 8 varying vectors and ANGLE has 7
there; told 8, Chromium still made no WebGL context): a page's
`getContext("webgl")` gives none, as before.

What it costs a Lumia 650 (1 GB, 390 MB for an app): a functor's pbuffer of
the part shown, rounded up to 64 pixels (768x1216 for 720x1168: 3.7 MB), and
the shell's copy of it, as much; the two bitmaps of software frames (3.4 MB
each at that size) are let go, and Chromium made its tiles before too. A
program of Skia's is compiled by D3DCompile on the phone's processor the first
time it is needed, and a page's first frames wait for theirs; ANGLE keeps them
for the next start (`EGL_ANDROID_blob_cache`, `angle-cache` in the app's
data). So on a display of OpenGL ES 2 the UI thread waits up to 15 s for a
functor's draw, not 4, before it gives pages up to software
(`WV_WAIT_ES2_MS`); the log says each slow frame's time ("webview: slow
frame").

**The WebView's developer-mode flags, and HitTestOpaqueness.** The WebView in
the Android root is AOSP's, not Google's, and Chromium that is not
Chrome-branded applies its field trial testing config wherever no variations
seed says otherwise (`variations_field_trial_creator_base.cc`,
`ShouldUseFieldTrialTestingConfig`): with the flag `webview-verbose-logging`
its log lists 123 active field trials of that config, among them
"HitTestOpaqueness" in group "Enabled". That feature bounds a layer's
recording by what the layer paints rather than by the layer
(`RecordingSource`, `can_use_recorded_bounds`), and Chromium's software draw
does not take it into account: `PictureLayerImpl`'s resourceless draw gives
the quad the visible rectangle cut down to those bounds and the texture
coordinates of the whole visible rectangle, and
`SoftwareRenderer::DrawPictureQuad` maps the one onto the other. So a layer of
its own that paints only part of itself was drawn squeezed into that part,
about the part's top left. On a Lumia 650 Google's sign-in page in Play Store
(Google Play services' sign-in) showed its password field's floating label at
0.4 of its size, the password's bullets at a third and the "Show password"
checkbox at a third; drawn on the graphics processor, as on an HP Elite x3,
the bounds only say which tiles have something in them, and nothing is
squeezed. Material's checkbox is such a layer (`will-change: transform, opacity`):
a 48-pixel input that paints nothing round an 18-pixel box, whose box came out
13 pixels wide at 2.00, 11 pixels in from its own corner -- 18/48 of it, as
the arithmetic of those two rectangles has it; Google's "Create account"
button likewise. Megler now answers for the WebView DevTools app, through
which a phone's user sets the WebView's flags (`megler.WebViewFlags`): the
WebView asks, as it starts and before it loads its library
(`WebViewChromiumFactoryProvider.initialize`, `DeveloperModeUtils`), whether
the component `org.chromium.android_webview.devui.DeveloperModeState` of its
package is enabled (`PackageManager.getComponentEnabledSetting`) and then for
the rows of `content://<its package>.DeveloperModeContentProvider/flag-overrides`,
and puts them on its command line (`FlagOverrideHelper`); the platform says
the component is enabled and its provider answers HitTestOpaqueness off. The
log says "the WebView's developer-mode flags: {HitTestOpaqueness=false}", and
the WebView's own with `webview-verbose-logging` "Field trial config study
skipped: HitTestOpaqueness.Enabled" and a `disable-features` with
HitTestOpaqueness in it. Only flags of the WebView's own list are taken
(`ProductionSupportedFlagList`; a name it does not have ends its start), so
Megler's are named one by one and set only for the WebView they were checked
against, 128 (another version is said in the log and gets none):
`webview-layer-borders` (Chromium's
`show-composited-layer-borders`, a border round each of its layers),
`webview-verbose-logging` (its command line and field trials in the log) and
`webview-hit-test-opaqueness` (HitTestOpaqueness left as the testing config
has it). The WebView test app checks it: a page of layers that paint part of
themselves -- a checkbox as Material's, a box in an empty layer -- and one that
paints all of itself, each box found whole in the picture. With
HitTestOpaqueness on, 36 of the checkbox's 196 inner pixels and 792 of the
other box's 3136 were theirs, and the layer that paints all of itself was
whole (the desktop build's test program running the app natively on the
x86_64 WebView, the same release, which takes seconds where qemu takes
minutes: `mg_tests.exe --java webview ... root-x86_64 ...`).

**Resources.** The WebView APK's `resources.arsc` is a shared library's:
package id 0, its own references through that id. `AssetManager.
addAssetPathAsSharedLibrary` (the overlay's) has the core open the APK as one
more of the APKs it reads (`megler.Native.addApk`, numbers from 12 on), and
`Resources` reads its table with the package given the next free id from
0x02, as Android's `AssetManager2` gives a dynamic package; ids that name
package 0 in that table are given 0x02 as they are read (`ResTable`).
`getAssignedPackageIdentifiers` says so, and the WebView renumbers its R
class to it (its log: `packageId=2`). Its assets are found through the app's
`AssetManager` too: Chromium maps `icudtl.dat`, its `.pak` files and V8's
snapshot out of the APK with `openNonAssetFd`.

**An APK's compiled XML.** `AssetManager.openXmlResourceParser(cookie, name)`
reads a compiled XML file of the cookie's APK (1 the app's, 2 on a library's,
0 the first that has it) into an `XmlBlock` parser; it answered null.
Google Play services 26 makes an `AssetManager` of its own for each of its
modules' APKs, adds the APK (`addAssetPath`, which here adds it as a library)
and reads the module's `AndroidManifest.xml` so; its Application ended in a
NullPointerException before anything ran. The data app's step
`asset-manager-manifest` does the same with its own APK. (The core reads an
APK's `resources.arsc` for its label and icon up to 256 MB, `ARSC_MAX` in
`apk/mg_apk.c`: Google Play services 26's is 88 MB, and at 64 MB it had its
package's name and no icon.)

**Input.** A touch reaches the page through `View.onTouchEvent` like any
view's. A tap on a field of the page focuses it, and Chromium's `ImeAdapter`
asks for the keyboard through `InputMethodManager`, which is
`megler.InputMethods` here and the shell's keyboard (`megler_keyboard_host`);
what is typed goes to the `InputConnection` the WebView makes, Chromium's
`ThreadedInputConnection`, unchanged.

**The network** is Chromium's own: its network service in the process, its
cookie store, its BoringSSL, over the layer's sockets (`bionic/mg_net.c`).
Nothing of the platform's TLS is used.

**What the platform had to give it**, each found by running it and each its
own commit with its test: `UserHandle.getAppId` (the WebView refused a process
whose app id read as root's), `NetworkSecurityPolicy` (every http:// request
refused), `CompatChanges` (a class the glue names), the package manager
knowing the WebView's own package (`megler.GooglePackages`, overlaid: Chromium's
crash handling asked for it), `etc/fonts.xml` in the root (Skia had no font at
all), `mmap` following an address it is given (PartitionAlloc's out-of-memory
crash), shared memory whose mappings are the same memory (every page arrived
empty: Mojo's data pipes), and surface control in `libandroid`
(`gfx::SurfaceControl::IsSupported` CHECKs that every function is there on
Android 10 and later); docs/ARCHITECTURE.md has the core's. On the phone,
VK's "not a robot" page found more, each a call through nothing or an
exception that ends the process on purpose: the EGL version (ANGLE over
Direct3D 11 says 1.5 and has no fence syncs; the ANGLE inside Chromium takes
1.5 to mean them and called an `eglCreateSyncKHR` it had not loaded, so an
app is told 1.4: `ndk/mg_egl.c`), hardware buffers (below),
`CameraManager.getCameraIdList` (null; now an empty list) and AAudio (now
`ndk/mg_aaudio.c`'s). Spotify's WebView, drawn on a `RenderNode`'s canvas
before there was a draw functor here, ended the app making its functor with a
null table: a canvas says it is hardware-accelerated to a WebView only where
the table is given.

**Direct system calls.** `libwebviewchromium.so` has five sites that make a
system call past libc: at +0x33c2dd2 (`gettid`), +0x33c2f6a (`futex`),
+0x33c2fee (`getpid`), +0x33c9574 (`rt_sigtimedwait`) and +0x33c971e
(`rt_sigprocmask`). Under qemu a direct system call goes to Linux and works
unseen; with the flag `trap-svc` the loader makes each site a fault
(arm/mg_patch.c). In every run of the WebView test with the flag on, none was
reached: loading, starting, five pages, JavaScript, cookies, a redirect and
typing make no direct system call.

**On the phone** (the HP Elite x3, packages 229 to 249): the test app
passes every step it passes under qemu, the provider made in 0.8 s and the
page loaded in 1.6 s, Chromium's GPU thread on ANGLE's Direct3D 11, with the
flag `trap-svc` as well; a tap where the log says the field is and the keys
h, e, l, l, o (the automation's flags `px-12-434-697` and `key-16-36` to
`key-20-43`) leave "hello" in the field, the window giving way to the
keyboard. Of real apps, VK mp3 mod and Kate open VK ID's "not a robot" check
in a WebView after a sign-in is tried, and it was shown -- the sheet, its
text, the button that closes it -- but the check itself went no further
than "loading" or "something went wrong, try again later", its canvases
having lost their GPU contexts (hardware buffers, below, came after; not yet
tried with it). Via, a browser built on the WebView, shows a search engine's
page of results over https; Chinese text in it is boxes (the root has no CJK
font). Twice a fresh start's first connection, to an address that took one a
moment later, failed with ENETUNREACH, and the page with it; not explained.

**Canvases and WebGL** are drawn by Chromium's GPU service into hardware
buffers (`AHardwareBuffer`, `ndk/mg_ahb.c`) it makes EGL images of
(`EGL_ANDROID_image_native_buffer`, named by the layer's EGL where ANGLE has
images of textures): each buffer is a texture of ANGLE's made in a context of
the layer's own (docs/ARCHITECTURE.md, "Graphics"). Without them Chromium lost
each canvas's GPU context ("GPU state invalid"): a 2D canvas went on in
software and WebGL was not offered. With them, Chromium ends each access to
a buffer with a fence of Android's (EGL_SYNC_NATIVE_FENCE_ANDROID), made by
its ANGLE through the system's EGL whatever that EGL names: on the phone that
was a call through nothing until the layer's EGL had fence syncs of its own
(`EGL_KHR_fence_sync`, `EGL_KHR_wait_sync`, `EGL_ANDROID_native_fence_sync`:
making one flushes, waiting for one is `glFinish`, every context of a display
being in order on its one Direct3D 11 device; no native fence descriptor is
given out). Under qemu Chromium does not draw canvases on the stand-in, so the
buffers are tried there only through the core's tests (`mg_gl_tests.c`,
test_hardware_buffers and test_fence_syncs), the fences also by Chromium
itself, which uses them once they are named.
The WebView test app's step "canvas" fills a 300x150 canvas of the page's and
reads a pixel back (`getImageData`), and says whether the page has WebGL and
what a pixel it cleared green reads: under qemu `webgl=no`; on a phone,
with buffers, it should read `webgl=yes:0.255.0.255`, and the log name the
first buffer (`AHardwareBuffer_allocate: the first buffer: 300x150`).

**A display of its own.** Chromium's GPU thread draws while the app's own
threads may be drawing too: a game shows an ad page in a WebView over its
running game. ANGLE's Direct3D 11 renderer belongs to a display and serves all
of that display's contexts with one state manager (what was last bound and
set) and its device's one immediate context, and counts on the contexts taking
turns through `eglMakeCurrent`. Two threads that each keep a context of one
display current take no turns, and ANGLE as gecko-w10m builds it gives a
context that shares nothing a lock of its own and lets them draw at once:
Asphalt 9 ended in `glDrawElements` inside ANGLE (`VertexArray11::syncStateForDraw`
reading a null vertex array, which the WebView context's `eglMakeCurrent` had
reset), a second and a half after the WebView of an ad page started (package
276). So `eglGetDisplay` called from `libwebviewchromium.so` (the caller's
return address) gives a second display of ANGLE's, made for the native display
`EGL_D3D11_ONLY_DISPLAY_ANGLE`, with a renderer, a state manager and a
Direct3D device of its own (`ndk/mg_egl.c`, `webview_display`); any other
caller gets the app's. The WebView's display is initialised and told as the
app's; a hardware buffer's texture is made on the display its first image is
asked for on, by that display's own thread and context of the layer's
(`graphics-buffers-webview` for the WebView's), and an image of it on the
other display is refused. Where ANGLE cannot make a second display, Chromium
shares the app's, as before. The core's test `test_webview_display`
(`mg_gl_tests.c`) asks for both displays and makes a buffer's image on the
WebView's.

**Its OpenGL ES version, and its frames' timing.** At every start the
WebView's own ANGLE (its OpenGL ES back end over this layer's EGL) has its
first context refused four times with `EGL_BAD_MATCH` before it has one:
`DisplayEGL::initializeContext` (ANGLE's `chromium/6613`) asks for OpenGL ES
3.2, 3.1, 3.0 and 2.0 in turn, each first with
`EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY` = `EGL_LOSE_CONTEXT_ON_RESET`
and then without, and ANGLE over Direct3D 11 makes 3.0 at most at any feature
level (`GetMaximumClientVersion` in `renderer11_utils.cpp`: "3.1 could
theoretically be supported on FL 11.0+, but is not supported in ANGLE"), so
3.2 and 3.1, with and without, are refused and 3.0 with its reset strategy is
made. That is EGL's answer on Android too wherever the graphics processor has
less than is asked (`EGL_KHR_create_context`); a phone's Adreno gives 3.2 at
the first try. The log said it as four refusals to look into; it now says
each version once as what it is (`context_refused` in `ndk/mg_egl.c`), and
any other refusal with what was asked (the version, the reset strategy,
robust access, flags, sharing). The WebView draws on the graphics processor
with its 3.0 context afterwards (Zona: "draw functor 1 drawn 29 times").
Dawn, Chromium's WebGPU, later asks for a 3.1 context of that ANGLE to find
an OpenGL ES adapter and is refused by it ("Requested version is not
supported"): there is no WebGPU here. And Chromium looks up Android 13's
`AChoreographer_postVsyncCallback` and `AChoreographerFrameCallbackData_*`
and says each is missing: this layer has no native `AChoreographer` at all
(none of Android 7's or 10's functions either). The WebView's begin frames do
not use it: `ExternalBeginFrameSourceAndroid` takes Java's `Choreographer`
when it is the WebView's (`requires_align_with_java`), and the lookup only
decides whether frames carry Android 13's deadlines; Flutter and the frame
pacing of Unity's Swappy take Java's `Choreographer` too while there is no
native one.

**Out of memory.** Chromium ends an app it cannot get memory for in
PartitionAlloc's `OnNoMemoryInternal`: the size it asked for goes into a
global of its library, and it traps (bkpt, then udf, where the phone's report
finds the pc). The crash report recognises that function's code at the pc
and reads the global: "Chromium ran out of memory asking for N bytes"
(`mg_core.c`, `mg_chromium_oom_global`). Checked on the function's bytes in
the core's tests, and under qemu by calling the library's own
`TerminateBecauseOutOfMemory` in a child process (test_chromium_oom_end,
with the Android root that carries the WebView). The report also gives the
address space views of shared memory and files hold, and the four largest
free stretches of it.

Spotify's WebView (packages 252 and 254) ended so, r4 holding 126,337,600,
which looks like the size: 4 bytes a pixel of an image. The stack has
Skia's cache of decoded images (`SkBitmapCache::Alloc`, its key tagged
'bmap'): a page drawn in software, as every WebView here is drawn, decodes
each image it draws at its full size, into Chromium's discardable memory. A
region of the image's size and a page more is made and mapped twice, on two
of Chromium's threads (the side that hands discardable memory out and the
side that uses it, both in this one process; measured under qemu with a
2048x2048 image: two views of 16 MB and 4 KB), and the descriptor's own view
made a third, while the largest free stretch of the 2 GB was 135 MB. Now a
descriptor keeps no view of its own (docs/ARCHITECTURE.md, the mmap row),
and the WebView's library no longer sits in a 130 MB reservation (above);
an image that big still needs two stretches its size. One view at one
address for both of Chromium's mappings was tried: Chromium counts its
mappings by address, and ended on its own check of that
(`shared_memory_tracker.cc(68)`, "Check failed: it != usages_.end()").

**What is not there.** Drawing on the graphics processor (the GL functor);
`AImageReader` and the rest of the imports the layer does not provide (video,
camera); the WebView APK's own processes and services (its developer UI, its
variations seed service: "Failed to bind to WebView service", which it
survives); class loaders of their own (above).

### Monitors and threads

A monitor is a lock with its owner and a count, made the first time an object is
locked; `wait` and `notify` are a condition on it. A wait ends when the monitor
is notified or its time is up by the clock, not when the host's own wait
happens to end: on Windows a timed wait ends at a tick of the system timer,
which can be most of the asked time too soon. A synchronized method in real
DEX is `monitor-enter`/`monitor-exit` instructions the compiler wrote (the
`synchronized` flag is only valid on native methods, where it is honoured).

The runtime's own tables -- classes, interned names and strings, registered
natives -- are under one lock. Each thread has its own frames and pending
exception.

A `Thread` is alive from `start()` until its `run()` returns, and
`getState()` is `NEW`, then `RUNNABLE`, then `TERMINATED` once it has ended
(it stayed `RUNNABLE`). The framework's `HandlerThread` is Android's: its
`getLooper()` is null at once for a thread not started or one that ended,
and waits only while the thread is alive and has no looper yet. It waited
for good: Facebook's stories loader stood in it for the whole run, the app
showing its loading circles (package 479). The data app's
"handler-thread" step asks one not started, one running, and one that quit.

### Memory

**A 32-bit process has 2 GB of addresses, and an app can run out of them
before its memory.** Yandex Maps did, half a minute after it started, each
time (package 489): 1.2 GB of its 2 GB of memory used, 69 MB of addresses
free and no stretch of them a megabyte long; the shell's own drawing failed
first (`CoreUIComponents.dll` writing through a NULL), then the map's OpenGL
(`GL_OUT_OF_MEMORY`) or its `malloc` (`std::bad_alloc`). What took them, and
what was done:
- **Google Play services' resource table** (84 MB, in every app it runs in:
  `megler.MicroG`) was read whole into the Java heap, and again for every
  `Resources` made of it. 58 MB of it is the strings of 75 languages. Another
  package's table is now read once for each APK, cut down to the resources of
  no language, of English and of the phone's languages (`apk/mg_arsc.c`, which
  also renumbers the strings the kept resources use; `readApkResourceTable`):
  about a tenth of it. The log says each one cut ("GmsCore.apk's resource
  table: ... KB, ... KB with the languages the app has"). The app's own table
  stays whole, since an app may switch its language itself. The core's tests
  cut microG's GmsCore's table to two languages and compare the strings the
  kept resources have with the whole table's.
- **Frame stacks**: the 199 threads each had 768 KB of addresses for one, 150
  MB. A thread the app's native code made and attached
  (`AttachCurrentThread`) calls into Java to be told something and comes back,
  and gets 128 KB (`DVM_STACK_ATTACHED_WORDS`). A thread Java starts gets 384
  KB (`DVM_STACK_JAVA_WORDS`, some four thousand frames, as many as ART's
  interpreter fits in an Android thread's 1 MB), or as much as it asked for when
  it asked for more than 1 MB; the main thread keeps the whole 768 KB. Yandex
  Maps had 229 threads Java started, Google Play services' among them, when its
  addresses ran out (package 493). The runtime's tests run a thousand calls deep
  and a recursion without end on an attached thread; the data app's
  "thread-stack" step two thousand on a thread of the default size, twenty
  thousand on one that asked for 16 MB, and one without end.
- **The memory line now says the addresses** too: how many megabytes are free,
  the largest free stretch, what is reserved and what committed. And a line
  after it ("java: addresses by kind") says what holds them: the program and
  its DLLs, views of files and shared memory, private memory and the sizes of
  allocation most of it is in, the threads (the core's, the runtime's and how
  many native code attached, their frame stacks), the runtime's tables and
  classes, and the graphics processor's memory. It asked the heaps too
  (`HeapSummary`, every heap of the process) in package 493, and was taken out
  of that: some of a process's heaps are not serialized, one was walked while
  its owner used it and the app ended, and the walk held the process heap's lock
  for most of a second.
- **And whose the sizes of allocation are**, where the core can tell
  (`address_whose`, which the census calls for each private allocation): of
  each of the four sizes, the owners with the most of them, in the form "599
  of 1024 KB (the Java heap's arenas 420, thread stacks 170, others 9), 2 of
  8192 KB (Go's runtime in libgojni.so)" (three owners a size at most;
  "others": those no owner is known of). The owners: the Java heap's arenas;
  an app's mapping by the library whose code made it (the place of a system
  call by number, else libc's caller), and Go's runtime when that was a
  library of Go's by number (`mg_mmap_whose`); a native library's image; the
  runtime's frame stacks; the threads' stacks for system calls; and, of what
  none of those is, an allocation with a guard page in it is a thread's
  stack. WhatsApp's log said "1 of 528384 KB" and "599 of 1024 KB", and whose
  they were took a disassembly and a guess (0.1.0.671).

**An app's DEX files are views of files where they can be**
(`mg_dvm_apk.c`), not memory of the process's, which Windows counts against
the app's limit:
- One the APK stores as it is, on a 4-byte boundary as zipalign puts it, is a
  read-only view of those bytes of the APK. Google Play services 26 stores its
  fifteen (109 MB) so.
- One the APK deflates is inflated once into `<data_dir>/dex/`, named for the
  APK, the entry and the entry's CRC-32 and size, and viewed from there, as
  ART keeps an app's DEX files outside its APK too. It is made under another
  name and renamed when whole; the other files of the same APK and entry (an
  older version's) are removed. Play Store 53 deflates its eight (58 MB). The
  name had a hash of the APK's path in it, and the path of an APK Megler brings
  has Megler's version in it: every update of Megler made another copy (four
  of Facebook's, package 482).
- Where neither can be made, the file is read into memory, as before.
- **A view is made only when it is needed.** Every file was viewed whole from
  the start, its checksum read over all of it and its caches of what each
  index resolved to made: Yandex Maps' sixteen and Google Play services'
  fifteen in it were 245 MB of a 32-bit process's 2 GB of addresses, read from
  the flash at every start, whether a class of them was used or not (package
  494). Now the file is viewed for a moment, its header and tables checked and
  its classes' names copied into one block of its own (the table of
  definitions points at them, `mg_dvm_classes_add_dex`), and let go
  (`mg_dvm_add_dex_lazy`). The first of its classes to be linked views it
  again (`mg_dvm_dex_ready`, in `link_dex_class`): the same file, its checksum
  right, and its caches made then. A file changed on the disk meanwhile, or
  that cannot be viewed again, is said in the log, and its classes are
  `NoClassDefFoundError`s. Every lookup that reaches a file's bytes starts from
  a class linked from it, so that is the one place. The runtime's tests store
  a DEX file in an APK of their own: added, it is not viewed, nor when its class
  is only asked about; linked, it is; and one changed after it was added, in a
  scope of its own, gives the error. The same bytes of the same file registered
  for two scopes (a service's or a process's over the app's own APK) share one
  view, as the bytes read for the app's own served such a scope before ("N of
  its DEX files are the ones read before, shared").
The log says it per APK ("N of its DEX files are views of files, each viewed
once a class of it is needed: ... of the APK, ... inflated into the data
folder's dex (... of them now)"), and after
each of the frame meter's lines a line "java: memory:" says the process's
parts the core can name: the heap's blocks and large objects, the DEX files
in memory and viewed, the native libraries' segments (`mg_ld_image_bytes`,
read into committed memory), what the app's native code has from `malloc`
and not freed (`mg_g_malloc_bytes`; the blocks are the host's), the app's
own mappings, anonymous and private copies of files, as far as they are
mapped, not made no-access and not given back (`mg_mmap_usage`), and what
madvise has given back. What is left of the process's figure is the
runtime's own `malloc`, the shell, Direct2D and ANGLE. Play Store 53 with Google Play services
26 was 1100 MB in package 479, the heap's blocks and large objects 300 MB of
it.

**Objects are collected** (`mg_dvm_gc.c`, the next section). Every object is
a zeroed allocation from the runtime's own heap (`mg_dvm_heap.c`). Allocation
goes through one function, `alloc_raw` in `mg_dvm_object.c`, which also asks
for a collection when enough has been made.

**An allocation takes the heap's lock and no other atomic operation.**
- Each thread counts what it has made itself (`made_objects`, `made_bytes`);
  `mg_dvm_get_stats` sums them under the runtime's lock. Before, five
  atomic additions per object kept global counts.
- A thread tells the collector what it made in pieces of at least 32 KB
  (`gc_unsaid`, `mg_dvm_gc_counted`), so a collection comes late by less
  than a piece per thread.
- A cell's index in its block is the offset times the cell size's
  reciprocal (`cell_index`), not a division. 32-bit ARM divides in a library
  routine, and this is done for every allocation and every object a
  collection marks.

Benchmark on x64 in ns a turn, the interpreter at the same place in both
builds: a small object 60.5 to 52.7; an `int[16]` 40.1 to 33.0;
`Integer.valueOf` 92.1 to 81.2; `StringBuilder` 955 to 897; `HashMap.get`
329 to 307; an exception made and caught 345 to 315.

### The collector (design, round 7; built in steps)

Why now: the frame meter's first lines from the phone. Apktool M's heap grew by
about 5 MB a second, 239 MB at the start and 1227 MB after three and a half
minutes. The process reached 1.6 GB, Direct2D's `CreateBitmap` failed with
E_OUTOFMEMORY, and the core aborted. Part of that growth was a layout loop
that is fixed now (`AbsListView`, above). But any app that animates or
scrolls allocates without end, and an app that runs long enough dies.

This replaces the plan under "Threads and memory" below in one respect. That
plan had native code exact (a table of local references per thread) and native
stacks not scanned. This one scans every stack conservatively and stops threads
where they are. The reason is the runtime's own C code. The natives of the
drivers, of `mg_dvm_art*.c` and of the JNI layer hold raw `mg_jobj *` in C
locals across calls that allocate:
`args[0].l = mg_dvm_new_string(a); args[1].l = mg_dvm_new_string(b);` is the
pattern, and there are hundreds of them. An exact table would need every one of
them found and annotated, and a missed one is a use-after-free. Scanning the C
stack finds them all without a line changed. It also covers guest JNI code that
keeps a local reference in a local, which is what local references are for.

**What it is.** A non-moving, stop-the-world mark and sweep.
- *Non-moving*: objects stay at their addresses. Every `jobject` is an address,
  `Get<Type>ArrayElements` and `GetPrimitiveArrayCritical` hand out the array's
  own storage, the identity hash is the address, and `Unsafe` works on offsets
  from it.
- *Mark*: the world stopped, from the roots below, with an explicit mark stack.
  The mark stack is the collector's own, kept from one collection to the next
  and grown before a stop to as many entries as the heap has objects (half
  again as many ahead), where that can be had; so is the list of reference
  objects found while marking. Each was allocated at every collection and
  freed after: WhatsApp's 1.5 million objects wanted 6 MB in one piece while
  its process had no stretch of addresses that long, and every collection from
  then on was given up, dozens a second, until an `OutOfMemoryError` (HP Elite
  x3, 0.1.0.671). Where one cannot grow it stays as it was: the stack
  overflows into walks of the heap for marked objects (`heap_rescan_marked`,
  each pass until one marks nothing new), a reference with no room on the list
  keeps its referent that time, a finalizable object with no room to be queued
  is kept for a later collection; and before the first growth there are small
  ones in the collector's own data. A collection is now given up only when a
  thread cannot be stopped, and the log says which and why. Once it holds
  enough (2048 objects), the mark is shared with helper
  threads of the collector's own, one fewer than the processors and three at
  the most: the stack becomes a pool they take chunks of into stacks of their
  own, a mark bit is set with an interlocked compare-and-swap so that each
  object is pushed once, and a thread with more than it needs gives half back
  when another has nothing. The helpers are made with their stacks before the
  first collection, so nothing is allocated while the world is stopped. Yandex
  Maps' mark took 325 ms for 770,000 objects on one thread. The flag
  `no-parallel-gc` keeps it on one.
- *Sweep*: after the world goes on, under the heap's lock. Objects allocated
  while it runs are born marked. The same helpers share it, a block at a
  time; and the threads are stopped and resumed by all of them at once, each
  taking the next thread of a list made first.
- No generations and no compaction. Fragmentation is limited by size classes.

**The heap.** It is no longer the C heap.
- *Small objects*, up to 2 KB, live in blocks of 64 KB, each of one size class
  (16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048
  bytes). Blocks come from the host as aligned reservations. A block has a
  header with its class, a free list, and two bitmaps, "allocated" and "marked".
- *Large objects*, over 2 KB, are allocated one by one. They are kept in an
  array sorted by address, each with its mark bit.
- "Is this word inside an object, and which one" is answered by the address.
  The block's base is found from a sorted array of block bases (binary search;
  some thousands of blocks for a gigabyte). The object is found by
  `(address - first) / size` and the "allocated" bit. A large object is found
  by binary search over its sorted array.
- Interior addresses count. A pointer to an array's elements keeps the array.
  This is the case for `u.arr.data` in a C local, or for what
  `GetByteArrayElements` returned to guest code.
- An empty block goes back to the host after a sweep when more than a few are
  empty.
- Blocks come sixteen at a time in arenas of 1 MB, each a reservation of the
  host's watched for writes (marking alongside the app, below). An arena that
  cannot be watched is had unwatched and every page of it counts as written at
  a second stop; the next one is watched again. (The first such arena took the
  watch from every arena for good: each second stop of WhatsApp's after it
  looked at every marked object of a heap of 400 MB, 1.4 to 1.6 s.) The
  warning says Windows' error.
- *Lowest first, and arenas given back.* A sweep makes the lists of blocks
  with room and of spare blocks from the highest block down, so that each
  hands out its lowest first: what lives gathers low and the arenas high up
  empty. After the sweep, the arenas left wholly empty past what the next
  budget wants (two budgets of spare blocks, and 8 MB more) are given back to
  the host whole, the highest first, their addresses too
  (`mg_dvm_heap_trim`). The heap kept the addresses of the most it ever had:
  WhatsApp's 565 MB of arenas for some 95 MB that lived, until its process
  could not get one more megabyte. The memory line says the arenas and how
  many have been given back.

**Roots, and exact or conservative.**
- *Conservative*:
  - Every registered thread's C stack, from its stack pointer at the stop to
    the top recorded at registration (`mg_thread_stack_bounds`), and its
    registers at the stop. This covers the runtime's C code, the drivers, the
    JNI layer and guest native code alike.
  - Every thread's frame stack, the interpreter's registers, from its base to
    its `sp` (plus a margin of 64 words). A register is untyped here: an int
    that looks like an address keeps an object a little longer. In exchange
    nothing has to know which register holds a reference at which pc, which
    would take a verifier.
- *Exact*:
  - Inside objects. A class's reference slots are its fields of type `L` or
    `[`, its superclasses' included, as a slot list made at link time. An
    object array is all references. A string has none. `mg_jobj.fields`, the
    JNI layer's by-name fields of its stand-in classes, counts by its
    `is_object` flags.
  - Every linked class: its statics (typed by their fields), its `Class`
    object, its `init_error`. Classes are never unloaded.
  - The runtime's tables: interned strings (each DEX file's resolved string
    constants, `scache`, are interned ones, and not walked apart),
    reflection's `Method`/`Field` objects (kept for good,
    one per member), proxy classes' method arrays, the preallocated
    `OutOfMemoryError`.
  - Each thread's pending exception, `peer` (its `java.lang.Thread`) and
    `scratch`.
  - The JNI layer's global reference table (`g_refs` in `mg_jni.c`, which
    `NewGlobalRef` already keeps with counts) and its stand-in classes' statics.
  - C globals of the drivers and the runtime that hold objects: the Java
    driver's clipboard, `g_dvm.app_loader`, and what an audit of every
    `mg_jobj *` in a static or a C struct finds. They are registered with
    `mg_dvm_add_root(mg_jobj **)`, and the audit's list goes into this section.
- *Weak global references* (`NewWeakGlobalRef`) are strong for now. A weak
  handle here is the object's address. If the object were freed and its
  address reused, `IsSameObject(weak, NULL)` would say "alive" for an unrelated
  object. Clearing them properly needs indirect handles, which is later work
  with its own commit.
- *Not covered*: a `jobject` that native code keeps in its own heap or globals
  without `NewGlobalRef`. That is invalid JNI, and on Android such a reference
  stops working at the native method's return. `jclass` values cached so do
  survive, because classes are never freed.

**How threads are stopped.** Asynchronously, with no safepoints.
- `mg_thread_suspend` and `mg_thread_resume` (host layer) stop a thread
  wherever it is and read its registers. On Windows: `SuspendThread`, then
  `GetThreadContext`. `SuspendThread` only asks, and `GetThreadContext` is the
  documented way to wait until the thread has stopped. The context is only
  read; `SetThreadContext` is never called. Elsewhere: a signal of the host's
  own whose handler saves the registers, says so (a semaphore) and waits in
  `sigsuspend` for the resume signal. `mg_thread_resume` returns only once the
  handler has left its wait, so the next suspension cannot find it still
  there (the first version deadlocked on exactly that).
- *ARM state.* Real libraries have hundreds of ARM-mode functions (MX
  Player's `libmxvp.so`: 358). If a thread stopped in one came back in Thumb
  state, the collector would crash apps at random. The core's tests have a
  case for it (`test_suspend_arm`, which the phone's Self-test runs). A
  thread counts in a hand-written ARM-mode loop (`mg_stub_arm_count`), each
  turn depending on the one before. It is stopped and resumed three
  thousand times with its registers read in between, and must go on
  counting, return when told, and its registers must say ARM state.
  - Under qemu: 3000 stops, 2996 of them in the loop, all 2996 in ARM state;
    the count reached 31 million.
  - On the phone (the Self-test of 0.1.0.138): 3000 stops, all 3000 of them
    in the loop, all in ARM state and none in Thumb state; the count reached
    354819. `SuspendThread`, `GetThreadContext` and `ResumeThread` keep a
    thread in ARM state there, as the device probe had shown for ordinary
    interrupts ("ARM mode" in ARCHITECTURE.md).
  - The same Self-test ran the collector's census and a collection on the
    phone, and both passed: 15712 of 21520 objects reachable, 2 threads
    stopped, 0.3 ms to stop and 7.5 ms to mark. The collection freed 7409
    objects (1053 KB) in 6.9 ms. Apps on the phone have not yet been seen
    collecting.
- *Which stack is scanned*: from the stopped stack pointer to the thread's
  real stack base, `mg_thread_stack_bounds`, recorded at registration
  (`GetCurrentThreadStackLimits` on Windows, `pthread_getattr_np` on Linux).
  Not "the top where it registered": a thread registered deep in a call may
  later run Java from shallower frames.
- The collector does this, in this order:
  1. It takes the runtime's locks: `mg_dvm_lock`, the heap's lock, the JNI
     references' lock and the interned strings' lock.
  2. It suspends every other registered thread and reads its stack pointer and
     registers.
  3. It marks.
  4. It processes references (below).
  5. It resumes the threads.
  6. It releases the locks.
  7. It sweeps.
- Because the locks are taken first, no stopped thread holds one of them.
- Between the stop and the resume the collector allocates nothing, frees
  nothing and logs nothing. A stopped thread may hold the C library's heap
  lock or the log's.
- A thread is registered by `mg_dvm_self` (it already is), with its `mg_thread`
  and its stack's top. It is unregistered when it ends: the end of a Java
  thread, `DetachCurrentThread`, and the bionic layer's thread exit for guest
  threads that attached. A thread that cannot be suspended because it is gone
  is dropped from the list.
- A thread suspended in the middle of anything is fine for a non-moving,
  non-incremental collector. A reference being moved is in a register or still
  in its old place, and both are scanned. An object just allocated is in a
  register or a local. The heap's lock means no thread is halfway through the
  allocator's lists.
- Threads that never touch the runtime are not stopped: the shell's UI thread,
  audio threads, the host's drawing threads.

**When it runs.**
- When an allocation finds that more than the budget has been allocated since
  the last collection. The budget is the larger of 8 MB and half of what
  survived the last one (three quarters before: Play Store with Google Play
  services had 350 MB of heap at each collection for 200 MB that lived). The
  bytes of bitmaps made since then count too
  (`mg_java_gfx_stats`), since that memory is freed only through the objects.
- `System.gc()` and `Runtime.gc()` (today they do nothing).
- Before an `OutOfMemoryError`. If an allocation would pass the heap's limit,
  a collection runs that also clears soft references, and only if that is not
  enough is `OutOfMemoryError` thrown. The limit is 512 MB, or half of
  `mg_mem_limit`, whichever is smaller.
- The collection runs on the allocating thread, before it takes the heap's
  lock. Its pause and the result are one line in the log ("gc: 41 ms, 212 MB
  live of 260, 48 MB freed, 3 threads stopped"), and the frame meter's line
  gets the time spent collecting.
- *Alongside the app.* Where the heap's arenas are watched for writes, a
  collection due to allocation marks alongside the app (`collect_alongside`):
  two short stops, the marking between them by the collector's thread and its
  helpers at the lowest priority while the app runs, what the app makes
  meanwhile made marked and kept. Its budget comes from what it *traced*: what
  the heap holds after its sweep less what was made while it marked
  (`mg_dvm_heap_black_made`), which counts toward the next collection instead
  -- it is mostly the next one's garbage. A thread that has made two budgets
  since the last collection (the app's main thread three; one budget, two for
  the main thread, where the machine's memory is pressed) waits for the
  collection under way; at a budget and a half, or with any thread waiting,
  the collection is behind and its markers take their usual priority until it
  ends, and the next one starts so (`hurry`): raised by the thread that finds
  it behind, at once, since a marker of the lowest priority that the app's
  threads keep from running would not get to raising its own. The heap then
  holds at most what lives and two budgets, about twice what lives. Before,
  the budget was half of everything the heap held after the sweep, what was
  made while it marked included, and threads waited only at three budgets:
  WhatsApp's collections marked at the lowest priority for 4 to 16 s on the HP
  Elite x3 while its threads, Google Play services' among them, made 100 to
  320 MB, each took that for living, and its heap grew to 565 MB of blocks for
  some 95 MB that lived, in 90 s (0.1.0.671). The collection's line says what
  it traced, what the heap holds and what was made while it marked, the
  budget, the arenas and how many were given back, the mark stack's most, and
  when it was behind.
- *The process's addresses* (a 32-bit host's 2 GB): after each collection's
  sweep, at most once a second, the free addresses are counted
  (`mg_host_address_space`). The budget stays within a quarter of them beyond
  192 MB, and with fewer than 128 MB free a collection due to allocation stops
  the app while it marks, so that nothing made meanwhile is kept.
  `mg_dvm_gc_set_address_probe` stands in for them in the tests.
- `test_gc_heap_near_live` (`core/tests/mg_dvm_tests_gc.c`, and
  `MG_DVM_TESTS_ONLY=gc` with `mg_tests --dvm-only` runs the collector's tests
  alone): four threads make 480 MB of garbage, small arrays and some over 2 KB,
  and now and then replace an array of a live set of 130,000 that one root
  holds, while collections mark alongside them; the heap's blocks are sampled
  every 10 ms. Twice: as the host's scheduler has it, and with the markers
  starved as a phone's busy cores starve a thread of the lowest priority
  (`mg_dvm_gc_set_marker_pace`: 32 objects followed, then a millisecond
  asleep, while a marker's priority is the lowest -- under qemu and on the PC
  every marker has a core of its own, and its priority costs it nothing). The
  blocks must stay within three times what lives (and 24 MB), what is in use
  within two and a half, and once the live set is let go the empty arenas go
  back. The run is a thread of its own, which ends before the live set is let
  go: a word of it left on the test's stack kept all of it. Under qemu-arm,
  the collector's tests alone, with the markers starved: before, 4
  collections in 15.2 s and the blocks at most 218 MB for 35 MB that lived
  (6.3 times), in use 332 MB, the arenas' addresses 218 MB and never fewer;
  after, 30 collections in 2.7 s, the blocks at most 45 MB for 30.5 MB (1.5
  times), in use 64 MB, the arenas' addresses 45 MB, and 26 MB once the live
  set is let go. As the scheduler has it, the blocks at most 50 MB before and
  41 MB after.

**References, finalizers, queues.** libcore's `java.lang.ref.Reference` has a
`referent` that ART treats specially, and so will this collector.
- While marking, an object of a subclass of `Reference` has its `referent`
  slot skipped and goes on a list.
- After marking:
  - `WeakReference`: the referent is cleared if it is unmarked.
  - `SoftReference`: the same, but only in a collection that clears soft
    references (before an `OutOfMemoryError`, or when the heap is past three
    quarters of its limit).
  - `PhantomReference`, `sun.misc.Cleaner` among them: cleared and enqueued
    when the referent is unmarked. `NativeAllocationRegistry` frees native
    memory through `Cleaner`.
- A cleared reference with a queue goes on a pending list linked through
  `pendingNext`. Nothing runs Java on the collecting thread, which may be deep
  in C code holding locks. The list is handed to a reference thread of the
  runtime's own, a Java thread made at start, which calls
  `ReferenceQueue.enqueuePending(list)`. That is libcore's own method, the one
  the ReferenceQueueDaemon calls; it runs `Cleaner.clean()` for cleaners and
  enqueues the rest.
- *Finalizers.* An object whose class overrides `finalize()` with a body
  (ART's "finalizable") is recorded at allocation in a C list, not through
  libcore's `FinalizerReference`. After marking, every unmarked finalizable
  object is marked again with everything it reaches (resurrected), taken off
  the list, and queued. The same runtime thread calls `finalize()` on each,
  catching what it throws. The next collection frees the object if nothing
  revived it. `Runtime.runFinalization()` waits until the queue is empty.
- libcore's `Daemons` are not started. FinalizerWatchdogDaemon would end the
  app if a finalizer took ten seconds, which is a policy for later.

**Bitmaps' native memory.** The framework's `Bitmap` has a `finalize()` that
calls `recycle()`, which frees the `megler.Gfx` handle (`Gfx.bmFree`). So with
finalizers, a bitmap nobody holds frees its pixels at the collection after
next. Bitmap bytes made count toward the next collection's budget, as said, so
an app that makes bitmaps and drops them collects before Direct2D runs out.

**The sweep frees what an object owns besides itself**: its monitor, a string's
cached UTF-8, the `fields` array of a stand-in object.

**How it will be tested.**
- *The core's tests* (both hosts):
  - allocate and drop in a loop with a small heap limit, so that the limit is
    passed many times over and the live size stays put;
  - a root held only by a C local of a native, across a collection;
  - a reference held only in an interpreter register;
  - an interior pointer into an array;
  - statics and global references;
  - weak, soft and phantom references with queues;
  - a finalizer that resurrects its object;
  - four threads allocating and checking trees of objects while collections
    happen.
  - A debug switch (`MG_DVM_GC_POISON`) fills freed objects with a pattern and
    keeps their memory out of reuse, so that a use after free shows as the
    pattern.
- *A test app `gc`* under qemu: an Activity that allocates for minutes with a
  bounded heap, checks its own data, uses `WeakReference`, `SoftReference` and
  `ReferenceQueue` and finalizers, and runs threads that allocate at once.
- *The `natives` app*: a JNI method that makes objects (`NewStringUTF`,
  `NewObjectArray`), calls `System.gc()` through JNI, and checks them;
  a global reference that survives.
- *Apktool M under qemu with the frame meter*, for minutes: the heap must stay
  flat.

**Order of the work.** Each step is a commit that builds and passes.
1. The heap: blocks, size classes and large objects in place of the C heap,
   with nothing freed yet. (Done: `mg_dvm_heap.c`, with `test_gc` checking
   that an address inside an object, an array's elements included, is found
   to be that object's, small or large, and that a stack address is not.)
2. Registration and suspension of threads, and the conservative scan. Then a
   collection that only marks and reports what it would free, run by the
   tests. (Done: `mg_dvm_gc.c`'s census, with the roots as listed above. A
   thread is registered with its `mg_thread` and its stack's limits, and a
   host hook (`mg_thread_set_end_hook`) takes it out of the list when it
   ends; its frame stack is freed then. `test_gc` holds objects through each
   kind of root and checks that each is found: a C local of another thread
   that is stopped, a registered C root, a chain through an object array, an
   interior pointer. It also checks that 100 arrays nothing holds are not
   found. On libcore under qemu: 94,142 objects, 16,204 reachable (1.5 MB), 9
   found first by a conservative root; 8.6 ms to mark.)
3. The audit of C roots, the sweep, the trigger and `System.gc()`. (Done.
   The sweep frees the cells and large objects nothing reached, and what
   they own besides themselves: the monitor, a string's UTF-8, the JNI
   fields. An empty block becomes a spare for any class. Past the first 128
   spare blocks (8 MB), a spare block's memory after its first page, which
   holds its header, goes back to the host, and is committed again, zero,
   when the block gets a class: the heap's arenas no longer stay committed at
   the most the heap ever had.
   - `alloc_raw` collects when the bytes made since the last collection pass
     the budget: the larger of 8 MB and half of what lived after the last
     one. Native memory registered since counts too
     (`VMRuntime.registerNativeAllocation`, and every bitmap's pixels from
     `mg_java_gfx.c`). It collects once more before an allocation fails.
   - `Runtime.gc()` collects.
   - Each collection is a line in the log: reason, pause split into stop,
     mark and sweep, freed, live, threads stopped, and what only the
     conservative scans found.
   - The frame meter has "collecting N ms in M collections".
   - The audit of C roots found the Java driver's clipboard (registered) and
     the tables the collector reads itself; Unity's and NativeActivity's
     stand-ins run without the runtime.
   - Marking reads nothing of a reference until the heap has said it is an
     object's start in use (`mg_dvm_heap_set_mark`). The first runs found
     why: a JNI stand-in's static held the number 0x6B in an object-typed
     field.
   - All the tests run with collections on and poisoned (1 MB budget in the
     runtime's tests, 2 MB under the test apps): freed memory is 0xDB and
     never reused. Every test still passes. The libcore run alone collected
     about a hundred times, 10 to 14 ms each under qemu for 20,000 live
     objects.)
4. References and the reference thread.
5. Finalizers, and bitmaps with them.
   (4 and 5 done together.)
   - A class is known at its first instance as a `java.lang.ref` kind (soft,
     weak or phantom) and as finalizable: a `finalize()` other than
     `Object`'s that does more than return, as ART has it
     (`mg_dvm_gc_class_ready`).
   - While marking, a reference's `referent` is not followed: the reference
     is noted unless it already waits to be enqueued (`pendingNext` set).
   - After the strong marking, in Java's order:
     1. soft referents are kept, unless the collection is the one before an
        `OutOfMemoryError`;
     2. weak (and cleared soft) references whose referent nothing reached
        are cleared, and those with a queue join a ring through `pendingNext`;
     3. finalizable objects nothing reached are taken off their list and
        kept, with what they reach, for their `finalize()`;
     4. phantom references whose referent is still not reached are cleared
        and enqueued.
   - The reference thread is a thread of the runtime's own, attached as a
     daemon and started at the first collection with work for it. It hands
     the ring to libcore's `ReferenceQueue.enqueuePending`, which runs a
     `Cleaner` and enqueues the rest. It calls each `finalize()`, catching
     what it throws. An object waiting for it is a root; after its
     `finalize()` the next collection frees it unless it came back, and
     `finalize()` is never called twice.
   - `VMRuntime.runFinalization` is Java in libcore and waits on
     `FinalizerReference`'s list for the FinalizerDaemon. That list is
     filled by Android's runtime, not this one. The method is made a native
     one before `VMRuntime` is first used, and it waits for the reference
     thread (`System.runFinalization()` threw "AssertionError: newly-created
     live Sentinel not on list!" before).
   - A bitmap nobody holds has its pixels freed through `Bitmap.finalize()`.
6. The `gc` app and the stress tests. (Done.)
   - `core/tests/app/gc`, run by `mg_tests --java gc` under the collector's
     poison, has eight steps, all passing under qemu:
     - weak: 100 cleared, 100 enqueued;
     - soft: kept;
     - phantom: 100 enqueued;
     - finalize: 100 finalized;
     - resurrect: brought back once and finalized once;
     - churn: 62 MB made through a window of a thousand arrays, each checked
       when it leaves;
     - threads: four threads building and checking trees and strings, 48
       rounds right;
     - bitmaps: 40 dropped, and 1 bitmap left at the end.
   - Afterwards 44 collections had taken 483 ms in all, and 3 MB was live.
   - The `natives` app's `gc-held` step: a native method holds a string and
     an array of strings in local references, and a string in a global
     reference made in an earlier call. It makes 2000 dropped byte arrays,
     calls `System.gc()` through JNI, and reads everything back unchanged,
     twice.
   - Apktool M under qemu with the collector and no poison: 49 MB of heap
     after its start (240 MB before), 43 MB after its licence, scrolling and
     waiting, 30 MB live after each collection; a collection of 200,000
     objects takes 60 to 100 ms there (mark nearly all of it).

### What is not checked

There is no bytecode verifier and there are no access checks. A method whose
instructions name registers it does not have, or use an int as a reference, is
not caught; `private` is not enforced. What is checked is everything that comes
from outside the instruction stream's own logic: the file's header, checksum and
table bounds, every index before its table is read, array kinds and bounds, that
an object has the field an instruction reads. An app's DEX is trusted as its
native code is.

## What is not done

- On the phone, Java apps run and are shown (Apktool M among them), and the
  Self-test has run the runtime's tests there. Most of what is said proven in
  this document was proven under qemu and on the PC, not on the phone.
- The collector ("The collector" above) frees objects, clears and enqueues
  references and runs finalizers. Weak global JNI references stay strong.
  Finalizers run on one runtime thread with no watchdog.
- No `MethodHandle` (`invoke-polymorphic`, `invoke-custom`,
  `const-method-handle` and `const-method-type` throw). A class loader cannot
  give two classes one name, and classes are not unloaded.
- Of the framework's native side only `megler.Native` and `megler.Gfx` are there
  (see "The Java driver"): no database, no sound from Java, no web view, no
  sensors, no video, no keyboard. (Out of date: databases, the keyboard,
  OpenGL ES from Java and the sensors are there now, each in its section.)
- **OpenGL ES from Java on a phone has not run.** Under qemu and on the PC it
  runs on a stand-in that draws. A `SurfaceView` that the app's *native*
  code draws on is there now (above, "A SurfaceView drawn by native code"),
  a `SurfaceTexture`'s too, likewise only under qemu. (A surface ordered on top of
  the window, opaque or translucent, is composed over the views now -- up to
  package 681 nowhere: docs/ARCHITECTURE.md, "Surfaces over the window".) Not there at all: more than eight
  views with EGL at once, `GLES32.glDebugMessageCallback` and the other
  calls Android's own glue leaves "not yet implemented" (the same exception
  here), and the composition on the GPU (docs/ARCHITECTURE.md says what it
  would take). `PackageManager.hasSystemFeature` still says yes to the
  Android Extension Pack (OpenGL ES 3.1 and more), which ANGLE here does not
  have.
- Images other than what the shell decodes; the stand-in reads PNG alone, and
  Windows 10 Mobile has no WebP decoder, which apps' resources use heavily.
- **The network passes on the HP Elite x3** (2026-10-16): sockets and names are the phone's
  own Winsock's (`bionic/mg_net.c`, `mg_netdb.c`; docs/ARCHITECTURE.md, "The
  network"), and `java.net`, `HttpURLConnection` over http and `java.nio`
  work on them under qemu and on the phone. The phone also reads external
  HTTP and HTTPS responses; see [PHONE-NETWORK.md](PHONE-NETWORK.md).
- **TLS is conscrypt's** where `libjavacrypto.so` is in the root ("The
  security providers"): tested under qemu and on the phone, on loopback and
  against Internet servers. Android 10's SSLSockets and public ALPN APIs
  bridge to that same provider. Where it is not in the root,
  digests, random numbers, MACs and ciphers are had without it, and TLS and
  whatever asks conscrypt's provider by name for more than a digest are not.
  conscrypt's `SSLEngine` (what Netty and gRPC use) has not been tried.
- An app in several APKs runs as one ("Split APKs and bundles"), but the
  components declared only in a feature split's manifest are not known, every
  split is on one class path, and an app is given ten splits at most. Not yet
  tried on a phone.
- Bytecode cannot call a method of a class only C implements.
- Thread interruption does not wake a waiting thread. A thread's state is not
  freed when it ends.
- The layer's libc now has every name the seven libraries import, but some
  only refuse: `splice`, `inotify_*` (`ENOSYS`); `mkfifo`, `mknod`, `capset`
  (`EPERM`); the extended attributes (`ENOTSUP`); the terminal calls
  (`ENOTTY`); `setmntent` (no mount table); `seteuid` and its kin succeed for
  the app's own id and refuse any other. `sendfile`, `posix_fallocate64`,
  `pathconf`, `getgrgid_r` and the checked forms of `pread`, `pwrite` and
  `readlink` do what they say.
- **Two things about the loader, known and left** (`ld/mg_ld.c`, not this
  module's): a library is known by its file name alone and an app's own are
  looked for first, so an app that bundles a `libcrypto.so`, `libexpat.so` or
  `libicuuc.so` of its own would collide with libcore's; and `libopenjdk.so`
  exports plain names (`equals`, `canonicalize`, `fdval`, `readBytes`) that the
  loader's last, "any library" step of resolution could hand to an app's
  library. A lookup by where a library came from fixes both.

## The plan

What is left, in the order it should be done.

### 1. On the phone

- Run the Self-test of a `--libcore` package (above) and fix what the Win32
  host shows.
- The shell's half: `megler_gfx_host` on Direct2D, DirectWrite and WIC, and
  `present_pixels`. Until then a Java app runs unseen.
- One shared mapping of a file mapped read-only more than once (ICU's data:
  47 MB down to 23.5).
- The resolution tables of a DEX file made on demand (7 MB at an app's
  start, more than its objects now).

### 2. The framework: `core/framework`, built here

Decided on measurements (`bash tools/dvm-report.sh link-framework`, and `link
<file>`; `python tools/dvm-natives.py` for the native methods):

| | Android 9's own `framework.jar` | the Java framework in `core/framework` |
|---|---|---|
| DEX | three files, 21.4 MB | one file, 7.6 MB |
| classes | 13,568 | 5965 |
| that link against libcore alone | all | all |
| classes, methods or fields it names and libcore lacks | 13 classes of `ext.jar` and elsewhere | none (with the overlay) |
| native methods it declares | 3713 in 222 classes | 1018 in 16 classes |
| of those, OpenGL ES and EGL wrappers | 1221 | 832 |
| the rest | 2492: Skia and hwui, media, SurfaceFlinger and input, Binder... | 186: `megler.Gfx` 54, `megler.Native` 35, `megler.Sqlite` 29, `megler.Audio` 27, `megler.Web` 18, `megler.MediaCodecNative` 11, `megler.EGLNative` 8, `megler.Sensors` 4 |

Android's own framework is the client half of a system: behind its natives
are Binder, `system_server` with the activity and window managers,
SurfaceFlinger, Skia. The one in `core/framework` is the same API with those
services written out in Java inside the app's own process.

`tools/build-framework.sh` builds it: `core/framework/src` and `core/framework/gen-src`
(3077 files of them used; `core/framework/stubs` to compile against), read where they are
and never written to, with `core/framework-overlay` on top, where a
file replaces the framework's own of the same path. javac of JDK 1.8.0_202, d8 of
build-tools 29.0.3 with `--min-api 26`; 5808 classes; `build/framework/
megler-framework.dex`, which `tools/dvm-android-root.sh` puts in the root. `bash
tools/dvm-report.sh link build/framework/megler-framework.dex` says: 5965
linked, 0 not; 0 classes, methods and fields named and not found.

**What differs from the framework as it came: a hundred and thirteen files** in the overlay,
most of them a statement or a few methods each inside an otherwise untouched copy
(the copies are whole files, because a file is what the overlay replaces),
**and thirteen files of members added** to its classes, **and four of its files taken
away**.

The second kind is for the platform's public members that the framework's own
class does not have and an app calls -- a `NoSuchMethodError` where Android
has a method, which nothing catches. `X.java.members` in the overlay holds
methods and fields that `tools/build-framework.sh` puts into the framework's own
`X.java` before the closing brace of its outermost class; the file begins
with comments that say what is added and why. Copying `View.java`, 1867
lines, to add three methods would hide what was added. What an app names
and the framework lacks is found without running the app:
`mg_tests --dvm-link <android root> megler-framework.dex <the app's dex>`
lists, among "methods named in one of their own classes that has none such",
every member of a framework class the app's DEX refers to that is not there.
For the three apps tried that list was `View.getDrawingTime`,
`cancelLongPress` and `getParentForAccessibility`, `Drawable.isProjected`
(all four added: `android/view/View.java.members`,
`android/graphics/drawable/Drawable.java.members`), and, not added because
no run reached them, `Typeface.Builder(FileDescriptor)`,
`TextUtils.CHAR_SEQUENCE_CREATOR` (there, but of the wrong type),
`PdfDocument.Page.getCanvas`, and members of Android 15 and 16 behind
version checks.

The other two files of members are the app's own folders on the shared
storage. `megler/ContextImpl.java.members`: `getObbDir()` is
`Android/obb/<package>` and `getExternalMediaDirs()` `Android/media/<package>`,
each made when asked for; the framework's own `Context` worked both out from
`getExternalFilesDir` and got `Android/data/<package>` (the parent of the
files folder) for the first and the files folder itself for the second.
`android/content/ContextWrapper.java.members`: a wrapper passes these, and
`getExternalCacheDir(s)` and `getExternalFilesDirs`, on to the context it
wraps; an Activity is a wrapper, and asked of an Activity they never reached
the context that knows. And `registerReceiver` with flags, a permission or a
handler: the framework's own wrapper passed on only the two-argument form, and
`Context`'s defaults for the others call `this.registerReceiver(r, f)`, which
reaches a subclass of the wrapper that overrides that form. Chromium's
wrapper makes it throw so that every caller uses the others, and Brave
1.97's `SmsProviderGms` ended the app with a `RuntimeException`; each form now
goes to the base, as on Android. The data app's step "wrapper-receiver"
registers through such a wrapper in all three forms.

`megler/AppRunner.java.members`, like those of `megler/Native`, `Gfx` and
`EGLNative` (each described with what uses it), adds to one of Megler's own
classes: `meglerBase()`, the activity at the bottom of the stack, which
`ActivityManager` (below) gives as the root of the app's task; the framework
as it came had only the top.

`android/os/MessageQueue.java.members` adds `next()`, Android's own name
for what `Looper.loop` takes the next message with (it calls the framework's own
`next(boolean main)` as `loop` does). It is hidden API, and code that runs its
own event loop takes the Java looper's messages with it: Gecko asks for it by
JNI on its own thread, and without it ended on a `NoSuchMethodError`
(`MOZ_CRASH()`) as it started. The data app's step "queue-next" sends a message
on a looper thread of its own and takes it back with `next()`.

- `android/webkit/MimeTypeMap.java`: the framework's own file hands its table to
  `libcore.content.type.MimeMap`, an Android 10 class; Android 9's `MimeUtils`
  has a table of its own that nothing can add to.
- `android/widget/AbsListView.java`, one statement: `onAttachedToWindow` asks
  for a layout after it has marked the list's data as changed. A list that is
  attached to a window again sets that mark, as Android's does, and while it
  is set a touch finds no row; only laying the list out clears it. On Android
  the layout follows by itself (a window's first traversal announces the
  touch mode, and the list lays itself out on hearing it); this framework
  has no such announcement. And it meets the case far more often than
  Android does: it takes a stopped activity's window away and puts it back
  on resume, where Android leaves a stopped activity's views attached. So
  every list was deaf to touches once its activity had been covered by
  another and come back. And one field, `mIsChildViewEnabled`: Android's own,
  not public, which AppCompat's `DropDownListView` (every popup menu and
  spinner of an AppCompat app) takes by reflection; without it each such list
  logged a `NoSuchFieldException` with its stack. And the rows a layout puts
  back: `ListView`'s and `GridView`'s `layoutChildren` scrapped every row and
  filled the list again through the adapter's `getView`, so every layout of
  the window bound every row again. Android keeps the rows of a layout whose
  data has not changed as "active views" and puts each back at its position
  without asking the adapter; here `addScrapView` keeps them so while
  `layoutChildren` runs from `onLayout` and the data is unchanged, and
  `obtainView` gives them back. The first numbers of the frame meter on the
  phone found it: Apktool M laid out in every frame, 30 to 150 ms of measure
  and layout, the main thread never idle and the heap growing by about 5 MB a
  second until the process ran out of memory after three and a half minutes.
  Its file list starts a task in `getView` that sets the row's size text when
  it ends; the text asked for a layout, the layout bound every row again, and
  every row started its task again. Under qemu, after its licence, the meter
  said "measure and layout 15.3" per frame and "3 more" MB of objects every
  five seconds before; 0.0 and 0 after. The demos app checks that a layout of
  its list with the data unchanged asks the adapter for no row.
- `android/os/Message.java`, seven methods that were signatures answering
  null or nothing: `obtain(Message)`, `copyFrom`, `getWhen`, `peekData`,
  `setCallback`, `setTarget`, `setWhat`. The first is the one that was felt.
  The support library's dialog (`androidx.appcompat.app.AlertController`, and
  the older `android.support.v7` one) keeps a message for each button and on
  a press sends a copy of it, `Message.obtain(mButtonPositiveMessage)`, then a
  second message that closes the dialog. The copy was null, so nothing was
  sent: every button of every such dialog closed it and did nothing else.
  (The platform's own `AlertDialog`, which the demos app uses, is the
  framework's own and calls its listeners directly.) Found with the runtime's
  trace and the profile of a run: of the two listeners neither had run.
- `android/os/WorkSource.java`, Android 14's in place of the generated stub,
  which kept no uids, wrote nothing into a parcel and had a null `CREATOR`. A
  location request of Google Play services' carries one: the client library
  in the app writes it into the transaction with its `writeToParcel`, inside a
  field whose size it patches in after (`SafeParcelWriter`), and Google Play
  services reads it back with the `CREATOR` -- null for a field of no size.
  Every `FusedLocationProviderClient` request then ended in Google Play
  services with a `NullPointerException` in `ImmutableWorkSource`'s
  constructor ("RegisterLocationCallback", "RegisterLocationListener" in its
  trace), and the app had no fix while Megler's own `LocationManager` had one:
  Transit, and the Google Maps "my location" layer (0.1.0.671). The data app's
  step "work-source" writes one so and reads it back.
- `android/app/PendingIntent.java`: its sends with an `OnFinished`, a
  required permission or options -- `send`'s six- and seven-argument forms,
  `send(Bundle)` and `sendAndReturnResult` -- were generated signatures that
  sent nothing. They send as the others do (a broadcast to the receivers that
  hold the permission) and then tell the `OnFinished`, on its `Handler`'s
  thread, or without one on a thread of its own once the send has returned
  (Android's binder thread), with the intent sent and Android's 0. Google Play
  services sends its `PendingIntent`s so (`bpep.d`): its network location's
  fixes to its fused provider were lost on the way, and with them every fix
  of Google's Wi-Fi positioning ("Wi-Fi, and Google's network location"
  below). The data app's step "pending-intent-options" sends one with an
  `OnFinished` on a `Handler` of its own and options, and one with a
  permission no receiver holds: each is told, the first heard.
- `android/location/LocationRequest.java`, Android 14's in place of the
  generated stub: its `Builder(intervalMillis)` forgot the interval, its
  factories made a request of nothing, `getMaxUpdates` said 0 where Android
  says "any number" (`Integer.MAX_VALUE`), and it wrote nothing into a parcel.
  Google Play services' location engine in the app asks for the passive
  provider with `createFromDeprecatedProvider("passive", ...)`, which came out
  a request of the fused provider every `Long.MAX_VALUE` ms that was never
  handed a fix. `megler/LocationService.java` hands a request of the passive
  interval every new fix without it setting how often the location is
  followed, and `LocationManager` takes Android's "any number" of updates for
  its own 0. The data app's step "location-request" builds requests, makes
  one of the deprecated provider, puts one through a parcel, and has one of
  the passive interval handed a fix.
- `android/location/Geocoder.java` and `Address.java` in place of the
  generated stubs, and `GeocoderParams.java`, `IGeocodeProvider.java`,
  `IGeocodeListener.java` and `com/android/location/provider/GeocodeProvider.java`,
  which the framework did not have: Android's Geocoder over the geocode
  provider of the Google Play services the app hosts (docs/MICROG.md,
  "Android's Geocoder"). `isPresent()` was false and every request an empty
  list, and an `Address` kept no lines and never a latitude: Transit named a
  place it was asked about by its coordinates ("Map location" and them). The
  synchronous forms and Android 13's listener forms, the provider's error as
  an `IOException` or `onError`, Android's argument checks; an `Address` with
  its lines numbered as Android's, its coordinates thrown for when none was
  set, and its `Parcel` layout. The data app's steps "geocoder" (no provider:
  not present, no addresses and no error, on the main thread and through the
  listener off it) and "address", and `gms-host` with a stand-in provider of
  Google Play services' (a place at a point and of a name in the request's
  locale, an error, the listener forms, the first request a synchronous one
  on the main thread).
- `android/content/res/AssetManager.java`, `Resources.java` and
  `ResourcesImpl.java`, another package's splits: an `AssetManager` made for
  another package (`meglerForPackage`) takes its splits too, whose tables are
  merged into its own (`meglerPackageTable`) and whose files are read where
  the base has none, as an app's own splits are. Google Play services from
  Play Store are a base and splits of the phone's density and languages, and
  an app that hosted them had their base alone: the Google logo of their
  account and help screens, of densities' values alone, was not there, and Play
  Store ended on `Resources$NotFoundException` (0.1.0.685, docs/MICROG.md,
  "Google's own Play services from Play Store: their splits"). `gms-host`
  hosts a stand-in of theirs built as such a bundle.
- `megler/AppRunner.java`, an activity whose `onCreate` throws: `launch`
  takes it off the stack at once, marks it finished, closes any window it
  opened and resumes the one below; it is never stopped or destroyed. It
  stayed on the stack, the one below paused under it, and once it had
  finished itself it was stopped and destroyed at the next idle: Google Play
  services' help screen, whose layout failed so in Play Store (0.1.0.686),
  ended in their lifecycle's "State must be at least 'CREATED' to be moved
  to 'DESTROYED'" out of the idle handler. One of the app's own still ends
  the app, as the exception goes on. `gms-host` starts one of their
  stand-in's that throws and is resumed after it.
- `android/content/ContentProviderClient.java`: `openFile`, `openAssetFile`,
  `openTypedAssetFile(Descriptor)`, `query` with a `Bundle`, `insert`,
  `update` and `delete` with extras, `bulkInsert`, `call` with an authority,
  `getStreamTypes`, `canonicalize`, `uncanonicalize` and `refresh` go to the
  provider, where they were generated signatures answering null, 0 or false.
  Google Play services' DynamiteLoader reads its modules' Chimera
  configuration so (`content://com.google.android.gms.chimera/api/<n>`), and
  gave up on each module it loads that way: "Failed to create module
  context. java.io.IOException: Config file descriptor is null" -- the Google
  Maps module's own Google certificates among them (0.1.0.671). The data
  app's step "provider-client-file" reads its provider's file through a
  client.
- `android/graphics/Bitmap.java`, `writeToParcel` and `CREATOR`: a bitmap
  goes into a Parcel and the reader gets one of its own, the same pixels,
  size, config, density and mutability, which stays when the writer recycles
  its own, as on Android -- here the copy itself in the parcel's table of the
  process's objects (`Parcel.writeValue`), not its bytes. `CREATOR` was null
  and `writeToParcel` the interface's, which writes nothing. Google Play
  services' maps client in an app writes a marker's picture so
  (`BitmapDescriptorFactory.fromBitmap`: an int, then `writeToParcel`) and
  its maps module reads it with `Bitmap.CREATOR`
  (`IBitmapDescriptorFactoryDelegate`, transaction 6): Transit ended with a
  `NullPointerException` ("Parcelable$Creator.createFromParcel ... on a null
  object reference" in the module's `m6.ev.a`) as it put a bus route on its
  map (0.1.0.675). The data app's step "bitmap-parcel" writes two so and
  reads them back.
- `android/view/ViewPropertyAnimator.java`, as Android's in what it tells
  and what it cancels. The properties asked for since a start get a
  `ValueAnimator` of their own; a property asked for again is taken from the
  animator that had it there and then, and that animator is cancelled once
  it has none left, while animators of other properties run on beside the
  new one -- the framework's own ran one animation at a time and cancelled it
  when the next started. The listener and the update listener told are the
  ones the `ViewPropertyAnimator` has when an event comes, whichever animator
  it comes from, so a listener taken off in `onAnimationCancel` does not hear
  the `onAnimationEnd` that follows -- the framework's own told the ones set
  when the animation started. `withEndAction`'s runnable is not run for an
  animation cancelled, and `start()` starts even with nothing asked for, as
  Android's does. Spotify's carousel animator (through
  `ViewPropertyAnimatorCompat`) takes its listener off when an item's fade is
  cancelled so as to finish the item once; here the end that followed was
  told to it too, and the item was finished twice (RecyclerView's listener
  run twice for one fade). The data app's step "animator-listener" checks
  each of these.
- `android/text/method/ReplacementTransformationMethod.java` (the base of
  `SingleLineTransformationMethod` and `HideReturnsTransformationMethod`),
  Android's: text with none of the characters to replace is shown as it is,
  and text with some keeps its spans -- a copy for text that cannot change, a
  view that follows it for a `Spannable` or an `Editable`. The framework's own
  gave a `String` of the text in every case, so a TextView that is
  `android:singleLine` drew its text with no span at all. Transit 6.3.7 leaves
  the markup its native code writes in a search result's title
  (`<color|1e212d/f0f0f1>Choose on map`: a light theme's colour and a dark
  one's) and hides it under a `ReplacementSpan` of no width, the rest
  coloured by a span of its own (`com.thetransitapp.droid.shared.util.e.d`);
  its result cards' titles are single-line (`search_actionable_card_cell`),
  and on the phone they read `<color|1e212d/f0f0f1>Ch` in the view's own
  colour (0.1.0.684, a Lumia 650). The data app's step "single-line-spans"
  lays such a title out from a layout of its own
  (`res/layout/markup_row.xml`) and draws it: the words alone, in the span's
  colour; and checks what the transformation gives a `SpannableString` with
  no line break (itself), one with line breaks (its spans kept), a
  `SpannableStringBuilder` (a view that follows its edits) and a `String`.
- `android/app/usage/StorageStatsManager.java`, two methods in their two
  forms: `getTotalBytes` and `getFreeBytes` were signatures answering 0.
  Since Android 8 this is where an app asks how large the phone's storage is
  (`getTotalBytes(StorageManager.UUID_DEFAULT)`), with `StatFs` beside it for
  what is free. Device Info's card read "Internal Storage -2147483648%, Free:
  4.0 GB, Total: 0.0 GB" on the phone: a free space divided by a total of
  nothing. Both answer for the file system the app's data is on: the total as
  Android gives it, rounded up to a size storage is sold in (1, 2, 4 ... 512
  of the next decimal unit, `FileUtils.roundStorageSize`), the free space as
  it is. The figures are the C library's `statvfs` under `java.io.File`.
  And the `queryStatsFor*` methods, which answered null: they answer a
  `StorageStats` with nothing counted. Firefox 152 times `queryStatsForUid` for
  its telemetry, Kotlin checks that the result is not null, and the
  `NullPointerException` on its worker thread ended the app. The data app's
  step "storage-stats" asks for its own.
- `android/os/storage/StorageManager.java`, the old way to list the storage:
  `getVolumeList`, `getVolumePaths`, `getVolumeState` and the static
  `getVolumeList(userId, flags)` were signatures answering null. They are
  hidden on Android and file managers reach them by reflection, because
  `getStorageVolumes` did not exist before Android 7. There is one volume,
  the shared storage, which the framework's own `StorageVolume` describes; a path
  that is no volume is "unknown". And `UUID_DEFAULT` is Android's constant
  (it was null), which is what `getUuidForPath` answers.
- `android/os/Environment.java`: `getDataDirectory()` is `/data`, as on
  Android; the framework's own file answered the app's own folder, so that the documented
  `new File(getDataDirectory(), "data/" + packageName + ...)` named that
  folder twice over. `getDownloadCacheDirectory()` is `/data/cache`. And four
  generated signatures answer what the hand-written ones beside them answer:
  `getStorageState(File)`, `isExternalStorageEmulated(File)`,
  `isExternalStorageManager(File)`, `isStandardDirectory`.
- `android/graphics/drawable/GradientDrawable.java`, two statements in
  `draw`: a shape with neither a solid colour nor a gradient is filled all
  the same when it is tinted or has a colour filter. Android's `draw` says so
  in one line (`if (colorFilter != null && st.mSolidColors == null)
  mFillPaint.setColor(mAlpha << 24)`): filled with opaque black, which the
  filter turns into the tint. A `<shape>` that gives only its corners is the
  usual way to say "this outline, in whatever colour I am tinted", and
  Material's `TabLayout` tints its `tabIndicator` with `tabIndicatorColor`
  exactly so. The framework's own file filled only a solid or a gradient, and such a
  shape drew nothing: Device Info's indicator is a rounded rectangle in the
  theme's primary colour behind the selected tab, whose label is white to be
  read on it, and without it "Dashboard" was white on near-white.
- `megler/Broadcasts.java` and `android/os/BatteryManager.java`, the battery:
  both had a full battery written in, the intent with four extras (no
  voltage, temperature, health or technology) and never delivered to the
  receiver it was registered with. Both ask the core now
  (`megler.Native.battery()`, two natives added by `megler/Native.java.members`,
  which are `mg_battery_read`), the intent has Android's extras and is
  delivered, and `registerReceiver(null, filter)` no longer leaves a null in
  the list of receivers. Device Info's card read "Battery 0%, Voltage: 0 mV,
  Temperature: 0C" on the phone because it shows what its receiver is told.
  And `Broadcasts.batteryChanged`, which the driver calls when the battery
  has become another, sends `ACTION_BATTERY_CHANGED` again and the charger's
  coming or going ("The battery", above).
  (Counted as one of the framework's own faults: the battery nobody could set.)
  And the network's sticky `CONNECTIVITY_ACTION`, which the framework as it
  came did not keep: a receiver registered for it heard nothing, and Asphalt 9's native
  code, told the connection's type only from there, said there was no
  network ("The battery", above). (Counted as one of the framework's own faults.)
  And `Broadcasts.send`, who hears a broadcast. An intent that names a
  component goes to that component alone, as on Android; the framework as it came gave
  it as well to every receiver registered for its action and to every
  receiver of the manifest whose filter lists it. Spotify's
  `WidgetUpdateCoordinator` receives its own `ACTION_USER_LOGIN`,
  `ACTION_USER_LOGOUT` and `ACTION_END_SESSION` and passes each to its
  widget providers by an intent with the same action that names the
  provider. That intent came back to the coordinator, which passed it on
  again, without end. Spotify's main thread spent its time on the
  deliveries and on its queue's lock, which the delivering thread held
  thousands of times a second: on the phone one window of 63 seconds drew
  one frame, 38 seconds of it waiting to enter monitors. An implicit intent
  now goes to the manifest's receivers whose filters take it as Android
  matches a filter (`megler.Manifest.match`, the framework's own, which
  services already used). A receiver the manifest disables, or that the app
  disabled with `setComponentEnabledSetting`, receives nothing; Spotify
  disables two of its widget providers. The data app's step
  "broadcast-explicit" has a manifest receiver that relays to another by
  name, as Spotify's does, and a disabled one: each is heard once, the
  disabled one never. With the framework's own delivery the relay ran until the
  step's bound of 50. (Counted as one of the framework's own faults.)
- `android/widget/Switch.java`, `onMeasure` and the first branch of `onDraw`:
  a switch with a thumb and a track drawable is as wide as two thumbs and the
  track's padding, and as high as the taller of the two, and the thumb moves
  inside the track's padding -- Android's sums. The framework's own file took the track
  drawable's own width, and Android's track is a nine-patch a few pixels wide
  that is meant to be stretched: a switch measured 13 pixels wide at 280 dpi,
  and its thumb, drawn at its own size from the left of those 13, hung out
  over the edge. In a preference row the switch is the last thing, so it hung
  off the screen: Apktool M's preference screens showed a sliver of each
  (half of each on the phone). They are 70 x 47 now, off and on as Material
  draws them.
- `megler/TextRuns.java`, one statement of `draw`: a span that changes the
  text's measure is applied to the paint once when the text is drawn. A
  `MetricAffectingSpan` has `updateMeasureState` for measuring and
  `updateDrawState` for drawing, and they do the same thing to a paint;
  Android's `TextLine` calls the one or the other. The framework's own `draw`
  called both. For a span that sets something that is harmless; a
  `RelativeSizeSpan` multiplied the size twice, a `ScaleXSpan` likewise, a
  superscript rose twice as far. Lines were broken for one size and drawn in
  another: the heading of Apktool M's licence (`Html.fromHtml` makes an
  `<h1>` a `RelativeSizeSpan(1.5f)` and bold) was laid out at 1.5 times the
  text's size and drawn at 2.25 times, every line running out of the dialog
  on the right. Bold had nothing to do with it: the stand-in, whose bold is
  as wide as its regular, cut the heading off exactly as the phone did.
- `android/widget/ListPopupWindow.java`, one statement of `show`: a popup
  that wraps its content is as high as its rows and, around them, what the
  list and the popup's background keep clear (Android's `buildDropDown`:
  "listContent + otherHeights"). The framework's own file made the window as high as the
  rows alone. The platform's popup background is a nine-patch with a shadow
  around the panel, 14 pixels above and below at 280 dpi, and this class
  gives its list 8 dp of padding at either end: QuickShortcutMaker's overflow
  menu, two rows of 84 pixels, got a panel 140 high.
- `android/appwidget/AppWidgetHostView.java`: `getDefaultPaddingForWidget`
  answered null, and every launcher made from Launcher3 reads `.left` of it in
  its `DeviceProfile`'s constructor: Super S22 Launcher ended on its first
  screen. Android answers framework-res's `default_app_widget_padding_*`,
  which are looked up here by name (the framework's own `megler.IR` has a later
  Android's numbers for them, not Android 9's). And four more signatures of
  the class that answered nothing: two constructors that gave `FrameLayout`
  no Context, `generateLayoutParams(Context, AttributeSet)`, and
  `dispatchDraw`, which drew none of the view's children.
- `android/content/pm/LauncherApps.java` and `LauncherActivityInfo.java`
  (one fault, two files): since Android 5 a launcher lists the apps it shows
  by `LauncherApps.getActivityList`, which answered an empty list, and a
  `LauncherActivityInfo` answered nothing at all. Super S22 Launcher found its
  own activity through `PackageManager`, looked it up there for its title,
  found none, and the thread that loads the home screen ended on a null
  title. Now the list is the activities with MAIN and LAUNCHER that the
  framework's `PackageManager` resolves (the app's own; it knows no other),
  each with its label, icon, component and application, and
  `resolveActivity`, `isPackageEnabled`, `isActivityEnabled`,
  `getApplicationInfo` and `startMainActivity` answer from the same. The
  service is made by the constructor with the fewest arguments (that is how
  `megler.ContextImpl` makes a service it has no class for), and the framework's
  own one without arguments passed a null Context: it takes the application's.
- `android/view/SurfaceView.java`, three statements: the view clears its area
  of the window -- the hole through which a surface under the window shows --
  only once there is something under it, as Android's does (`mDrawFinished`,
  set at the surface's first frame). The framework's own file cleared from the start for
  every `GLSurfaceView`, and for any `SurfaceView` without a frame yet.
  AIDA64's main screen has a `GLSurfaceView` over its whole content area,
  only to ask the GPU its name; its renderer never starts here, and the list
  of pages behind it -- laid out, answering taps -- was cleared from the
  window. Here the hole is made when an EGL surface is made on the view, the
  first moment the framework knows of, and never for a software surface,
  whose frames are drawn where it stands. And the surface has its size when
  `surfaceCreated` is called, as on Android: `lockCanvas` there gave null.
  And for the window the core composes (OpenGL ES from Java): the view says
  where its hole is each time it draws it (`EGLNative.place`) and that its
  surface is gone (`EGLNative.forget`, where the framework as it came hid every view's),
  the latter for every surface, EGL or not: native code's windows of it
  (`ANativeWindow_fromSurface`) are no longer valid from then on.
- `android/opengl/GLSurfaceView.java`: the renderer runs on the layer's GL
  thread as in the framework's own file, and what that thread does each turn this class
  decides (`meglerFrame`): the surface's size from `surfaceChanged`, its going,
  `onPause` and `onResume` with `setPreserveEGLContextOnPause`, the view
  leaving its window ending the thread; every change wakes the thread, which
  otherwise waits; the configuration `onSurfaceCreated` is given is what the
  app's chooser chose, the `GL10` what its `GLWrapper` made. The framework's
  own render thread drew the views as well and had none of that.
- `javax/microedition/khronos/egl/MeglerEGL.java`: on a GL thread the real
  context and surface (so another thread can share the context); window
  surfaces made with their view; nine configurations (8888, 888, 565, each
  with depth 24 and stencil 8, depth 16, neither) in EGL's order, all one
  underneath, so that a chooser that wants an exact match finds one.
- `android/content/pm/ConfigurationInfo.java`, one statement: the device's
  OpenGL ES version is what a context can be made of here (3.0, or 2.0 at
  Direct3D's feature level 9_3), not 3.2.
- `megler/EGLNative.java.members`: `createWindowSurface`, `place`, `forget`,
  `glesVersion` (core/src/drivers/mg_java_gl.c).
- `android/os/Build.java` and `SystemProperties.java` (one fault, two files):
  the processor is 32-bit ARM's -- `CPU_ABI` armeabi-v7a, `CPU_ABI2`
  armeabi, `SUPPORTED_ABIS` and `SUPPORTED_32_BIT_ABIS` both, no 64-bit one,
  and `ro.product.cpu.abi`/`abilist` to match -- as the phone's is, as the
  C library's properties say, and as the driver places an app's libraries.
  The framework as it came names a 64-bit one only, and MX Player, which checks the
  processor against the decoders it carries, logged "ABIs=[arm64-v8a]".
- `android/os/Process.java` and `android/content/pm/ApplicationInfo.java`
  (one fault, two files), the same processor told two more ways:
  `Process.is64Bit()` is false, where the framework's own file, written for 64-bit iOS,
  said true; and the app's own `ApplicationInfo.primaryCpuAbi`, hidden API,
  is armeabi-v7a when the app has native libraries in its
  `nativeLibraryDir`, null when it has none, as the package manager records
  it at install. Facebook's SoLoader (WhatsApp, React Native apps) reads
  `primaryCpuAbi` by reflection and, finding null, keeps of
  `Build.SUPPORTED_ABIS` those of the process's width, which it asks
  `Process.is64Bit`: told 64 bits it kept nothing of the two 32-bit ones,
  and WhatsApp 2.26 ended at its start with "No supported ABIs found on this
  device" (package 270). The natives app's step "abi" asks as SoLoader does.
- `android/content/res/ResTable.java`, two methods: a resource is found by
  its name without indexing the whole package ("What an AlertDialog cost").
- `android/content/res/Resources.java`, the class `Bag`: a style's items are
  found by hashing (the same section). And one statement in `updateCfg`:
  resources are chosen for the API level of the platform's resources, 28
  (`PLATFORM_RESOURCES_SDK`), not for `Build.VERSION.SDK_INT`, 34. This one is
  not a fault of the framework's own but Megler's circumstance: the framework says
  it is Android 14, and the platform's resources here are Android 9's because
  libcore is. An app's `-v31` resources refer to Android 12's system colours
  by number, and under those numbers Android 9 has private resources of its
  own; Device Info recursed 6095 frames deep through a circle made that way
  (`control_checkable_material` asks the theme, the `-v31` theme answers with
  a colour that refers to it). Choosing as a phone with Android 9 chooses is
  the only choice that fits the resources that are there. The constant must
  follow the Android root. But an app's layouts, menus and XML documents are
  chosen for 34 (`ResTable.find`, `REPORTED_TYPES`): the app's code, told 34,
  finds by id the views of the layout Android 14 chooses. Android's Settings
  library keeps its main switch bar in `layout`, `layout-v31` and `layout-v33`,
  and only the last two have a frame round the title and the switch, which
  `MainSwitchBar.setEnabled` sets enabled and activated once `SDK_INT` is 31:
  chosen at 28 the layout had no frame, and Google Play services' Location
  accuracy screen ended on a `NullPointerException` there (0.1.0.673). Of the
  platform's resources that the layouts, menus and XML of API 29 to 34 in
  Google Play services, Maps, Files, Niagara and Device Info name, every one
  is a public resource of Android 9's. And a resource of the app's that has no
  choice of 28 at all is taken from up to 34, whatever its type (it was done
  for fonts, styles and colours alone): such a layout names dimensions and
  drawables of `values-v31` and `drawable-v31` alone. The data app's step
  "layout-api-level" inflates such a layout as `MainSwitchBar` does.
- `android/content/res/Resources.java`, besides: a theme attribute whose
  value names an attribute that names another with no end -- a style's item
  that names itself, as Android's `TextAppearance` does with
  `textColorHighlight` -- is not there, as `AssetManager2`'s
  `Theme::GetAttribute` gives up after twenty steps: `obtainStyledAttributes`
  leaves it out and `resolveAttribute` says false. The framework's own file kept it as an
  unresolved attribute, which `TypedArray.getColor` throws on: GeckoView's
  `getSystemColors` applies `android.R.style.TextAppearance` to a theme and
  asks its colours, and Gecko ended on the Java exception (`MOZ_CRASH(JNI
  exception)`). The data app's step "theme-selfref" asks such a theme for its
  colours.
- `android/content/res/Resources.java`, besides: the platform's preloaded
  drawables, `sPreloadedDrawables` (a `LongSparseArray` for each layout
  direction) and `sPreloadedColorDrawables`, which Android 7 to 9 keep in
  `ResourcesImpl` and look in before a drawable's file is read. The key is
  Android's: the value's asset cookie in the high word, its data (for a file,
  the string's index in the pool) in the low. Nothing is preloaded, so they
  start empty, but apps fill them by reflection, taking the field from
  `ResourcesImpl` (from `Resources` where there is none, as there was here
  until `ResourcesImpl` below; the fields are its now). Instagram 333 swaps
  each array element for a `LongSparseArray` of its own that answers its
  vector icons, whose values (`"I1801B8"`) name no file. Without the field it
  logged "Could not enable custom drawable support" and its main activity's
  layout ended the app with "File I1801B8 not found". The data app's step
  "preloaded-drawables" does as Instagram does with a platform drawable's key,
  gets its own drawable for it, and the file's again once the element is put
  back.
- `android/content/res/ResourcesImpl.java` (a class the framework did not
  have), `Resources.java` and `android/view/DisplayAdjustments.java` (another):
  what a `Resources` is made of -- its `AssetManager`, configuration and
  metrics and the configuration its resources are chosen for, the app
  package's table, its loaders, the drawables, colours, fonts and bags made
  of them -- is a `ResourcesImpl` of its own, as on Android 7 and later, in
  Android's field `mResourcesImpl`; the hidden `getImpl()` gives it,
  `setImpl(ResourcesImpl)` gives `Resources` another (the themes they made
  are rebased onto it: `Theme.rebase` applies their styles again, where it
  did nothing), and the hidden constructor `Resources(ClassLoader)` makes
  `Resources` with none until then. Libraries make `Resources` of their own
  over the platform's so, by reflection, to answer some lookups themselves:
  Tuya Smart's dynamic strings for every activity
  (`DynamicResourceContextThemeWrapper.getResources`), and the app ended at
  its first activity on `NoSuchMethodException: getImpl []` (0.1.0.673).
  The screen's changes now reach each implementation once
  (`ResourcesImpl.meglerScreenChanged`), as Android's `ResourcesManager`
  applies a configuration to every `ResourcesImpl`. The data app's step
  "resources-impl" makes such `Resources` as Tuya does, and one of the
  public constructor that answers a string itself, and finds them alike.
- `android/graphics/drawable/RippleDrawable.java` and `megler/ResInflate.java`,
  a method and a statement: a ripple inflated from XML is told that it has a
  mask. Android's commonest ripple is nothing but a mask (the background of
  every list row and of everything that is `selectableItemBackground`), and
  the mask is what keeps it inside the view. The inflater left the mask layer
  out of what is drawn, rightly, and never told the drawable; with no mask
  and no layers the drawable took itself for a borderless one and drew its
  highlight as a disc of half its longer side: for a pressed row of a list, a
  grey disc as wide as the screen over the rows above and below.
- `android/graphics/drawable/InsetDrawable.java` and `megler/ResInflate.java`:
  an inset may be a fraction of the drawable's bounds as well as pixels, as
  Android 8 and later have it (`InsetDrawable.InsetValue`): an `<inset>`'s
  `android:inset="10%"` is read as a fraction, its type looked at first, as
  `InsetDrawable.getInset` does, and the bounds, the intrinsic size and the
  padding are Android's sums of both. The inflater read every inset with
  `getDimensionPixelOffset`, which throws for a fraction on Android too, and
  `InsetDrawable(Drawable, float)` made one with no inset. WhatsApp's adaptive
  icon is inset by 0.01% round its foreground, and its Appearance screen
  (Settings, Chats, Theme) ended the app: "Can't convert value at index 6 to
  dimension: type=0x6" (0.1.0.671). The data app's step "inset-fraction"
  inflates such insets and measures what they give.
- `megler/ResInflate.java`, a few lines: an animator's XML is read as
  Android's `AnimatorInflater` reads it, and an element that is no
  animator's -- a view animation's `<alpha>`, `<translate>`, `<scale>` or
  `<rotate>`, alone or in a `<set>` -- is "Unknown animator name", thrown.
  The inflater made a `ValueAnimator` of anything. AndroidX's fragments try
  an animation resource as an animator first when its type is not named
  "anim", and take the view animation when that throws; Facebook's apps name
  the type of theirs "anim.2", so Messenger's fragments came in by an
  animator of 200 ms that animated nothing, and a fragment is resumed only
  once its animator ends, where a view animation lets it be resumed at once.
  The step of Messenger's first-run screen that asks to import the contacts
  opens its Bloks screen as it is made and learns it did in its `onPause`;
  paused by that screen before its animator was over, it was never resumed,
  so never paused, and on coming back it did not move on: the screen stayed
  until Back (0.1.0.673). The data app's step "animator-not-animation"
  inflates both kinds both ways.

- `android/app/Activity.java`, two methods: `initWindowDecorActionBar` makes
  the window's decor before it asks whether the window has an action bar, as
  Android's does. Whether there is a bar is the theme's to say, and the
  window reads its theme when its decor is made. Without this
  `getActionBar()` was null for an activity that asks for its bar before
  `setContentView` -- the usual order with ActionBarSherlock, whose
  `getSupportActionBar()` wraps `Activity.getActionBar()` on Android 4 and
  later. QuickShortcutMaker's About screen ended the app with a
  `NullPointerException` on it.
  And, a fault of the framework's own too, `getWindow` gives the window the manifest's
  `android:windowSoftInputMode`, which the framework's own file read and never used.
  And an activity that starts one of the app's activities for a result
  before its window is on the screen (in `onCreate`, `onStart` or
  `onResume`) leaves its window off the screen until it resumes again with
  the result, as `ActivityThread.handleResumeActivity` does
  (`mStartedActivity`, "willBeVisible"); `meglerVisible`, which `AppRunner`
  asks whether the activity below must be started again, says whether it was
  started and not stopped. The framework's own file showed the window after every
  `onResume`, so the window had the focus and lost it to the activity it had
  started, which Android never gives it. Asphalt 9's `MainActivity` starts
  its data installer from `onResume`, makes its native game only in
  `onActivityResult`, and calls that game from `onWindowFocusChanged`: it
  read at 0x34 and ended (package 262). The crash app's first activity starts
  one for a result from `onResume` and says what it heard, in order, once it
  has the focus: the result, then the focus, and nothing before. Not done:
  when the activity started for a result is floating (a dialog's theme),
  Android shows the one under it too; here its window waits as well, since
  a window added later would lie above the dialog.
  And the managed dialogs: `showDialog` makes a dialog once by
  `onCreateDialog(id, args)`, keeps it by its id, and prepares
  (`onPrepareDialog`) and shows it at each call; `dismissDialog` dismisses it
  (and throws for an id never shown, as Android does) and `removeDialog`
  forgets it. They did nothing: WhatsApp asks whether the phone number is
  right with `showDialog(21)` when one taps Next, and nothing came of it
  (package 479). The data app's "managed-dialog" step makes, shows again,
  dismisses, removes and makes again. `onDestroy` dismisses them, as
  Android's does.
  And the activity's other windows, its dialogs and popups (application
  windows and sub-windows; not system windows such as a toast, which outlive
  it): hidden while it is stopped and shown again when its own window is
  (`megler.ViewRoot.ownerStopped`, from `meglerStop` and `makeVisible`) --
  not drawn, no touches, keys or focus, still attached, told
  `onWindowVisibilityChanged` -- and removed once it is destroyed, with a
  line in the log (`ViewRoot.closeAll`, as `WindowManagerGlobal.closeAll`
  removes a destroyed activity's "leaked" windows). They were drawn all the
  while: WhatsApp's progress dialog of its phone number screen stayed in the
  list of windows after that screen had gone, and showed through the screens
  after it (package 481). The data app's "dialog-of-stopped" step hides a dialog,
  shows it again and removes it.
- `megler/AppRunner.java`, `startActivity` with `FLAG_ACTIVITY_CLEAR_TOP` for
  an activity already in the stack, as Android does it. The activities above
  it go. Then, when its launch mode is standard and the intent has no
  `FLAG_ACTIVITY_SINGLE_TOP`, it goes too and the activity is started again
  with the new intent; otherwise it is brought back (`onRestart` and
  `onStart` when it was stopped) and hears `onNewIntent`. It is found by its
  class, as Android compares `realActivity`, so an activity started through
  an alias is found by an intent that names the activity. The framework's own file kept
  the old activity whatever its mode, gave it the intent by `setIntent`
  alone, and called its `onActivityResult(-1, RESULT_CANCELED, null)`, which
  Android never does. Instagram 333's signed-out screen, brought back so after
  it had downloaded its Russian strings, finished on that, and with no
  activity left the app ended with status 0. The crash app's first activity,
  started through an alias, goes through both cases with `Top` before it
  starts the activity that throws: a new first activity for the flag alone,
  `onNewIntent` with `FLAG_ACTIVITY_SINGLE_TOP`, and no result it did not ask
  for. An activity that is finishing is passed over, as Android passes over
  finishing records, and one finishing below a finished top one is not brought
  back. And `Activity.isTaskRoot` is Android's: the lowest activity of the
  stack that is not finishing (it was "started by none"). Instagram's launcher
  activity starts its main activity and finishes; the main activity, not the
  root by the old rule, finished in its `onCreate` ("is not the root"), and the
  launcher's second start with `FLAG_ACTIVITY_CLEAR_TOP` brought that finishing
  one back (`onRestart`, `onStart`), which failed for the state its `onCreate`
  never made ("Required value was null", package 561). The crash app's
  `Trampoline`, started as First finishes, is the root, and its `Main`, started
  twice so, finishes in `onCreate` and is never started.
  An activity that finishes and gives its place back to the one below is
  hidden at once (its windows not drawn and given no input,
  `ViewRoot.finishedHidden`), and stopped and destroyed only after the next
  frame, when the main thread is idle, ten seconds at most, as Android stops
  a finishing activity once the next one is visible and its thread reports
  idle. It was destroyed in the message that brought the one below back, and
  what it had begun as it finished was cut off: Pyaterochka's sign-in saves
  the address it was sent back to through DataStore from a coroutine of its
  `lifecycleScope` and finishes, and the edit, which DataStore runs on the
  main thread a message later, never ran ("No responseUri present", 576).
  The crash app's `Gate` posts work from its `onPause` as it finishes, which
  finds it not destroyed.
  And an intent for the phone's home screen, `ACTION_MAIN` with
  `CATEGORY_HOME` (an app sending itself home), sends the app to the back as
  `moveTaskToBack` does (`AppRunner.isHome`): the launcher coming in front
  on Android is Windows' Start coming in front here, and nothing of the app
  finishes. There was no home: the intent found the app's own launcher
  activity, because the manifest's filters were matched without their
  categories (`megler.Manifest.matches` now wants every category of the
  intent in the filter, as `IntentFilter.matchCategories`). Facebook's Back
  on Facebook Lite's sign-in screen and on Messenger's first-run screen goes
  home through its launcher of activities of other signatures
  (`DifferentKeyIntentScope`); it removed that one as its own, found nothing
  else ("No matching different-signature components", twice), and went on as
  without a home: Facebook Lite ended its process at its sign-in screen's
  Back (status 0, three runs of a user's on 673), and Messenger finished its
  first-run screen (`NeueNuxActivity`), its main activity below finished as
  well, and the app ended. The hello app's third Back goes home so (none of
  its own activities is the home) and the app goes to the back; the data
  app's step "intent-categories" resolves the home, the launcher's intent and
  an unknown category.
  And `Activity.finishAffinity` finishes the caller and the activities under
  it of its task affinity (`android:taskAffinity`, now read with the manifest:
  the application's, the package's unless it names another, none for an
  empty one) together, as `ActivityStack.finishActivityAffinityLocked`: all
  are finishing before the first goes, so none is resumed on the way, and the
  first one down of another affinity stays and comes back if the top went.
  They were finished one by one, each the top in its turn, and the one under
  each was resumed before it went too. Messenger's first-run screen finishes
  so on Back when it finds no home: its main activity, resumed for that
  moment, asked for the first-run screen again (Messenger looks for a pending
  first-run at every resume) and finished, and the screen it asked for
  finished in `onCreate`, the first not destroyed yet (673). The crash app's
  `Trampoline` starts `Away` and `AwayTop`, of an affinity of their own;
  `AwayTop` finishes it, `Away` is not resumed, and `Trampoline` comes back.

- `android/app/Activity.java`, `getCallingActivity` and `getCallingPackage`:
  who started the activity for a result, which its `setResult` answers, and
  nobody when it was started without one, as on Android; they named whoever
  started it. Android 14's `getLaunchedFromPackage` and `getLaunchedFromUid`
  name the app itself for an activity it launched, for a result or not. The
  crash app's `Gate`, started for a result, has `First` for its caller; its
  `Top`, started without one, has none.

- `android/widget/LinearLayout.java`, four statements: what a child with a
  weight and a size of 0 is given when the layout's own size is not exact.
  Android has two rules. For an app written for Android 7 or later the space
  to share is what is left plus what those children took when first
  measured, and each gets its share alone. For an older app
  (`mAllowInconsistentMeasurement`, targetSdkVersion 23 or less) the space to
  share is what is left, and each gets what it measured plus its share. The
  framework's own file shared the first rule's space by the second rule's sum, so such a
  child got what it measured twice over: the frame under a dialog's title was
  twice the height there was, appcompat's dialog laid its message out at
  full length and put the buttons below the screen, and Apktool M's first
  dialog could not be answered. Both rules are Android's now, chosen by the
  app's targetSdkVersion. They differ visibly in a row of tabs with no limit
  on its width (a tab strip in a `HorizontalScrollView`): the old rule leaves
  each tab as wide as its label needs, the new one makes them all the
  average. QuickShortcutMaker 2.5.0 says it is written for Android 10, so its
  four tabs are each a quarter of what the four labels need, and the longest
  label is cut; by Android's code that is what Android does with this APK.

- `megler/ViewRoot.java`, three calls and a field, and no fault of the
  framework's own: `doFrame` tells the driver where a frame's phases begin and how
  much of the screen it drew, for the frame meter ("The frame meter", above).
  And a window's dirty rectangle, which `drawAll` draws alone, with the tests'
  check of it ("Drawing only what changed"). And `keyFocus`, the focused
  view of the window that keys go to, for the next file's log line. And,
  this time a fault of the framework's own, what the keyboard does to a window: its
  `softInputMode` decides, as on Android ("The keyboard"). The framework's own file
  placed every dialog by gravity in the part above the keyboard without
  fitting it there, gave dialogs the keyboard as an inset, and resized every
  activity. And the frame of the insets it makes (`computeInsets`), for
  `WindowInsets.getFrame` (below): the screen's size for an activity's
  window, a dialog's or popup's own once laid out.
- `megler/DecorView.java`, one method: `dispatchApplyWindowInsets` sets the
  content stack's margins only while the stack is still the decor's own
  child with the decor's layout parameters. An app may take the decor's
  content out, put it in a view of its own and put that view in the decor
  -- the old way to have a navigation drawer over the whole window, which
  VK mp3 mod does with a `DrawerLayout` -- and Android's decor then leaves
  that content alone. The framework's own file cast the stack's parameters to
  `FrameLayout.LayoutParams` whatever they were, and VK ended at its first
  layout after signing in (package 271). The data app's step after the
  dialog fragment moves a dialog's content so and lays it out.
- `megler/InputMethods.java`: only what `deliverPending` writes in the log.
  What it does with the keyboard's text is the framework's own, unchanged
  ("The keyboard": where typed text went). This is for diagnosis; the
  framework's own file has no fault here.
- `android/view/View.java`, the invalidations and one statement of
  `drawFromParent`: each invalidation passes its rectangle up to the window,
  clipped on the way, and a child outside a partial frame's clip is not
  drawn. The framework's own file asked for the whole window each time, which the window
  then drew whole. (`megler/Gfx.java.members` adds `bmDiffer`, the tests'
  comparison of two bitmaps in C.)
  And `setScrollContainer`, which the framework's own file dropped: `isScrollContainer`
  now says what was set, which decides an unspecified `softInputMode`.
  And, with `megler/ViewRoot.java`'s `ScreenCanvas`, the size a view's canvas
  says while the view draws (a fault of the framework's own, counted as one): on
  Android a view draws into a display list of its own size (hardware
  acceleration is on for every app written for Android 4 or later), so
  `canvas.getWidth()` and `getHeight()` in an `onDraw` or a `dispatchDraw`
  are the view's. Here every view draws into the screen's one canvas, which
  said the screen's size everywhere. Now `drawFromParent` has it say the
  child's size while the child draws, and `ViewRoot.drawWindows` the
  window's while its views draw; anything else of the canvas is the
  screen's. VK mp3 mod's QR scanner frame (`Path.java` below) rounds its
  corners with a rounded rectangle of the canvas's size and draws a marker
  at each corner in a layer of that size, turned about its middle: with the
  screen's size it rounded the screen's corners, drew its markers off the
  screen and made a layer of the whole screen for every picture of the
  camera. The camera test app's frame logs the size its canvas says, which
  the test wants to be the frame's, and the test finds its bottom corners
  white.
  And now each view does draw into a display list of its own (a RenderNode's,
  which the core plays on the graphics processor), and is hardware
  accelerated as Android's are: `View.isHardwareAccelerated()` is true for a
  view attached to a window, and so is `isHardwareAccelerated()` of the canvas
  its list is recorded on. Compose then keeps each graphics layer as a
  RenderNode whose alpha, translation and scale change without its content
  being drawn again; on a canvas that said it was not, it drew every layer's
  content into its view's list, the layer's properties with it, and a fade or
  a scroll that only changed those left Spotify's screens black. A view
  without a list of its own (a WebView, a SurfaceView, a TextureView) draws
  on a canvas that says it is not (`RecordingCanvas.meglerSoftware`):
  Chromium's WebView draws its page into a bitmap only then. The flag
  `no-view-lists` turns both off. The data app's step
  "hardware-accelerated" checks the view, its list's canvas, a SurfaceView's
  and a bitmap's. (A WebView whose page is drawn through the draw functor
  says it is: "The WebView".)
  And the window's frames are drawn and shown by a render thread, as
  Android's RenderThread draws what the UI thread recorded:
  `megler.ViewRoot.drawAll` records a frame -- every window, the part of the
  screen it changes cleared -- on a recording canvas
  (`Canvas.meglerRecordFrame`), the views' lists by reference, and hands it
  over (`Gfx.frame`, `mg_java_gfx.c`); the render thread plays it onto the
  screen's canvas, which only it draws on, and has the shell show the screen,
  while the main thread goes on with its next frame. One frame is in flight:
  before the next is handed over, the main thread waits for it to be shown and
  then gives the core the nodes' properties that changed
  (`RenderNode.meglerSyncAll`; drawing on the screen did that before, and a
  recording does not). What a frame draws is kept by its list (nodes, lists,
  bitmaps, shaders are counted references), and a node's properties are read
  under the lock their setter takes. Before, the main thread drew every list
  onto the screen and waited for the shell's present itself: Spotify's spent
  26% of its time drawing lists and 62% presenting, 27 ms a frame, of which
  its own Java was 10. The log says "gfx: the render thread" every five
  seconds: frames, the time drawing the lists and showing each, and how long
  the main thread waited for it. The flag `no-render-thread` draws and shows
  each frame on the main thread.
  And `dispatchDetachedFromWindow` leaves the view's `View.animate()`
  animation running, as Android's does: the framework's own cancelled it,
  which told its listener the animation's end inside whatever took the view
  out of the window. RecyclerView's layout removes a stale child it has
  taken from its scrap with `removeDetachedView` and then unscraps it
  (androidx's `Recycler.tryGetViewHolderForPositionByDeadline`); the item
  animator's listener, told the end in between, had recycled the child
  already (`removeAnimatingView`: no longer a child, so unscrapped), and the
  unscrap met null. Spotify's Now Playing carousel, whose covers its item
  animator fades out through `View.animate()`, ended so at a track change
  (0.1.0.678: a `NullPointerException` on `p.t7y0.n(Lp/c8y0;)V`, the
  recycler's `unscrapView`, in `p.t7y0.m` under `CarouselLayoutManager`'s
  `onLayoutChildren`). The data app's step "detached-animator" does that
  with a list of its own in the window: nothing is heard inside the removal,
  the child is still in the scrap after it, and the fade goes on to its end
  and is told then, once.
  And a view hears whether it is shown, `onVisibilityAggregated`, as
  Android's (7 and later) tells it: true at its attach to the window where it
  is shown, false at its detach, and the subtree of a view whose visibility
  changes in a window through `dispatchVisibilityAggregated` (`ViewGroup.java`
  tells its visible children); View's own makes its background and
  foreground visible or not, ImageView's its picture. Nothing called it: no
  drawable heard that its view had left the window. Facebook's network
  drawables let their pictures go only at `setVisible(false)`, and Messenger's
  log had 124 lines "AsyncDrawable with url ... wasn't hidden before it was
  GC'd" (0.1.0.683); Litho's `LithoView` takes its visibility hint from it.
  The data app's step "visibility-aggregated" hides a picture and a
  background with their parent made `GONE` and taken out of the window.
- `megler/KeyStoreProvider.java`, one method, and Megler's circumstance
  rather than a fault of the framework's own: `PairGen.generateKeyPair` takes another
  provider's key pair generator from its service, because
  `KeyPairGenerator.getInstance(algorithm, provider)` refuses the system's
  BouncyCastle for RSA and EC, and BouncyCastle is what has them while
  conscrypt's native library is not in the root ("On libcore").
- `android/media/MediaCodecList.java`, one method, Megler's circumstance
  again: every decoder is listed only when `megler.MediaCodecNative.supported`
  says its type can be decoded (the framework's own file listed H.264, AAC and MP3
  always and asked only for HEVC). The list must be what `MediaCodec` can
  make, and here that is whatever the shell's decoders are
  (`megler_codec_host`: Media Foundation's on a phone, "Decoders for
  MediaCodec" below), which a phone may lack one of. With no decoder at all
  the list is empty, which Android allows: an app finds none and
  `createDecoderByType` throws `IOException`. The framework's
  `MediaCodec.createDecoderByType` still says in that message that Megler
  decodes H.264, HEVC, AAC and MP3.
- `android/media/MediaCodec.java`, one rule, and a fault of the framework's own: an
  input buffer goes back to the app (`dequeueInputBuffer`, or the
  `Callback`) only while fewer outputs wait to be released than a phone's
  decoder has buffers for (eight pictures, sixteen blocks of sound); else it
  is held until `releaseOutputBuffer` brings them below that. The framework's
  own file handed every buffer back as soon as it was decoded, so the decoder never
  filled, and ExoPlayer, which feeds a renderer for as long as it is given
  buffers and has samples (up to fifty seconds of them), would have decoded
  everything it had loaded in one turn of its loop and kept every picture
  until its time. The NDK's decoder here (`ndk/mg_media.c`) had learnt the
  same from Unity.

- `android/preference/PreferenceActivity.java`, the largest: the activity's
  content and preference headers. Android's `onCreate` sets a layout in which
  the list (`android.R.id.list`) lies three layouts deep, and apps rely on it
  (`findViewById(android.R.id.list).getParent().getParent().getParent()` is
  how a `PreferenceActivity` is given a toolbar); the framework's own file had no list
  until asked. And `loadHeadersFromResource` was empty, so a settings screen
  made of headers -- the usual kind from Android 3 to 9 -- was an empty list.
  Now the headers are read, listed in the platform's row layout, and opened
  as Android opens them on a phone (the same activity again with
  `EXTRA_SHOW_FRAGMENT`). Apktool M's Settings ended the app on the first and
  was empty on the second; it shows its seven headers and their screens.
  Not done: two panes, bread crumbs.

- `android/util/Xml.java`, the class names of `newPullParser` and
  `newSerializer`. The framework's own file asked only for Android 10's names of kXML's
  classes, `com.android.org.kxml2.io.KXmlParser` and `KXmlSerializer`.
  Android 9's libcore (`core-libart.dex`) names them `org.kxml2.io.*`, so
  every `Xml.newPullParser()` threw an `AssertionError` of a
  `ClassNotFoundException`. MX Player 3.3.1 makes one at its start and ended
  there on the phone (0.1.0.207). The overlay asks for Android 9's name first,
  then Android 10's. The data app's step "xml" writes a small document with a
  serializer and reads it back with a pull parser, both through
  `android.util.Xml`. A search of `core/framework` for other names that differ
  between Android 9 and later (`com.android.org.*`, `libcore.*` and the like,
  in `Class.forName` and string literals) found no other.

- `android/content/res/AssetFileDescriptor.java`, `getParcelFileDescriptor()`.
  What `AssetManager.openFd` and `openNonAssetFd` give (the APK's file,
  opened anew for each call, and where in it a stored entry starts) was made
  from a plain `FileDescriptor`, and `getParcelFileDescriptor()` returned
  null. On Android every `AssetFileDescriptor` has one, and code takes the
  number from it: Chromium's `ApkAssets.open` (Brave 1.97) calls
  `getParcelFileDescriptor().detachFd()` to map `icudtl.dat` and its `.pak`
  files out of the APK, and ended in a `NullPointerException` that its native
  side makes fatal. Now one is made on the first call. It owns nothing:
  closing it, or the `AssetFileDescriptor`, leaves the descriptor open, as
  `close()` always has here; whoever detaches the number may close it. The
  core had a fault on the same path: a path from the APK's root, which
  `openNonAsset` and `openNonAssetFd` pass with a leading "/", was looked for
  under `assets/` (`core/src/drivers/mg_java.c`, `asset_name`), so neither
  found what it was asked for.
  The data app's step "asset-fd" opens `resources.arsc` (stored) with
  `openNonAssetFd`, detaches the number from its `ParcelFileDescriptor`
  and reads the table's header where the entry starts.

- `android/app/ActivityManager.java`, `RecentTaskInfo`, which
  `AppTask.getTaskInfo()` returns: the fields it has from `TaskInfo` on
  Android 10 and later (`taskId`, `isRunning`, `numActivities`, `baseIntent`,
  `baseActivity`, `topActivity`, `origActivity`), filled from the activities
  as they are -- one task, id 1, as `Activity.getTaskId()` says, the first
  activity started at its root and the last on its top (`meglerBase()` above).
  The framework's own had only `id`, and a field a class lacks is a
  `NoSuchFieldError`, which nothing catches: Brave 1.97's
  `ChromeTabbedActivity.onCreate` reads `baseActivity` of its task and ended
  there under qemu. `mg_tests --dvm-link` over Brave's two DEX files names
  four fields of `RecentTaskInfo` (these), `TextUtils.CHAR_SEQUENCE_CREATOR`
  (there, of another type) and `ColorSpace.Named.OK_LAB` (Android 16) as the
  fields it names and the framework lacks. The data app's step "tasks" reads
  its own task so.

- `android/app/ActivityThread.java` and `megler/AppRunner.java`: Android's
  hidden fields that hold the process's `ActivityThread` and `Application`,
  `sCurrentActivityThread`, `mInitialApplication` and `mAllApplications`,
  which code with no `Context` reads by reflection as well as the methods.
  `AppRunner` puts the Application in them as soon as it has made it
  (`ActivityThread.meglerApplicationMade`), before its providers and its
  `onCreate`, as Android's `handleBindApplication` and
  `LoadedApk.makeApplication` put it; `currentApplication()` and
  `getApplication()` answer from `mInitialApplication`, as Android's do.
  Tuya Smart's library loader (`ThingLibraryLoader.getContext`) reads
  `currentActivityThread()`'s `mInitialApplication` on a worker thread while
  the Application starts: there was no such field (`NoSuchFieldException`
  in its log), it had no `Context`, and the thread ended on "context is
  null" (0.1.0.673). The data app's Application (`DataApplication`) asks so
  on a thread of its own in its `onCreate`, and the step
  "activity-thread-application" checks what it found and every other way.

- `android/app/DialogFragment.java`, in place of the generated stub, whose
  `show()` did nothing: `onCreateDialog` was never called and `getDialog()`
  was null. VK mp3 mod's sign-in, when VK asks for a confirmation code, shows
  `AuthCheckFragment`, a `DialogFragment` whose `onCreateDialog` builds an
  `AlertDialog` with the code's field, over `TransientAuthActivity`, whose
  window has nothing else: a black screen, and nothing more happened (the
  flag `trace-calls-...` showed `Auth.doAuth` answering 4, "open the
  browser" with type 2fa, and `open2FA` returning at once, package 261). Now
  Android 9's class over the framework's own `FragmentManager`: `show()` adds the
  fragment with no container and commits; the dialog gets the fragment's
  view if it made one, is shown in `onStart`, hidden in `onStop`, dismissed
  with the fragment's view; `dismiss()` takes the fragment away, by its back
  stack entry where `show(FragmentTransaction, String)` put it on one. Where
  the framework's `FragmentManager` differs, so does this: it never asks a
  fragment for its layout inflater, so the dialog is made in
  `onActivityCreated` (and by `onGetLayoutInflater` for whoever asks), and it
  calls `onDestroyView` only for a fragment with a view, so `onDestroy`
  dismisses a dialog still there. The data app shows one after its popup:
  `onCreateDialog` called once, the dialog showing with its field, the
  fragment found by its tag, and both gone after `dismiss()`.

- `android/view/WindowInsets.java`, `getFrame()` (Android 16): the size of
  the window the insets are relative to, which `megler/ViewRoot.java` gives the
  insets it makes. The insets carry it through copies, consuming, replacing
  and the `Builder` (whose `setFrame` the generated stub ignored); insets
  nobody gave one have 0 x 0. The framework's own generated stub returned null.
  Chromium asks every `WindowInsets` for its frame and catches the
  `NoSuchMethodError` an Android before 16 gives; here the method was there,
  returned null, and the `NullPointerException` that followed in a listener
  of `onApplyWindowInsets` ended Brave 1.97 at its first layout. The data
  app's content view logs the frame of the first insets it is given (by
  reflection: the app is built against API 29), and the test wants a size
  there (480x800 under qemu). A stub of a later API that returns null where
  the platform never does is a fault of this kind wherever an app calls it
  without asking the version first; this is the one a run has met.

- `android/view/ViewGroup.java`, one statement of the constructors' `init()`:
  the flag that says a group does not draw itself is set directly
  (`View.meglerWillNotDraw`, which the overlay's `View` adds), as Android's
  `ViewGroup` sets it, where the framework's own file called `setWillNotDraw(true)`.
  That method is public and not final; called from the constructor it runs a
  subclass's override before the subclass's own constructor has set
  anything up. Chromium's `CompositorView` (Brave 1.97) overrides it to pass
  the call on to its surfaces, made later in its constructor, and the
  browser's main activity ended inflating its layout with a
  `NullPointerException` there. The data app's step "viewgroup-flag" makes a
  `FrameLayout` whose override needs a field of its own: it is not called,
  and the group does not draw itself.

- `android/widget/TextView.java`, the cursor's and the selection handles'
  drawables (API 29): `getTextCursorDrawable` gives what the style or
  `setTextCursorDrawable` set, else a stand-in made once (2 dp wide, in the
  text's colour); `getTextSelectHandle`, `Left` and `Right` give what was set,
  else a stand-in 22 dp square; their setters keep what they are given. The
  framework's own getters returned null, which Android's never do for an
  `EditText` of a platform theme, and its setters dropped what they were
  given or kept an `Integer` the getter cast to a `Drawable`. Chromium's
  omnibox (Brave 1.97) tints all four with `mutate().setTint(...)` and the
  browser's activity ended there. Nothing draws them here; the cursor is
  drawn as before. The data app's step "text-drawables" asks an `EditText`
  for all four, tints them, and sets one.
  And `onMeasure` makes the layout again for another width where the text
  ends in an ellipsis, as Android's does when the width it ellipsizes at
  changes. A single line scrolls horizontally, so a layout wider than the
  view was kept as good enough, its ellipsis where that wider width ended. A
  `LinearLayout` not given an exact width measures a weighted child at its
  own width first and at its share after: Transit's search cards'
  single-line titles kept the layout of the first measure, and the line was
  cut at the view's edge with no ellipsis (0.1.0.684). The data app's step
  "ellipsis-narrower" measures such a title at 500 pixels and then at 200:
  the ellipsis at 200.

- `android/os/ParcelFileDescriptor.java`, what `dup` and the descriptors it
  is given own, which is now Android's: `dup(FileDescriptor)`, `dup()` and
  `fromFd(int)` make a descriptor of their own (`Os.dup`) that closing the
  result closes; `adoptFd(int)` takes the number over; `detachFd()` gives it
  away, so that closing no longer closes it. The framework's own wrapped the same
  descriptor and owned nothing. GeckoView's `startCrashHelper` (Firefox 152)
  makes two socket pairs, wraps each end with `dup`, closes the four
  originals and hands the duplicates' numbers to Gecko: with no duplicate
  those were the closed ones' -1, and Gecko stopped on `MOZ_CRASH: fd != -1`
  as it started. The data app's step "pfd-dup" duplicates a socket pair's end
  and a file's descriptor, closes the originals, writes through the
  duplicate, detaches it and adopts it again.

- `android/os/Parcel.java`, `marshall` and `unmarshall`, which carry the
  parcel's table of objects with its bytes. A parcel here keeps what
  Android would write out as bytes -- lists, maps, bundles, Parcelables,
  CharSequences -- as objects in a table, and its bytes hold their numbers
  in it. `marshall` gave the bytes alone, and a parcel unmarshalled from them
  had no table: every object read back as null. Facebook keeps an activity's
  Parcelables as their marshalled bytes
  (`com.facebook.base.activity.parcel.OpaqueParcelable`) and restores them
  with `unmarshall`, `setDataPosition(0)` and the creator: Messenger, opening
  a conversation, read null for the list its photo editor's layers had been
  written as ("ActivityParcelableHacks: Failed to restore"), and its
  composer's camera configuration ended the app ("Required value was null",
  a user's 12-minute run of 673). Now the bytes of a parcel with objects end
  with a trailer of 16 bytes (a mark, a number, the objects' count, a mark),
  the table is kept under the number (the last 16384 tables), and `unmarshall`
  takes the trailer off and gives the parcel the same objects. A parcel
  without objects marshals to its bytes alone, as before; bytes kept past the
  process find no table and read nulls, as before. The data app's step
  "parcel-marshall" writes a list and a bundle, marshals, unmarshals into
  another parcel and reads them, and marshals a parcel of data alone.

- `android/os/UserHandle.java`, the arithmetic of a uid, which is Android's:
  a uid is a user's id times `PER_USER_RANGE` plus an app's id, so
  `getAppId(uid)` is the remainder, `getUid(user, appId)` the sum, and
  `isSameApp`, `isSameUser`, `getCallingAppId`, `getCallingUserId`, `isSystem`
  and `isOwner` follow. The framework's own were generated signatures answering 0
  or false, and an app id of 0 is root's: AOSP's `WebViewFactory` refuses a
  WebView to a process whose app id is root's, the system's or the phone's,
  and refused every app ("For security reasons, WebView is not allowed in
  privileged processes"). The app's uid here is 10100, an app's. The data
  app's step "user-handle" asks its own app id by reflection.

- `android/security/NetworkSecurityPolicy.java`, whether cleartext HTTP is
  permitted, which the framework's own generated signature never permitted, to any
  host. Now as Android decides it: with `android:networkSecurityConfig`, that
  XML's `<base-config>` and, for a host, the most specific `<domain-config>`
  that names it; without one, `android:usesCleartextTraffic`; without either,
  permitted for an app written for Android 8.1 or earlier and not for a later
  one. Android's WebView asks this for every http:// request
  (`net::ERR_CLEARTEXT_NOT_PERMITTED` when it says no), so no http page loaded
  in any app. The answer comes from the configuration TLS goes by
  (`android/security/net/config`, below). The data app, written for Android 9,
  whose configuration permits it for `sub.megler.test` and its subdomains alone,
  is refused it elsewhere (its step "cleartext"); the WebView test app says
  `usesCleartextTraffic="true"` and loads its pages over http.

- `android/security/net/config/`, Android's network security configuration,
  of which the framework had `ApplicationConfig` as a signature alone: the
  app's XML read at its first use, its trust anchors, pins and cleartext for
  each host, and the provider `AndroidNSSP` whose `RootTrustManager` TLS goes
  by (`NetworkSecurityConfigProvider.java` says why; see the TLS paragraphs
  under "On libcore" above). conscrypt's classes it builds on are compile-time
  stubs in `core/framework/stubs/com/android/org/conscrypt`.

- `android/app/compat/CompatChanges.java`, new: Android 11's system API for
  whether a compatibility change is on for the app, which the WebView's glue
  asks about WebSettings' simplified dark mode (change 214741472) on Android
  13 and later; its start ended on `NoClassDefFoundError` without the class.
  A change is on unless it is enabled only from a target SDK the app does not
  reach (the changes asked about here are listed with theirs), as a change
  with neither `@Disabled` nor `@EnabledSince` is on Android. The data app's
  step "compat-change" asks for that change (off: it is written for Android 9)
  and for one the platform does not know (on).

- `android/content/res/AssetManager.java`, `Resources.java` and `ResTable.java`
  (one fault, three files), shared libraries: an APK added with
  `addAssetPathAsSharedLibrary` -- Android's WebView APK, whose resources
  and files the WebView's code finds through the app's `AssetManager` --
  where the framework's own methods were signatures answering 0 or null and every
  id that was not the platform's was looked for in the app's table. The core
  opens the APK beside the app's (`megler.Native.addApk`, two natives added by
  `megler/Native.java.members`); its table's package, id 0 in a shared
  library's file, is given the next free package id from 0x02, as Android's
  AssetManager2 gives a dynamic package, and the ids that name package 0 in
  that table are given it as they are read; `getAssignedPackageIdentifiers`,
  `getApkPaths` and `findCookieForPath` answer; `open`, `openFd`,
  `openNonAsset`, `openNonAssetFd` and `list` look in the library's APK after
  the app's. The data app's step "shared-library" adds the Android root's
  WebView APK, finds its package as 0x2, one of its drawables and one of its
  strings by name and reads both, and opens its `assets/icudtl.dat` as a
  descriptor where it is stored, as Chromium maps its ICU data.

- `android/content/res/loader/ResourcesLoader.java` and
  `ResourcesProvider.java`, `android/content/res/Resources.java` and
  `AssetManager.java`, `android/content/pm/PackageManager.java` and
  `android/os/ParcelFileDescriptor.java`: Android 11's resources loaders. A
  provider made from an APK (`ResourcesProvider.loadFromApk`, by the file its
  descriptor was opened from; `loadFromSplit`) is that APK opened beside the
  app's (`megler.Native.addApk`) with its table read as a package of its own
  (0x7f), and a loader's providers come before a Resources' own resources
  once it is added to them (`Resources.addLoaders`): an id one of them has is
  its value and its file, `getIdentifier` finds its names in its package, and
  only the Resources it was added to have it. And
  `getResourcesForApplication("android")` is the platform's resources alone, a
  `Resources` of their own with no app package (`Resources.meglerPlatform`),
  as Android gives framework-res.apk's: it gave the app's own. Google Play
  services' DynamiteLoader makes a module's resources so on Android 11 and
  later (the platform's, and a loader of the module's APK), and the stubs kept
  nothing: the Google Maps module's legacy renderer looked up its textures'
  0x7f ids (`maps_dav_one_way_16_256` and twenty more) in the app's table,
  `BitmapFactory.decodeResource` gave null, and the map's GL thread ended on
  `Bitmap.getWidth()` of it (`gmm6.vector.gl.aj`): Transit's map stayed empty
  (0.1.0.671). Not done: a provider of a directory or of a table alone
  (empty, said in the log), and the loaders' assets through the Resources'
  `AssetManager`. The data app's step "resources-loader" adds an APK of
  resources alone that it carries (`assets/loader.apk`,
  `core/tests/app/data/loader`, made by `tools/build-test-app.sh`) to the
  platform's resources and reads a string, a colour and a picture of it.

- `android/hardware/camera2/CameraManager.java`, `getCameraIdList` and
  `getCameraIdListNoLazy`, generated signatures that answered null: the
  phone's cameras (camera2 under "The cameras" below), and an empty list,
  never null, on a device without one. The WebView counts the cameras with
  `getCameraIdList().length` as soon as a page asks for its media devices,
  and the NullPointerException, thrown inside Chromium's native code, ended
  the app on VK's "not a robot" page. The data app's step "cameras" reads
  the list (empty, not null) and `Camera.getNumberOfCameras()`, 0 there: the
  data app is run without the stand-in's cameras, a phone with none.

- `android/hardware/Camera.java` and `megler/CameraNative.java` (new), in
  place of the generated stub, whose `getNumberOfCameras()` answered 0 and
  whose camera did nothing; and `android/view/SurfaceView.java` and
  `android/graphics/SurfaceTexture.java.members` (a frame posted as a whole
  bitmap), and `android/content/pm/PackageManager.java` (the camera
  features): the phone's cameras through Android's old camera API, which VK
  mp3 mod's sign-in by QR code (budiyev's code-scanner over ZXing) and most
  scanners use. "The cameras" below has all of it.

- `android/content/pm/PackageManager.java`, Android 13's calls with a flags
  object (`ResolveInfoFlags`, `ComponentInfoFlags`, `PackageInfoFlags`,
  `ApplicationInfoFlags`), which are the calls with an int, the flags passed
  on. Some passed 0 (`getInstalledPackages` gave no signatures asked for that
  way), and `queryIntentServices`, `resolveService`, `resolveContentProvider`,
  `queryBroadcastReceivers`, `getReceiverInfo`, `getProviderInfo`,
  `getPackageUid`, `getInstalledApplications` and `queryContentProviders`
  with one were generated stubs that found nothing: Facebook Lite asks
  `queryIntentServices` and `resolveContentProvider` so, and Messenger
  `resolveContentProvider`. The data app's step "package-flags" asks each
  for the app's own service, provider, receivers, uid and signatures.

- `android/media/MediaMetadata.java`, in place of the generated stub, whose
  getters answered null and 0, whose `writeToParcel` wrote nothing and whose
  `CREATOR` was null. AndroidX's `MediaMetadataCompat.getMediaMetadata()`
  makes the platform's object by writing itself (its Bundle) into a Parcel and
  reading it back with `MediaMetadata.CREATOR`; `MediaSessionCompat` does that
  for every `setMetadata`. Spotify's "MediaSession worker" thread threw a
  NullPointerException there for each track, dozens of times a minute. Now
  Android's class: a Bundle of the keys, the Builder that fills it, the Parcel
  form (the Bundle, then the bitmap size limit, which a parcel holding the
  Bundle alone leaves at its default), and `getDescription` made of the display keys or
  the title, artist and album. The data app's step "media-metadata" sends one
  through a Parcel and reads one from a Bundle alone, as the compat class
  writes it.

- `android/net/wifi/WifiManager.java`, the generated stub with the Wi-Fi on,
  and `WifiInfo.java.members`. The stub answered from an empty map:
  `isWifiEnabled()` false, `getWifiState()` 0, `getConnectionInfo()` and
  `getDhcpInfo()` null, while `ConnectivityManager` says the phone is on
  Wi-Fi and connected. Asphalt 9's installer asks both before it fetches its
  data (`getNetworkInfo(TYPE_WIFI).isConnected() && isWifiEnabled()`), was
  told there was no Wi-Fi and no mobile network, and showed that the
  download had failed (package 264; the flag `trace-calls-...` on its
  installer showed the two checks answering false). Now the Wi-Fi is on and
  enabled; the connection is one the phone is connected to, as Android 9
  tells an app without the location permission ("<unknown ssid>", BSSID
  02:00:00:00:00:00), with the phone's IPv4 address from
  `NetworkInterface`, which `getDhcpInfo()` has too, with its netmask; and
  `calculateSignalLevel` is Android's formula. Which network the phone is
  really on is not asked of Windows: the framework says Wi-Fi throughout.
  The data app's step "wifi" asks as the installer does. And the phone's
  scans and its connection's network for an app that may have the location
  ("Wi-Fi, and Google's network location" below; `ScanResult.java` and
  `WifiSsid.java`, Android's in place of the generated stubs).

- `android/media/session/PlaybackState.java`, in place of the generated stub,
  which dropped what `setState` and `addCustomAction` were given (the state,
  the position and the speed read back as 0), had a null `CREATOR`, and an
  empty abstract `CustomAction` without its `Builder`. AndroidX's
  `PlaybackStateCompat.getPlaybackState()` makes the platform's object with
  `PlaybackState.Builder` and `PlaybackState.CustomAction.Builder`, for every
  `setPlaybackState` of `MediaSessionCompat`: Spotify's "MediaSession worker"
  thread ended on `NoClassDefFoundError:
  android/media/session/PlaybackState$CustomAction$Builder` (package 249).
  Now Android's class: the state, the position at its update time and the
  speed, the buffered position, the actions, the custom actions, the active
  queue item, the error and the extras; its Builder, Parcel form and
  `CREATOR`; `CustomAction` with its own. The data app's step
  "playback-state" makes one with a custom action and sends it through a
  Parcel.

- `android/webkit/`, forty-nine files: AOSP's `android.webkit` at
  android-15.0.0_r36 (`tools/webview-fetch.sh` fetches the sources to compare)
  in the place of the framework's own WebView, written for iOS over WKWebView, and its generated
  signatures, so that `WebView`, `CookieManager`, `WebSettings`, `WebStorage`,
  `GeolocationPermissions`, `ServiceWorkerController`, `TracingController` and
  the rest are Android's, delegating to the WebView APK's provider ("The
  WebView" above). Each file's annotations of Android's own are taken out
  (`@NonNull`, `@SystemApi` and their kind, which this framework has no
  classes for); each file that differs further says how at its top:
  `WebViewFactory`, `WebViewLibraryLoader`, `WebViewDelegate` (where the
  package comes from, how its library is loaded, single process),
  `WebViewFactoryProvider` (Android 15's flags off), `WebView` (the hooks of
  this framework's `View`: `onDetachedFromWindow` and the public
  accessibility methods where AOSP overrides `...Internal` ones; the
  translation overrides and `encodeProperties` left out),
  `WebViewClient`, `FindActionModeCallback`, `JsDialogHelper` and
  `LegacyErrorStrings` (the platform's own resources found by name in Android
  9's framework-res). `MimeTypeMap`, `URLUtil` and `DateSorter` stay as they
  were. And four of the framework's own files are taken away
  (`X.java.removed`, a third kind of overlay file: `tools/build-framework.sh`
  leaves `core/framework`'s `X.java` out): `MeglerSupportLib`, `MeglerWebSettings`,
  `MeglerWebMessagePort` and `org/chromium/support_lib_glue/SupportLibReflectionUtil`,
  which would hide the WebView APK's own class of that name, androidx.webkit's
  way in.
- `megler/Web.java`: nothing left but `deliver()`, which `AppRunner` calls for
  input phase 9 and which has nothing to do; the WKWebView host is not used.
  `megler/ViewRoot.java` no longer tells the framework's own WebView that windows
  came and went.
- `megler/GooglePackages.java`, one more package the package manager knows: the
  Android root's WebView, `com.android.webview`, installed, with the version
  and the files its APK has (`WebView.getCurrentWebViewPackage`). The
  WebView's code asks the package manager for its own package: Chromium's
  crash handling lists the WebView's APK and library folders at the
  browser's start, and ended the start on the null it got.
- `android/graphics/RenderNode.java`, `RecordingCanvas.java`,
  `BaseRecordingCanvas.java` and `Picture.java`, and
  `android/graphics/Canvas.java.members`: a node and a picture are display
  lists the core records ("Drawing: `megler.Gfx` and the shell", display
  lists), where the framework as it came drew a node's content into a bitmap of its
  size when it was recorded and drew that bitmap where the node was.
  Compose makes a node for each graphics layer, dozens, many as large as
  the screen; Spotify, a Compose app, ended on the phone with
  `OutOfMemoryError: Bitmap 1650x2560` from `beginRecording`, after
  Direct2D's `EndDraw` had failed for want of memory. A node's canvas is
  over the core's recording canvas and `endRecording` gives the node its
  list; `drawRenderNode` on a recording canvas writes the node itself into
  the outer list, and on any other draws it now, with its list and
  properties as they are. The properties stay in the Java's fields: a
  setter that changes one marks the node, and the marked nodes hand theirs
  to the core, one call each, before anything is drawn on a canvas that does
  not record. The outline (with its path) and the layer paint are copied when
  set, as Android copies them, and a layer is made as Android makes one,
  for a translucent node or one asked to be composited (the framework's own file put
  every node on with its layer paint). A picture drawn into a list is the
  list it has then; `Picture(Picture)` copies it and `close()` frees it.
  `Canvas.java.members` adds the constructor of a canvas over a recording
  canvas, and what a node or a picture asks of the canvas it is drawn on:
  whether it records, and that something was drawn. And the generated
  `RenderNode.create(String, AnimationHost)`, which answered null, makes a
  node. The data app's six `render-node` steps check it: twelve nodes and
  three of 1440 x 2560 recorded once and drawn by a parent recorded once
  give the picture the nodes' bitmaps gave, pixel for pixel; the children
  moved, scaled, made translucent and invisible without the parent recorded
  again give it again; nodes nested three deep and moved, one discarded and
  recorded again; a paint, a path, a matrix and a picture changed after
  recording do not change the list, a bitmap does; two hundred nodes of
  1440 x 2560 take 110 KB, where the framework's own ran out of memory.

- `android/graphics/Path.java`, two methods: Android's inverse fill rules.
  `toggleInverseFillType` did nothing, and what Canvas hands megler.Gfx with
  a path (`meglerFill`) said only winding or even-odd, so a path set to
  `INVERSE_WINDING` or `INVERSE_EVEN_ODD` was filled and clipped by as if
  it were not inverse. An inverse path is how a view rounds its corners by
  painting over them, and VK mp3 mod's QR sign-in does exactly that round
  its camera's picture: its `BarcodeViewfinderLayout` toggles a rounded
  rectangle to inverse in its constructor and, after its `TextureView` has
  drawn, fills it in white -- the four corners. Taken as an ordinary path
  the white filled the rectangle itself: on the phone (package 267) the
  camera streamed, every picture was drawn, and the scanner showed white.
  Now `meglerFill` has a second bit for inverse, and the core
  (`drivers/mg_java_gfx.c`) fills an inverse path as a rectangle holding
  the canvas's clip with the path inside it, by even-odd (exact for
  `INVERSE_EVEN_ODD`, and for `INVERSE_WINDING` wherever the path winds no
  more than once round a point, which a rounded rectangle or a circle does),
  and clips by one as the other op (clipping to an inverse path is cutting
  the path out). A stroke ignores the inverse, an empty inverse path draws
  nothing (Java does not pass it on). The core's tests of megler.Gfx's
  natives draw VK's frame over a picture on the stand-in and, on the PC, on
  the shell's own Direct2D: by `INVERSE_WINDING` the picture shows with
  white corners, straight, under a matrix and through a display list, and
  by plain winding the white covers it; `INVERSE_EVEN_ODD` of two squares;
  a rectangle clipped to and clipped out by `INVERSE_WINDING`. The camera
  test app frames its `TextureView` as VK does, and the test finds the
  camera's picture in it and the white corner.
  And `getPathIterator`, which gave null, with `PathIterator.java` in
  place of the generated stub: the segments are the path as the core draws
  it (`Gfx.pathSegments`, the same `path_build`), arcs, ovals and round
  rectangles in cubics and the transforms gone into the points, given as
  Android 14 gives Skia's (`SkPath::RawIter`): a line, quad or cubic with its
  start point first, a close with none, then `VERB_DONE`. There are no
  conics, since `conicTo` is a quad here. Compose walks paths with it on
  Android 14 and later (androidx.graphics.path), and the Play Store's app
  pages ended on a `NullPointerException` at `PathIterator.hasNext()`
  (package 479). The data app's "path-iterator" step walks a path of each
  verb and a rectangle, one moved by `offset`, an empty one, and a points
  array too short.
  And `MutableContextWrapper.setBaseContext`, whose generated stub kept the
  context as a property and left the wrapper on its old base. WhatsApp
  inflates a chat's views ahead of time in a `PrewarmContextWrapper` over
  the application and sets the Conversation activity as its base when the
  chat opens; the list of messages unwraps its context to that activity to
  measure the unread divider, found none ("The Context is not an
  Activity"), and the app ended as a chat with an unread message opened
  (package 487). The overlay sets the base. The data app's
  "mutable-context" step changes a view's wrapper to the activity and
  unwraps it.

The framework's own files in `core/framework` still have all forty-six faults;
the overlay is where they are fixed.

That the framework was written against a later libcore (Android 16's ART
module) showed nowhere else in linking; whether it shows in
*behaviour* is known only as far as the two test apps go. The framework is
now kept here, in `core/framework`, with the overlay apart from it.

The platform's resources it reads are Android 9's own `framework-res.apk`,
and they fit: the action bar, the dialog, the list row and the button of the
test apps are that file's layouts, styles and nine-patches.

**Its native side still to write**, in this order of need:

| The framework's natives (as written for iOS) | What it is | Here |
|---|---|---|
| `megler.Sqlite` | the system's SQLite | (done) `drivers/mg_java_sqlite.c` on SQLite's amalgamation compiled into the core, with a VFS over the host layer: "Databases" |
| `megler.Audio` | `AudioTrack`, `SoundPool`, `MediaPlayer` | (done) `drivers/mg_java_audio.c`: "Sound from Java", below |
| `megler.MediaCodecNative` | `MediaCodec` | (done) `drivers/mg_java_media.c` over the shell's decoders (`megler_codec_host`): "Decoders for MediaCodec", below |
| `android.opengl.GLES*`, `EGLNative` | generated calls into ANGLE | (done) `drivers/mg_java_gles.c`, generated by `tools/gles-natives.py` from the declarations and Khronos' prototypes, and `drivers/mg_java_gl.c`: "OpenGL ES from Java"; the picture over OpenGL is composed in the core |
| `megler.Web` | `WebView` | (done) not the shell's: Android's System WebView, Chromium, whose library runs as the app's code and draws in software ("The WebView") |
| `megler.Sensors` | sensors | (done) `drivers/mg_java_sensors.c` over `ndk/mg_sensors.c` and the shell's sensors (`megler_sensor_host`, `player/SensorHost.cpp`: Windows.Devices.Sensors): "Sensors", below |
| the keyboard, clipboard, sharing, URLs, orientation, insets | `megler.Native`'s calls to the host | (done but the clipboard, which is the app's own) `megler_keyboard_host`, `megler_system_host`, `megler_set_insets`; the screen's way up and its size: `megler_display_host` and `megler_set_surface` ("The screen resized and turned") |

### The microphone

`AudioRecord` hears the phone's microphone (`megler_microphone_host`,
megler.h; `player/MicrophoneHost.cpp`). `startRecording` asks the shell for
it at the record's rate and channels (`megler.Native.micStart`). Windows asks
the user the first time: MediaCapture, for audio alone, on the UI thread; the
package declares the microphone capability. Then an AudioGraph runs at that
rate and those channels, which the graph resamples the microphone to. Each of
its quanta, 32-bit float, is made 16-bit and pushed to the core
(`megler_microphone_push`) into a ring of two seconds, which `read` takes from
(`micRead`): 16-bit as it is, 8-bit or float made from it, into arrays or a
`ByteBuffer`. A blocking read waits for what it asked for; after a second of
nothing it is given silence, so that the app does not hang. `stop` and
`release` let the microphone go (`micStop`). With no microphone, or after the
user's no, a record is silence at the microphone's pace, as before.
`hasSystemFeature` has `android.hardware.microphone`. `MediaRecorder`, which
encodes as it records (AAC, AMR), is not done yet. The data app's step
`microphone` records from the harness's microphone, which hears a ramp, and
reads it back; the harness checks it was asked for at 16000 Hz, mono, and let
go.

### Biometrics: Windows Hello

`BiometricPrompt`, `BiometricManager` and `FingerprintManager` check the
user with Windows Hello (`megler_biometric_host`, megler.h;
`player/BiometricHost.cpp`; `megler.Biometrics` in the framework overlay):
the iris, a fingerprint or the PIN, however the user set it up.
`canAuthenticate` is `BIOMETRIC_SUCCESS` where `UserConsentVerifier` says
Windows Hello is available, `BIOMETRIC_ERROR_NONE_ENROLLED` where it is not
set up, `BIOMETRIC_ERROR_NO_HARDWARE` where the phone has none (asked again
after five seconds at the most); `FingerprintManager.isHardwareDetected` and
`hasEnrolledFingerprints`, and `hasSystemFeature("android.hardware.fingerprint")`,
say the same. `authenticate` shows Windows' own dialog with the prompt's title
and text, on the UI thread, while a thread of the framework's waits, and tells
the app's callback on its executor (`FingerprintManager`'s on its handler):
succeeded, with the `CryptoObject` it was given (concrete now: the stubs'
was abstract, and Play Store ended in an `InstantiationError` making one);
the negative button's listener, or `BIOMETRIC_ERROR_USER_CANCELED`, when the
user cancels; `BIOMETRIC_ERROR_CANCELED` when the app cancels (its
`CancellationSignal` closes the dialog); `BIOMETRIC_ERROR_LOCKOUT` after too
many tries; `BIOMETRIC_ERROR_HW_UNAVAILABLE` otherwise. One check at a time.
Keys that need the user's check (`setUserAuthenticationRequired`) are not
bound to it: Megler's keystore uses them whenever the app does.

**A call's sound** goes where the app's `AudioManager` says, as on a phone:
in `MODE_IN_COMMUNICATION` or `MODE_IN_CALL`, or with a communication device
set, to the earpiece, or to the loudspeaker after `setSpeakerphoneOn(true)` or
`setCommunicationDevice` of the speaker; `MODE_NORMAL` ends it. The two are
the communication devices there are (`getAvailableCommunicationDevices`,
`getCommunicationDevice`), and `getDevices` lists both. Each change is told to
the shell (`megler.Native.audioRoute`, `megler_audio_host.route`), which makes
its XAudio2 output again in the Communications category for a call and sets
the phone's endpoint, the earpiece or the speakerphone
(`Windows.Phone.Media.Devices.AudioRoutingManager`, declared in
`player/AudioHost.cpp` from the phone contract's metadata), and the default
once the call has ended; the microphone then hears as a call does
(Communications, the echo taken out). The data app's step `audio-route` goes
to the earpiece, the loudspeaker and back; the harness checks the shell was
told each.

### Sensors

`SensorManager` hands out the phone's sensors as Android's own
`SystemSensorManager` does (the overlay's `android.hardware.SensorManager`
and `Sensor`, over `megler.Sensors`, `drivers/mg_java_sensors.c`). What they
are made of -- Windows' sensors, in Android's units and signs, with Android's
times -- is the core's, for Java and native code alike (ARCHITECTURE.md,
"Sensors"). No permission is asked: on Android none of these needs one
(`BODY_SENSORS` is the heart rate's).

- **The list**: `getSensorList` by type (unmodifiable, as Android's),
  `getDefaultSensor` by Android's rule -- the proximity's default is the one
  that wakes the device, and there is no other -- and each `Sensor`'s name,
  vendor, version, type, string type, maximum range, resolution, power, least
  and greatest delay, reporting mode, whether it wakes the device, handle; its
  `toString` is Android's. It is asked of the core when the app first wants
  it, not at the app's start.
- **Listeners**: `registerListener` at a `SENSOR_DELAY_*` (0, 20, 66.667 and
  200 ms) or a period in microseconds, on the listener's handler's thread or
  the main thread's; the same sensor twice for a listener is refused, and a
  listener's thread is the one it first registered with, as on Android. Each
  sensor runs in the core's queue for the framework at the shortest period its
  listeners want; one thread of the framework's, "SensorEventThread", waits on
  that queue and posts each event to the listeners of its sensor whose own
  period has gone by since their last (four fifths of it), at most a hundred
  of a sensor's waiting on a listener's thread that does not take them (the
  newest lost, as Android's socket loses them). A listener's `SensorEvent` is
  one object for each of its sensors, its values as many as
  `Sensor.getMaxLengthValuesArray` says (the rotation vector's three for an app
  made for Android 4.2 or before, five after); `onAccuracyChanged` comes before
  the first event and whenever the accuracy changes, as Android's
  `dispatchSensorEvent` calls it. Nothing is delivered for a sensor a listener
  has removed. `flush` ends with `onFlushCompleted` for each of the listener's
  sensors after the events already queued.
- While the app is paused no events come: the core keeps Windows' sensors
  stopped.
- **Not there**: sensors that trigger once (`requestTriggerSensor` is false:
  Windows has no significant motion), dynamic sensors, direct channels, data
  injection. Android 1.x's `SensorListener` is served as Android's
  `LegacySensorManager` serves it (its old signs), for a display that is not
  turned.

The data app's step `sensors`, on the stand-in for the shell's sensors
(`core/tests/mg_sensor_standin.c`: one of each kind, each reading a known
value in Windows' units and signs): the eleven listed, by type, with the
stand-in's names, ranges and least delays; the proximity the default that
wakes the device; every sensor's first event on the main thread in Android's
units and signs to the last digit (the accelerometer's (0.25, -0.5, -0.75) g
as (-2.45, 4.90, 7.35) m/s², the gyrometer's degrees as radians, the
orientation's quaternion with w made positive, the inclinometer's pitch 30,
roll -20, yaw 300 as azimuth 70.31, pitch -31.57, roll 17.23), each after
`onAccuracyChanged`, its time read after the listener was registered and
before it was handed over; the accelerometer at `SENSOR_DELAY_GAME` no
sooner than 16 ms apart, in order, while the stand-in reads every 5 ms; a
flush of all eleven; then the listener removed and every one of Windows'
sensors stopped (the harness checks the stand-in's counts). The natives app's
step `sensors` does the same from native code: the list and the defaults
through libandroid, a queue on the main thread's looper with a callback
(found by `dlsym` of `ASensorManager_getInstanceForPackage`, as NativeActivity's
sample glue finds it), the accelerometer at 20 ms and the light.

### Bluetooth LE

`android.bluetooth` is the phone's radio, Bluetooth Low Energy's central side,
as Android 13 has it (the overlay's `BluetoothAdapter`, `BluetoothManager`,
`BluetoothDevice`, `BluetoothGatt`, `BluetoothGattCallback`, the GATT service,
characteristic and descriptor, and `android.bluetooth.le`'s `BluetoothLeScanner`,
`ScanFilter`, `ScanSettings`, `ScanRecord`, `ScanResult`; `android.os.ParcelUuid`,
a stub before; over `megler.Bluetooth`, `drivers/mg_java_bluetooth.c`, and the
core's `run/mg_bluetooth.c`; ARCHITECTURE.md, "Bluetooth LE"). One thread of the
framework's, "BluetoothEventThread", from the app's first use of Bluetooth (or
a receiver of its broadcasts) on, takes what the radio did and hands it on:

- **The adapter**: one, on or off as Windows' radio is (a phone without a radio
  for LE has one that is always off, and no `android.hardware.bluetooth` nor
  `.bluetooth_le` feature), named as the phone is, its address Android's for
  every app since Android 6 (02:00:00:00:00:00); `ACTION_STATE_CHANGED` as the
  radio turns, through `STATE_TURNING_ON` and `STATE_TURNING_OFF`; the scanner
  while it is on; devices by their address; the paired ones. It cannot turn the
  radio on: `ACTION_REQUEST_ENABLE` and `enable()` (an app of a target before
  Android 13) show the phone's Bluetooth settings (`AppRunner.startActivity`),
  and the request's result is `RESULT_OK` once the radio is on, `RESULT_CANCELED`
  when the app is in front again with it off; `disable()` is refused, as for an
  app on Android 13. No classic Bluetooth, no advertising (no peripheral role,
  `getBluetoothLeAdvertiser` null, `openGattServer` null).
- **Scans**: each of the app's scans hears, on the main thread, the
  advertisements its filters pass (all of them with none): `onScanResult` for
  each, or the first of a device's alone (`CALLBACK_TYPE_FIRST_MATCH`), or
  `onBatchScanResults` every report delay; a record read as Android reads one
  (flags, service UUIDs of every size, the local name, the power, the service
  and manufacturer data, Android 13's map of every AD structure), its bytes as
  the radio heard them, the advertisement's with the scan response's after. A
  callback started twice hears `SCAN_FAILED_ALREADY_STARTED`; the radio turned
  off ends every scan, unheard. Android 4.3's `startLeScan` is a scan of its own
  with the raw record. An app of a target before Android 12 hears results
  without the location as well, where Android 10 and later would ask it of the
  app: the location is Windows' to grant.
- **`BluetoothGatt`**: `connectGatt` (null with the radio off), `connect`,
  `disconnect`, `close`; the services by their instance ids, which are the
  device's handles; one request at a time, as Android's stack has it -- a read
  while one is answered is refused, a write tried for a while and then answered
  `ERROR_GATT_WRITE_REQUEST_BUSY`; Android 13's `writeCharacteristic` and
  `writeDescriptor` with the value, and the old ones with the object's own value
  and write type; a write without response answered once it is sent; the
  notifications of a characteristic `setCharacteristicNotification` let through
  (its Client Characteristic Configuration descriptor, written, subscribes at the
  device); `requestMtu`, `readRemoteRssi`, the PHY's calls and the connection's
  priority taken. Its callbacks come on the Handler `connectGatt` was given, else
  in order on a thread of the framework's, "BluetoothGatt", as Android's come on
  a binder thread; Android 13's forms with the value, which `BluetoothGattCallback`
  hands to the old ones (setting the characteristic's value first) for an app
  built before. A request the device refuses for want of pairing is sent again
  once the radio has paired, as Android's stack does.
- **Pairing**: `createBond`, `removeBond` (Android's hidden one, which apps call
  by reflection), the bond as Windows has it (`getBondState`, `getBondedDevices`),
  `ACTION_BOND_STATE_CHANGED` with the bond before and the reason, and the
  ordered `ACTION_PAIRING_REQUEST`, whose PIN a receiver gives with `setPin`
  (flutter_blue_plus does, with the PIN its app gave `createBond`); one no
  receiver answers is the user's to answer, as Android's own dialog asks.
- **Permissions**: an app that targets Android 12 needs `BLUETOOTH_SCAN` to scan
  and `BLUETOOTH_CONNECT` for the adapter's name and for what touches a device,
  else `SecurityException`, as there; the group, nearby devices, is granted with
  no one asked (Windows asks nothing for Bluetooth LE).

The test app `bluetooth`, on the stand-in for the shell's radio
(`core/tests/mg_bluetooth_standin.c`: MeshCore-Test, a random address with the
Nordic UART service, which echoes what is written to RX as a notification on TX
and answers one characteristic only once bonded with the PIN 123456; a beacon;
a device paired before), goes through what MeshCore's app does, in steps the
harness reads (`run_bluetooth`): the adapter and its feature, Android 12's
permissions requested, the adapter's name and address and the paired device;
a scan filtered by the Nordic UART service (MeshCore-Test heard on the main
thread with every field of its record, the beacon not; the same callback
started again refused) and the old scan (both, with their raw records); a
connection (its callback not on the main thread), the services with their
handles, properties and write types, the MTU (185 of 247 asked), notifications
on through Android 13's `writeDescriptor`, writes with and without response
and the old way, each echoed as a notification with the value; two reads at
once (the second refused); the RSSI; a read the device answers only bonded
(BONDING, the PIN asked for and given by a receiver, BONDED, then the read
answered), the bond removed by reflection, a bond whose PIN no receiver gives
(the user asked); the connection ended, one the device drops (19), one to a
device that never answers (133); the radio turned off by the harness
(`STATE_TURNING_OFF`, `STATE_OFF`, no scanner, `connectGatt` null) and
`ACTION_REQUEST_ENABLE` (the settings shown, where the stand-in's user turns it
on: `STATE_TURNING_ON`, `STATE_ON`, `RESULT_OK`; asked again while on,
`RESULT_OK` at once). The harness checks what the stand-in was asked -- two
active scans, both stopped, every connection ended, the bytes written, the MTU,
two pairings, one PIN given by the app and one by the user, the settings shown
once -- and the core's lines in the log.

### Wi-Fi, and Google's network location

`WifiManager`'s scans are the phone's (the overlay's `WifiManager`,
`ScanResult` and `WifiSsid`, Android's now, over `megler.Wifi`,
`drivers/mg_java_wifi.c` and the shell's `megler_wifi_host`, `player/WifiHost.cpp`
over Windows.Devices.WiFi; ARCHITECTURE.md, "Wi-Fi"). They are what Google Play
services in the app finds the phone from, as on Android: its network location
sends the networks a scan hears to Google's location server and has back where
they are, a fix of tens of metres, where Windows' own location gives some phones
the cell network's 300 to 660 m alone (Transit, 0.1.0.679: "location is still
kinda awful", every minute's summary "0 of them from the satellites").

- **A scan**: `startScan` returns at once; one thread of the framework's,
  "megler-wifi", has the shell scan (a few seconds on a phone), keeps the
  networks as the last results and sends `SCAN_RESULTS_AVAILABLE_ACTION` with
  `EXTRA_RESULTS_UPDATED` (false where no scan could be made, the results then
  the last ones), then calls Android 11's `ScanResultsCallback`s. A request that
  comes while a scan is made is answered by it; none begins sooner than three
  seconds after the last began, a request inside that waiting for it (Google
  Play services waits five seconds for its scan and drops a result older than
  its request, so a scan made before it cannot answer it).
- **The results**: `getScanResults` gives copies of the last scan's, each as
  Android has it: the BSSID ("aa:bb:cc:dd:ee:ff"), `SSID` as text (empty for a
  hidden network) and `getWifiSsid` as the octets, the signal, the channel's
  frequency (and `centerFreq0`), Android's capabilities of Windows' security
  ("[WPA2-PSK-CCMP][RSN-PSK-CCMP][ESS]", "[IBSS]" for an ad hoc network), the
  standard of Windows' physical layer, a width Windows does not say (20 MHz,
  80 MHz for 802.11ac and ax on 5 GHz), and the time each was heard:
  microseconds of `elapsedRealtime` at Windows' report, which Google Play
  services checks against its request. Through a `Parcel` and back.
- **Android's rules**: a scan and its results only while the app may have the
  location -- the location not switched off, and `ACCESS_FINE_LOCATION` from
  Android 10 (FINE or COARSE before, or Android 13's `NEARBY_WIFI_DEVICES`) --
  else `startScan` false and an empty list, as Android has them, and said once
  in the log. Google Play services in the app scans for it: the app's
  permission is what counts. The Wi-Fi's own permissions are not asked of the
  app (Transit, which asks Google Play services for the location, names
  neither). `getConnectionInfo`'s network has its name (quoted), BSSID, signal
  and channel where the app may have the location, else what Android 8.1 tells
  others ("<unknown ssid>", 02:00:00:00:00:00). `isScanAlwaysAvailable` stays
  false: Windows scans only with the Wi-Fi on. `getSystemService("wifiscanner")`
  stays null (`WifiScanner.java` says why).

**What Google Play services needs** (26.41.31, jadx of the user's GmsCore). Its
network location is a module of its own ("network_location_provider") that runs
in the app's process here, started by its fused provider
(`com.google.android.location.internal.GMS_NLP`, its
`GoogleLocationChimeraService`), and three things stood in its way:

1. **Scans.** Without a `WifiScanner` it scans through `WifiManager`
   (`gezt.a`: `startScan`; its `CallbackRunner`, `gexg`, listens for
   `SCAN_RESULTS_AVAILABLE_ACTION` only then; `gexb` reads `getScanResults`),
   and a result is used only if heard after its request and less than 30 s
   ago (`geyw`), its BSSID a MAC address and its SSID not ending in "_nomap".
   `startScan` answered false and the results were none.
2. **Google Location Accuracy.** It locates only while
   `network_location_opt_in` is 1 in Google's settings
   (`content://com.google.settings/partner`, Google Services Framework's
   `GoogleSettingsProvider`, which Megler mounts): its "network" provider is on
   only where the location's mode is and that is 1 (`dhcg.a`, `dhce`), its
   network location operates only then (`geup`: location permission, "network"
   on, consent), and its fused provider asks it for fixes only then (`gedl`).
   Android asks this as a phone is set up, or through an app's
   `SettingsClient` when the settings fall short; Transit checks them only
   while `LocationManager` says every provider is off, so it was never asked.
   `megler.LocationAccuracy` asks it: the first time the app's own code binds
   Google Play services' location API (`GoogleLocationManagerService.START`,
   an app's `FusedLocationProviderClient` or `SettingsClient`; `MicroG.bind`)
   while it holds the location, where Google's own Play services are the app's,
   Megler's question ("Allow Transit to use Google Location Accuracy?", the
   shell's group `location_accuracy`) is put unless an answer is kept for the app
   or the setting is decided already, and the answer written into Google's
   settings -- 1 or 0, as Google Play services writes it (an insert of the name
   and the value). The setting is one for every app, as Android's is; the
   app's Settings in Megler change its answer, written at its next start. Google
   Play services follows the setting (its own observer of its `Uri`), so the app
   that asked has it at once.
3. **Its fixes' way to the fused provider.** It hands each fix to its fused
   provider by a `PendingIntent` sent with an `OnFinished`, a permission and
   options (`bpep.d`: `send(context, 0, intent, onFinished, null, permission,
   options)`), a form that sent nothing (the `PendingIntent` overlay's
   `sendAndReturnResult`, android/app/PendingIntent.java above).

Then its server: the module asks Google's `VoilaTileService/FindTiles` over
gRPC for the access points it does not know (`GmsCoreVoilaTileGrpc`, `gded`;
`GlsClientGrpc`, `gexu`) and locates the phone among them itself (its "frewle"
model); whether Google answers a phone of Megler's is for a phone to show.
The call names no account: its credential is an `X-Goog-Spatula` header
(`hamx`, `hamz`: "Could not retrieve spatula header" where it has none),
which Google Play services' own AppCert service makes for the package
(`com.google.android.gms.auth.APP_CERT`, `appp.a`): the package's name and
certificate hash, and either a signature with the device key (the
`device_key` file, `apps`, got from Google's `devicekey` endpoint with a
DroidGuard result, `appr.b`) or, without one, the check-in's Android ID
alone. The account's own credential (BAD_AUTHENTICATION on the Lumia 650)
is not asked for; a phone without the device key ("DeviceKeyStore: Cannot
load key: Device key file not found", Google Play services' own line) sends
the header unsigned, and whether the server takes it is what the network's
and the location's lines below say. Google refuses Megler the device key,
whose request carries a DroidGuard result: docs/MICROG.md, "Google's own
Play services: DroidGuard", says where and why.

**What a phone showed next** (0.1.0.684, a Lumia 650, Google Play services
26.43.30 updated through Aurora): Google Location Accuracy on, one scan
asked for by the module (its `hpsb`), three networks handed to it -- and
then no second scan in two minutes and no fix. The module runs on alarms of
its own (`GmsAlarm`, `dhmz`: on Android 12 and later through
`AlarmManager.setExact`/`setWindow` with an `Executor` and a `WorkSource`, or
`set` with a window, an interval and a `WorkSource`; its timers' names are
its wake locks', "NetworkLocationLocator", "NetworkLocationScanner",
"NetworkLocationActivityDetection"), and every one of those forms was a
signature that did nothing ("AlarmManager.setWindow(...) is a stub here,
first called from dlib.g", just after the module set
"NetworkLocationActivityDetection"); `cancel(OnAlarmListener)` did nothing
either. They are Android's now (`core/framework/src/android/app/
AlarmManager.java`): one alarm a listener, moved when set again, gone when
cancelled, told on its `Handler` or through its `Executor`, timed on a
thread of their own. And at each of its scans the module asks for the cells
around (`TelephonyManager.requestCellInfoUpdate`, `gezs.e`) and waits for
them on a latch (`geyj`) until a timeout: those forms never answered, and
answer now with no cells, as Windows gives an app none. It follows the
network by Android 12's `registerBestMatchingNetworkCallback`
(`NetworkConnectionMonitor`, `geup`), which now tells it as the other
callbacks do (it read the first state itself). A wake lock is held from
`acquire` to its `release` now (`isHeld` said false, and Google Play
services said "<tag> should be held!" 64 times in those two and a half
minutes). Ruled out on that log: the module's own location listener takes
only "gps" fixes (`CallbackRunner$MyLocationListener$1.a`), and Google Play
services had asked `LocationManager` for nothing but "passive", whose
Windows' fixes are named "network" -- no cell fix reached it as a GPS one;
and it trusts a scan's timestamps from the first, as long as they are
within an hour (`gezy`: "six scans" decides only whether to stop checking).

The log says more of it now: each alarm the first time it is set and goes
off ("MeglerAlarms", 40 lines at most: `"NetworkLocationLocator" set to go
off in 20.0 s (a listener's, told through an Executor)`), when no scan has
been asked for in a minute ("MeglerWifi": "no scan asked for in the last
60 s: 1 asked for so far, 1 made, the last asked for by hpsb"), a minute of
the network location's scans without a fix ("MeglerLocation": "Google's
network location: no fix in the last 60 s; it asked for 3 Wi-Fi scans in
them ..."), and whether the location server's name was looked up ("network:
Google's location server, "voilatile-pa.googleapis.com" ..., has 4
addresses", or that it was not found): what the server answered is inside
TLS.

**The fixes**, as `megler.LocationService` sees them on their way (a
`PendingIntent` of Google Play services' whose intent carries its network
location's `EXTRA_RELEASE_VERSION`): the app's own requests of
`LocationManager` are handed the better of a fix of Google's and Windows'
newest -- the more accurate, each taken 3 m less accurate for every second of
its age -- as on Android, where the network provider is Google Play services'
network location. An app of `LocationManager` alone gets none of Google's:
the module runs only for Google Play services' fused provider, which such an
app never binds, and Megler does not start it for `LocationManager` (GmsCore
read into the app's process -- seconds and tens of megabytes on a Lumia -- and
the phone's Wi-Fi networks sent to Google for an app that asked nothing of
Google). Google Play services' own requests ("gps" and "passive", its fused
provider's and its network location's, and its `getLastKnownLocation`: told
by GmsCore's class loader, the listener's or the closest up the stack, not by
any scope's, which an app's own other processes have too) are never handed
its own fixes back, and while a fix of its network location is fresh (a
minute) are handed Windows' from the satellites alone: Windows' cell fixes
came every second as "gps" ones, fresher than the Wi-Fi's, and are what its
collection takes satellite positions for. With none for a minute they have
Windows' as before. Google's network location does not need Google Play
services' package context to have an attribution tag of its own (066d60ea):
an app's `FusedLocationProviderClient` requests are made as the app, and the
module hands its fixes to the fused provider by its `PendingIntent`, not
through the location API that refuses Google Play services' own requests
with a null tag (`gerz.h`).

The log says, in the shell's lines and the framework's ("MeglerWifi",
"MeglerLocation"): what Windows says of the Wi-Fi ("wifi: Windows' access:
allowed, 1 Wi-Fi adapter; the Wi-Fi is on"), each of the first eight scans'
networks and then a minute's ("wifi: scan 1 found 14 networks (3 on 5 GHz),
the strongest at -48 dBm, in 2804 ms"), who asked for the first scans ("a scan
asked for by gezt") and to whom results were handed, Google Location
Accuracy's answer ("Google Location Accuracy on: the user was asked, and said
Allow"), Google's network location's first fix and then each minute's ("its
first fix, 24 m accurate", "4 fixes in the last 60 s, accurate to 18 m at best,
25 m the last; Windows' newest 640 m"), and when Windows' cell fixes stop and
start going to Google Play services.

The test app `wifi`, on the stand-in for the shell's Wi-Fi
(`core/tests/mg_wifi_standin.c`: four networks -- one UTF-8 name, a hidden one,
an ad hoc one -- the first a little weaker at each scan, the phone connected to
the first), asks for a scan and hears its broadcast on the main thread, reads
every field of the four results, a second scan's three seconds later, two
requests answered by one scan and its callback, the connection, and a result
through a `Parcel` (`run_wifi`: three scans of the stand-in's, the core's and
the framework's lines). The data app, which names no location permission, is
refused its scan and results. And `gms-host`, holding the location, binds the
location API of the stand-in for Google's own Play services, which has a
stand-in for Google's settings: the user is asked once, and
`network_location_opt_in=1` is written there. Windows' fix is the cell
network's 600 m in that run (`mg_location_standin_cell`), and the stand-in's
network location hands its fused provider a fix of 5 m as Google's does (a
`PendingIntent` of its own, sent with an `OnFinished`, the intent marked with
the module's release): the app's `getLastKnownLocation("network")` and the
first fix of its "gps" request are that one, named for the provider asked,
and the stand-in's own "gps" request is handed none of Windows' while it is
fresh. A second later an alarm of the stand-in's, set as `GmsAlarm` sets
theirs (`setExact` with an `Executor` and a `WorkSource`), goes off and its
second fix is sent. The data app's steps "alarm-listeners" (a listener's
alarm moved, cancelled, through an `Executor` with a `WorkSource`, with a
window, repeating), "wake-lock" (counted, uncounted, timed out) and
"cell-info-update" (no cells, told; the best-matching network callback
told) check the rest.

### Android's HTTP engine: `android.net.http.HttpEngine`

Android 14 gives apps an HTTP stack of the platform's, `HttpEngine`: Cronet,
Chromium's network stack, in the Tethering module, over a native library of
its own. Megler does not carry that library, and the framework had the API as
generated signatures: `HttpEngine.Builder.build()` answered null.

**Who takes it.** Cronet's own provider list (`CronetEngine.Builder`, which an
app carrying Cronet's API makes) offers HttpEngine only where
`SdkExtensions.getExtensionVersion(31)` is 7 or more; Megler says 0, so Cronet
never picks it there, and an app's own Cronet, or Google Play services', is
taken. The Google Play Store (53.7.23, `sxi.g` in the jadx of its classes3)
asks for HttpEngine itself on Android 14 when a server flag of its,
`NetworkOptimizationsAutogen` / `enable_bypass_sdk_extensions_check_for_platform_cronet_on_u`,
is on for the phone: "Device is eligible for platform Cronet and will attempt
to use it." It then builds the engine with no check of its own
(`HttpEngineNativeProvider.createBuilder`), and a null went into Cronet's
wrapper as its engine: every request of its Volley stack (`openConnection`)
and every gRPC call (`newBidirectionalStreamBuilder`, its channel to
play-fe.googleapis.com) ended on that null -- "Uncaught exception in the
SynchronizationContext. Panic!", no search suggestions as the user typed, and
Google Play services' modules (Maps, the OCR) never downloaded, since Google
Play services asks the Play Store's ModuleDelivery for them (a user's
Lumia 950, 0.1.0.681: "Error while loading ModuleDelivery: ... HttpEngine.openConnection
... on a null object", "Zapp module request failed: 1,-5007"). Where the flag
is off -- the Lumia 650's and the Elite x3's logs: "Device is ineligible for
platform Cronet and will use embedded Cronet instead" -- the Play Store uses
the Cronet it carries ("source: Statically Linked") and all of that works.
The Play Store has a fallback, OkHttp and gRPC's OkHttp transport, but only
for an engine whose build throws (`aeus.e` catches it and uses none); a null
it takes as an engine.

**Megler's engine** (`megler.http`, in the overlay; `HttpEngine.java` there
hands the Builder's calls to `megler.http.EngineBuilder`, as Android hands them
to Cronet's builder through the hidden `IHttpEngineBuilder`) is Java over what
Android 9's libcore has, and a client of HTTP/2 of its own:

- `UrlRequest` (`Request.java`): Android's own HTTP client, libcore's
  `HttpURLConnection` (okhttp 2.7, HTTP/1.1), on the engine's threads, its
  redirects off: Cronet's states and callbacks -- `onRedirectReceived` and
  `followRedirect` (303, and 301 or 302 after a POST, go on as GET without the
  body; 307 and 308 rewind the upload), `onResponseStarted`, a `read` into the
  app's buffer at a time and `onReadCompleted`, `onSucceeded` at the body's end,
  `onCanceled` after `cancel()`, `getStatus`. The upload is the app's
  `UploadDataProvider`, read on its executor, of a fixed length or chunked, and
  closed when the request ends; one without a Content-Type is refused at
  `start()`, as Cronet refuses it. The client's own `X-Android-*` headers are
  not the response's (one of them gives the protocol, "http/1.1"); an app's
  Accept-Encoding is dropped, as Cronet drops it (the client asks for gzip and
  decodes it). https gets the platform's TLS and Android's host name check,
  whatever defaults the app set for `HttpsURLConnection`.
- `BidirectionalStream` (`Stream.java`): what gRPC's Cronet transport makes, a
  request's body and its response flowing both ways at once. HTTP/2 over TLS
  alone (an http URL is refused, as Cronet's are): `H2.java`, `Hpack.java` and
  `Connections.java`. A connection is TLS by the platform's own context
  (conscrypt; the app's network security configuration and Android's
  authorities, as `HttpsURLConnection` trusts), the host's name checked by
  Android's `OkHostnameVerifier`, ALPN offering "h2" alone (a server that does
  not take it fails the stream, "net::ERR_ALPN_NEGOTIATION_FAILED"); one or
  more for each host and port, shared by the engine's streams, the first
  thread that needs one making it while the others wait for it. This side's
  settings: no push, a megabyte that a stream may receive ahead of the app's
  reads and 16 MB for the connection, given back by WINDOW_UPDATE as the app
  reads, half a window at a time; what is sent waits for the server's windows.
  The response's first HEADERS are its headers (1xx skipped), a later one with
  END_STREAM its trailers, kept apart and handed to
  `onResponseTrailersReceived` before the body's end; header compression is
  RFC 7541's both ways (the encoder writes plain literals, never indexing the
  sensitive ones; the decoder has the dynamic table and Huffman's code, from
  the code lengths of Android 9's okhttp). A connection with no stream for
  five minutes is let go; one the server has said nothing on for 30 s is
  asked for a PING when it is taken again and closed if no answer comes in
  10 s (a phone that was suspended, a network that changed). GOAWAY: what the
  server did not take fails as a closed connection's, which may be tried again.
  Android 9's own HTTP/2 (okhttp 2.7's `FramedConnection`) is not used: its
  stream merges a response's trailers into the headers it has already handed
  over, so where a response's frames arrive together -- a unary gRPC call's,
  often -- gRPC's status would be read as a header; and its settings' methods
  are its package's alone.
- `openConnection` and `createUrlStreamHandlerFactory`: libcore's
  `HttpURLConnection`, made through libcore's own handlers
  (`com.android.okhttp.HttpHandler`, `HttpsHandler`), so that the factory,
  installed as the process's, does not come back to itself.
- Failures as Android's engine says them: a `NetworkException` with Android's
  error code and Chromium's name of the error in the message ("Exception in
  BidirectionalStream: net::ERR_CONNECTION_REFUSED (...)"), the Java exception
  its cause; a `CallbackException` for what the app's callback or upload
  threw. `HttpException` keeps its message and cause (the stub dropped both),
  and `QuicOptions` the hosts and values it is given (the stub's
  `addAllowedQuicHost`, which the Play Store's Cronet wrapper calls, was a
  signature alone).
- The builder checks what Cronet checks (the storage folder there, a disk
  cache with one, 32-byte pins, QUIC hints' hosts); the options of QUIC, the
  cache, Brotli, DNS and connection migration are kept and change nothing,
  and public key pins are kept and not enforced, which the log says once
  ("HttpEngine: the app pins public keys (...): kept, not enforced here").
  `getVersionString()` is "1.0.0.0": Cronet's list ranks an HttpEngine below
  the version it prefers (133) under the Cronet an app carries, so where both
  are offered, Chromium's stack is taken. The default user agent is Android
  14's form, "AndroidHttpClient (Linux; U; Android 14; ...; Cronet/1.0.0.0)".
  `shutdown()` with requests running is refused, and nothing starts after it.

The log says it: "HttpEngine: the app made one: Megler's own ...", once a
process; each HTTP/2 connection made, with TLS's protocol and suite and the
time it took, the first sixteen ("HttpEngine: HTTP/2 to <host>:<port>
(<protocol>, <suite>) in <n> ms"); and the network failures of requests and
streams still running (not those the app ended), the host alone, the first
twenty ("HttpEngine: a stream to <host> failed: ...").

**Tested** in the data app (`HttpEngineSteps.java`; `mg_java_tests.c` checks
the steps and the lines): "http-engine", the builder as the Play Store drives
it, the version and user agent, what is refused, `shutdown`; "http-engine-request",
a GET redirected (302, a relative Location) and followed, read four bytes at a
time on the app's executor, the URL chain, status, protocol and a header of
two values, no X-Android-* headers, the user agent and the app's header sent,
a 404 and its body; "http-engine-upload", an upload of a fixed length given
five bytes a read and a chunked one, each provider closed, one without a
Content-Type refused, a 303 after a POST followed as a GET without the body;
"http-engine-failures", a refused connection (ERROR_CONNECTION_REFUSED), an
exception of the app's callback and of its upload (CallbackException, theirs
the cause), a cancel in the middle of a body; "http-engine-connection", the
engine's URLConnection and a URL of its handler factory's. These run on both
hosts, against an HTTP/1.1 server of the app's own on 127.0.0.1. Where
conscrypt's TLS is (qemu), against an HTTP/2 server of the app's own over TLS
(localhost, the test authority the app's configuration trusts, ALPN h2), whose
frames are written by hand and which shares no code with the engine:
"http-engine-stream", two echoes at once on one connection, each writing twice
and ending as gRPC does, the headers before the body, the trailers
(grpc-status 0) before its end, and a GET, whose HEADERS end the request; the
server saw the engine's settings, pseudo-headers and headers;
"http-engine-stream-flow", 200 000 bytes up to a server whose streams take
16 KB at a time and 1.25 MB down, past the megabyte a stream may receive ahead
(the server waits for the engine's WINDOW_UPDATE), CRC32s equal, no window
ever overrun; "http-engine-stream-cancel", a stream canceled (RST_STREAM
CANCEL at the server), one the server resets (NetworkException, INTERNAL_ERROR),
an http URL refused, an exception of `onStreamReady` (CallbackException, the
stream reset); "http-engine-hpack", RFC 7541's C.6.1 to C.6.3 as three
responses on one connection after a size update of the table to 256 bytes,
the second padded and the third cut by a CONTINUATION: each status and header
as the RFC has them. Once, against Google's own front end (2026-11-10, the
engine's classes on a desktop JVM, with its TLS): HTTP/2 to
play-fe.googleapis.com over TLS 1.3, Google's compressed headers read, and a
gRPC call to the Play Store's BrowseService without credentials answered in
its trailers, apart from the headers ("grpc-status" 7, "Permission denied on
the service."); a request to www.google.com/generate_204 got its 204. **Not
tried on a phone** yet: the Play Store's log there says "HttpEngine: the app
made one" and its HTTP/2 connections when the flag is on, and search
suggestions answer.

Not there: QUIC; an HTTP cache (`wasCached` is false); Brotli; public key
pinning; HTTP/2 for `UrlRequest` (it is HTTP/1.1, as Android's
`HttpURLConnection` is); `bindToNetwork` (there is one network); request
priorities. Cookies follow libcore's: a `CookieHandler` the app installed is
used by `UrlRequest` too, which Cronet's own store would not be; cleartext is
allowed where libcore allows it (Megler's libcore does not consult the app's
network security configuration for it).

### Sound from Java

`megler.Audio`'s natives (`drivers/mg_java_audio.c`) are what the framework's
`AudioTrack`, `MediaPlayer` and `SoundPool` play through, on the mixer of
`ndk/mg_audio.c`:

- a track (an `AudioTrack`; the music a `MediaPlayer` decodes and writes) is
  one of AAudio's written streams (`mg_pcm_*`, `ndk/mg_aaudio.c`): 16-bit PCM
  at the track's rate and channels, 100 ms of it waiting at most. A blocking
  write waits for room, paused or not, as `AudioTrack.write` does, until a
  flush, a stop or a close;
- a sound (`SoundPool`) is decoded whole when loaded, and each play of it is a
  voice of the mixer at the sound's rate times the play's, with its two
  volumes and its loops;
- a decoder reads what the app has in memory or behind a descriptor:
  - Ogg Vorbis through stb_vorbis (`core/third_party/stb_vorbis`, public
    domain; Windows has no Vorbis decoder);
  - WAV (8, 16, 24 or 32-bit PCM, or float), read here;
  - anything else (MP3, AAC and M4A, FLAC, WMA) through the shell's decoder
    (`megler_media_host`). It wants a file, so the bytes are written to one
    under `media/` in the data folder, named for what they are, and deleted
    with the decoder. The shell's reader (`player/MediaHost.cpp`) was made
    for films and refused a file with no picture ("no picture in the file");
    on the phone Spotify's SoundPool sounds, MP3s, were all refused so. A file
    with no picture at all is opened for its sound now (width and height 0,
    as `megler_media_host` allows); a picture that is there and cannot be
    decoded still makes the film one that cannot be played. The PC test
    (`--codec-windows`, which builds MediaHost.cpp in) reads the data app's
    `tone.mp3` and `tone.m4a`: half a second of 440 Hz each, as sound alone.
  A decoder gives 16-bit samples, at most two channels.
- the shell's decoder gives a file's pictures as well (`decVideo`: the next one
  into a bitmap, with its time; `decInfo` its width and height). `MediaPlayer`
  (the overlay's, `core/framework-overlay/android/media/MediaPlayer.java`)
  draws each on the Surface given by `setSurface` or `setDisplay`, fitted or
  cropped (`setVideoScalingMode`), once its clock reaches the picture's time:
  the track's position while there is sound, a monotonic clock after it or
  without it. One worker decodes and writes, and stop, reset and release wait
  for it; a generation number keeps an old worker or a cancelled
  `prepareAsync` from reporting. A source is read whole into memory (256 MiB
  at most), an HTTP one while preparing, with the headers given. `VideoView`
  plays through it, and puts off `start` and `seekTo` until it is prepared.
  (mayconus's work, docs/LUMIA-950XL.md.)

The mixer gained per-channel volumes and a rate that can change
(`mg_voice_set_gains`, `mg_voice_set_rate`) for these.

The data app's steps:
- "audio-track": a playing track takes a second of PCM in about a second;
- "media-player": a WAV the step writes plays to its end in about its half second;
- "media-player-ogg": the Ogg asset `tone.ogg` (440 Hz, half a second, made
  with ffmpeg's libvorbis), through its descriptor, plays to its end likewise;
- "sound-pool": the same asset, loaded and then played.

### Decoders for MediaCodec

`megler.MediaCodecNative`'s natives (`drivers/mg_java_media.c`) are what the
framework's `MediaCodec` decodes through. `MediaCodec` itself is the framework's own
Java: it decodes each queued buffer at once, on the thread that queues it,
keeps the outputs in order for `dequeueOutputBuffer` or for the `Callback`
on its handler (ExoPlayer's asynchronous mode), and draws a picture that is
released with `render` into the output `Surface` with `lockCanvas`, scaled
to it. The decoding proper is the shell's, through a call of its own in
`megler.h`, `megler_codec_host`: a decoder of a kind (H.264, H.265, AAC, MP3)
opened with the stream's configuration, fed one sample at a time, giving
pictures (NV12 or RGBA) and blocks of 16-bit sound. The shell's side is
Media Foundation's synchronous decoder transforms (`player/CodecHost.cpp`):
found with `MFTEnumEx`, fed with `ProcessInput`, emptied with
`ProcessOutput`, a change of size or format followed, NV12 pictures held in
the shell's own buffers until the core lets them go (P010, which a decoder
of ten-bit streams may offer alone, is copied out as NV12, the high eight
of each sample's ten bits), sound asked for as 16-bit PCM in two channels at
most, each decoder on a thread of its own.
Picture decoders are asked for low latency (`MF_LOW_LATENCY`): a picture
then comes out as soon as the stream's own reordering allows, as from
Android's decoders, rather than once the decoder's whole picture buffer is
full (on this PC's H.264 decoder, after 2 units of the test stream instead
of 18 to 24, varying with its threads). The flag file
`codec-no-low-latency` brings the decoders' own way back.
What the core does between an app's buffers and that call:

- **Pictures.** An app may hand H.264 and H.265 over with start codes or
  length-prefixed, with the parameter sets in the stream, as csd-0 and csd-1
  (or as MP4's avcC or hvcC record), or in a buffer flagged as codec-specific
  data. The shell is given Annex-B alone, without access unit delimiters,
  and the parameter sets in front of the first unit after a start or a flush
  when the unit does not carry them.
- **Display order.** Media Foundation's decoders give pictures in display
  order. A decoder that gives them in decode order says how
  far out they can be (`reorder`): that many are held, sorted by time, the
  earliest going on as each new one comes, all of them at the end of the
  stream. A flush drops what is held. In low-latency mode a Media
  Foundation decoder trusts the stream to say how far its pictures are
  reordered; should one come out after a later one has gone on, that is
  said in the log, and four are held back from then on (twice as many each
  time it happens again, up to sixteen).
- **Sound.** One packet per buffer. AAC's AudioSpecificConfig is csd-0 or a
  codec-specific buffer; ADTS headers are taken off, and the first one is the
  configuration when there is no other; with neither, one is made from the
  format's rate and channels. MP3's rate and channels are its first frame's.
- **A picture to the screen.** `renderFrame` turns the shell's picture into
  the `Bitmap` MediaCodec draws: NV12 to colour with BT.601 or BT.709, video
  range or full, as the stream says, and as Android does when it says
  nothing (BT.601 under 720 lines, BT.709 from there), from tables, a block
  of 2 x 2 pixels at a time.

`MediaCodecList` (the overlay's) lists a decoder for each kind the shell
says it has.

The tests:
- the core's ("the decoders behind a Java app's MediaCodec", both hosts and
  the phone's Self-test): `core/tests/mg_codec_standin.c` stands in for the
  shell's decoders and decodes nothing, but refuses a unit that is not
  Annex-B, has no slice, or comes first without the parameter sets, refuses
  ADTS, and hands pictures out in decode order (`reorder` 4), each a grey
  whose luma says its time. Ten units in decode order come out as ten
  pictures in display order in the right grey, with the parameter sets as
  csd in Annex-B, as an avcC record with length-prefixed units, in the first
  unit, and in a codec-config buffer; a flush with four held drops them and
  the four after it come out alone; AAC with csd, with a configuration over
  what the format said, with a codec-config buffer, and with ADTS and none;
  MP3 whose first frame says 22050 Hz in one channel; NV12's red in BT.601
  and BT.709, video range and full, and RGBA. And a decoder that says it
  gives pictures in order and does not: after the first picture out of
  place the rest come in order, and after a flush all of them.
- the data app's steps, under qemu with the same stand-in (the assets
  `avc.mgau`, `aac.mgau`, `mp3.mgau`, made by `tools/media-test-streams.py`
  with ffmpeg: 24 pictures of H.264 with two B-pictures between references
  and an IDR at 0 and 12, 168 x 94; half a second of a 440 Hz tone as AAC LC
  and as MP3):
  - "codecs": the stand-in's three decoders listed, one found for a film's
    format and made, and H.265 an `IOException`;
  - "codec-avc": csd-0 and csd-1, each picture shown on a `Surface` over a
    `SurfaceTexture`: the output format, then the 24 pictures in display
    order, then the end of the stream; the last one is in the
    `SurfaceTexture` in its grey;
  - "codec-avc-flush": seven units, a flush, then from the IDR at 12: only
    pictures 12 to 23 come out;
  - "codec-avc-inband": the parameter sets in a codec-config buffer;
  - "codec-avc-async": the callbacks on a `HandlerThread`, the 24 pictures in
    order on its thread ("codec-avc-async-done", which the runner waits for);
  - "codec-aac", "codec-aac-adts", "codec-mp3": a block of PCM for each
    packet, 44100 Hz in two channels;
  - "codec-avc-full": units queued and no output taken: the decoder is
    full (`dequeueInputBuffer` says try again) with eight pictures or more
    waiting, before all 24 are in; once the waiting ones are released it
    takes input again, and all 24 come out in order.
- the shell's own decoders on a PC (`tools/build-tests.sh win`, as
  `mg_tests --codec-windows`: `core/tests/mg_codec_tests.c` over
  `player/CodecHost.cpp`, built into the test program with
  `core/tests/mg_codec_windows.cpp`), fed the same streams and
  `avc-small.mgau`. On this PC (Windows 11) MFTEnumEx finds "Microsoft H264
  Video Decoder MFT", "HEVCVideoExtension" (the Store's), "Microsoft AAC
  Audio Decoder MFT" and "MP3 Decoder MFT", and:
  - H.264, with the parameter sets as csd, in the first unit, in a
    codec-config buffer, and as avcC with length-prefixed units: all 24
    pictures in display order (Media Foundation's decoder reorders them
    itself), the first after 2 units, each 168 x 94 out of 176 x 96
    decoded, its luma and its colour after `renderFrame` the grey its number
    says; a flush after seven units (pictures 0 to 5 out by then) and then
    from the IDR at 12 gives 12 to 23 alone; and a stream that goes on at
    96 x 64 after a codec-config buffer gives all 36 in order, each at its
    own size;
  - H.265 the same three ways and the flush (168 x 94 of 168 x 96, the first
    picture after 3 units), and H.265 Main 10 (`hevc10.mgau`, ten bits a
    sample) likewise, which this PC's decoder hands out as NV12 itself; and
    both once more with P010 taken where it is offered beside NV12
    (`CodecHost::preferTenBits`, for the tests), which is the way of a
    decoder that offers P010 alone: rows of 336 bytes for 168 pixels, made
    NV12, every picture in order in its grey;
  - AAC raw with csd-0 and with ADTS headers alone: 23 blocks of 1024 frames,
    44100 Hz in two channels, the tone 440.0 Hz at 8181 RMS (the stream's
    8192); MP3 configured without a rate: 23664 frames of the 24192 the
    frames hold (the decoder's own delay), 440.0 Hz at 7782 RMS;
  - H.264 and AAC decoded at once on two threads, five times each, every run
    whole.
  And timed (`mg_codec_speed_run`) on two seconds of ffmpeg's moving test
  picture at 720p and 1080p, made into `build/media-speed` by
  `tools/media-test-streams.py --speed` (not kept in the repository): on
  this PC (the test program's `/O1`), H.264 at 720p 3.2 ms a picture to
  decode and 4.3 ms for `renderFrame` to turn it into a bitmap, at 1080p
  5.7 and 9.8 ms, H.265 at 1080p 3.9 and 10.6 ms; the first picture after 3
  units (three B-pictures between references; H.265 after 4). The
  conversion cost more than the decoding, so `renderFrame` takes its terms
  from tables now and does a block of 2 x 2 pixels at a time, the pixels
  unchanged: a picture of 1920 x 1080 in 3.8 ms where it took 13.0, built as
  the app is (`/O2`). The speed run times it alone first, with no decoder at
  work beside it (4.4 ms in a `/O2` build of the test program, 6.8 in the
  usual `/O1` one); with the decoder's threads at work beside it the times
  wander by a half.
- the same on a phone: `tools/build-appx.sh arm --auto codec` packs the
  streams and makes the timed ones into the package's folder `codec`, and
  the auto run goes through both with the phone's own decoders, a line each
  into `megler-auto.log` (`tools/phone.py auto` fetches it), starting with
  which decoder the phone has for each kind.

**On the phone** (HP Elite x3, 2026-10-18, packages 245 to 248):
- **The decoders.** For H.264, Microsoft H264 Video Decoder MFT. For H.265,
  HEVCVideoExtension, then Microsoft H265 Video Decoder MFT. For AAC,
  Microsoft AAC Audio Decoder MFT. For MP3, MP3 Decoder MFT. All are
  synchronous.
- **HEVCVideoExtension never returned from its first `ProcessOutput`.** The
  first run of the speed and order checks stood still there; nothing else
  moved. So every call of a decoder's is limited now (`callLimit`, 8 s, in
  `CodecHost.cpp`). A call that has not returned by then is given up on: the
  core is told the call failed, the decoder and its worker's thread are left
  to it, and a later call answers at once. The decoder's name is said in the
  log with the call it was in, and written to `LocalState\codec-hung.txt`, so
  that no later open, in this run or the next, takes it; the next decoder for
  the kind is taken instead. On the phone that is Microsoft's own H.265
  decoder: the second run went past HEVCVideoExtension at once. The PC test
  checks the limit itself (`CodecHost::checkCallLimit`).
- **What passes:** H.264 in every form of parameter sets, a flush, a change of
  size, all in display order with the right pixels; H.265 and H.265 Main 10
  through Microsoft's decoder; AAC raw and with ADTS; MP3; two decoders at once.
- **What does not:** after a flush, Microsoft's H.265 decoder gives the first
  picture (12, the stream's IDR) black, and one picture or three short at the
  end. Telling it of a discontinuity and a new start of the stream changed
  nothing (tried, not kept), nor did low latency off. The test stream is
  x265's, whose GOPs may be open: whether a stream of a phone's camera or a
  service does the same is not known.
- **Speed**, a picture each:

  | Stream | Decode | NV12 to a bitmap | Pictures a second |
  |---|---|---|---|
  | H.264 720p | 21 to 29 ms | 5.0 ms | 29 to 39 |
  | H.264 1080p | 47 to 55 ms | 11.8 ms | 15 to 17 |
  | H.265 1080p | 11 to 17 ms | 14.2 ms | 31 to 40 |

  `renderFrame` alone takes 9.6 to 10.6 ms for 1080p. Microsoft's H.264
  decoder works in software here; it would use the graphics processor if it
  were given a Direct3D device manager, which it is not yet.

### The cameras

An app's `android.hardware.Camera` -- the old camera API, deprecated since
Android 5 and still what most code scanners use, VK mp3 mod's sign-in by QR
code among them (budiyev's code-scanner over ZXing) -- is the overlay's
`Camera.java`, Android 9's class written out in Java, over the natives of
`megler.CameraNative` (`drivers/mg_java_camera.c`), over the shell's cameras
(`megler_camera_host` in megler.h).

VK mp3 mod (version 190) scans two ways, both in its DEX files: code-scanner
in `BarcodeScanFragment` (`open(n)` on a thread of its own, a `SurfaceView`
by `setPreviewDisplay`, `setPreviewCallback`, autofocus),
and a ZXing scanner of its own in `BarcodeShareFragment` (`open()`, a
`TextureView`'s `SurfaceTexture` by `setPreviewTexture`,
`setPreviewCallbackWithBuffer` with a buffer added back after each frame,
`autoFocus` straight after `startPreview`). Neither takes a picture. The QR
sign-in is the second (seen on the phone, package 267: its
`CameraConfigurationUtils` lines, "preview size = 640x640, view size=770"):
a `DialogFragment` whose `TextureView`, 220 dp square, sits in a
`BarcodeViewfinderLayout` that paints its corners over with an inverse
path. That path was the white over the picture on the phone (`Path.java`
in the overlay's list above).

- **The cameras.** `getNumberOfCameras` and `getCameraInfo` (which way the
  camera faces, how many degrees its picture is turned) are the shell's
  answers; `open()` is the first camera at the back, or null where there is
  none. `PackageManager.hasSystemFeature` says `android.hardware.camera`
  (one at the back), `.any`, `.front`, `.autofocus` and `.flash` from them:
  a back camera's focus and light are known once it has been opened (the
  shell keeps what it saw), and until then focus is taken as there and a
  light as not. The CAMERA permission is a dangerous one ("Permissions", in
  Design): an app requests it and Windows asks the user, once; the camera
  test app's stand-in for the shell grants it.
- **Opening.** `open(n)` connects; as on Android 9, whose `native_setup`
  answers `EACCES` whenever the camera service gives no camera, every
  failure -- refused, held by something else, no such camera -- is a
  `RuntimeException("Fail to connect to camera service")`, and the log says
  which ("camera: camera 0 could not be opened: ..."). The camera's event
  handler is on the looper of the thread that opened it, or the main one.
- **Parameters** are Android's: a map of keys and values in the order they
  were set, `flatten` as `k=v;k=v`, `unflatten`, and every getter and setter
  reading and writing Android's keys, so that `get("preview-size-values")`
  works as it does there. What a camera's driver would start with and offer
  is made from what the shell says the camera does: its preview sizes (up to
  1920 x 1080, the largest first; the preview starts at the largest within
  1280 x 720), NV21 (`yuv420sp`) and YV12 (`yuv420p`), its frame rates and
  their ranges, focus modes `auto`, `continuous-picture`,
  `continuous-video` and `macro` where it focuses close up (`fixed`
  otherwise), flash modes `off` and `torch` where it has a torch (none at
  all otherwise: `getSupportedFlashModes()` is null, as on Android), one
  scene mode, white balance, effect and antibanding each, no zoom, no
  exposure compensation, no focus or metering areas, and a focal length and
  angles of view that are a phone's usual (made up). `setParameters` refuses
  what is not offered as a driver does (`RuntimeException("setParameters
  failed")`, the key and the value in the log), and applies the focus mode
  and the torch at once while the camera previews.
- **The preview.** `startPreview` starts the shell's camera at the preview
  size and at the slow end of the preview's frame rate range, but not below
  15 frames a second unless the range's top is (a camera may run anywhere in
  its range; a scanner's usual 15 to 30 is then 15, half the pictures to copy
  into the app's Java and to show), and a thread of the camera's own
  ("Camera preview N") waits for its news (`waitNews`): a picture, an end
  of focusing, the camera gone. A camera that streams faster than it was
  started at (the shell takes the nearest rate it has, and a phone's preview
  may have only 30) has the pictures that come sooner than four fifths of the
  period after the last one kept dropped before they are copied. The core
  copies each picture it keeps as it comes (the shell's memory is its own for
  the call alone), NV12 into NV21, in three buffers -- the one the shell's thread fills, the latest whole one,
  the one the preview thread works from -- so that neither waits for the
  other and a picture not taken in time is replaced by the next. For the
  view the picture is drawn into a `Bitmap` of the turned size, mirrored
  first for a front camera, then turned by `setDisplayOrientation`, as
  Android shows a preview (BT.601 video range, from tables); the bitmap is
  then posted whole as the surface's front buffer (`meglerPostBitmap`, on the
  `SurfaceView` given by `setPreviewDisplay` or the `SurfaceTexture` given by
  `setPreviewTexture`, which a `TextureView` shows), and the view scales it
  to its size. Drawn into a canvas of the surface's first, it would be read
  back from the graphics processor once a picture. Two bitmaps take turns,
  and a `SurfaceView` whose window has not yet drawn the last picture is not
  given another (`meglerFrameDrawn`). Any other surface gets the picture drawn
  into its `lockCanvas`, scaled. A `SurfaceTexture` drawn by OpenGL ES
  (`updateTexImage`) gets nothing, as the framework's `SurfaceTexture` has
  no pixels for OpenGL.
- **Preview frames**, as Android's JNI hands them: with `setPreviewCallback`
  a new array for each picture, but the next is copied only once the message
  with the last one is being handled, as Android re-arms its one-shot mode
  in `handleMessage` -- a slow callback misses pictures rather than having
  them queue; with `setOneShotPreviewCallback` one picture; with
  `setPreviewCallbackWithBuffer` the app's buffers in the order they were
  added, each used once, no buffer no picture, and a buffer too small taken
  off the queue ("Callback buffer was too small! ...") and the picture given
  as null while other buffers remain, dropped when none do -- both Android's
  JNI. `setPreviewCallback`, the one-shot one and
  `setPreviewCallbackWithBuffer(null)` drop the buffers.
- **Focus.** `autoFocus` in `auto` focuses once (`FOCUS_ONCE`), in `macro`
  close up; in the continuous modes focusing goes on by itself from the
  moment the mode is set during the preview, and `autoFocus` answers when the
  picture is in focus; with fixed focus it answers at once, in focus, as
  Android's cameras without focus do. `onAutoFocus` comes on the camera's
  event handler; nothing comes after `cancelAutoFocus`; `autoFocus` outside
  the preview is `RuntimeException("autoFocus failed")`; `stopPreview`
  forgets the callback, as Android's does.
- **The torch** is flash mode `torch`: on while the camera streams, off with
  `off` and when it stops.
- **The end.** `release` stops and closes; a camera used after it throws
  "Camera is being used after Camera.release() was called"; a camera the app
  leaves open is closed when the app ends (`mg_java_camera_close_all`).
- **Not done:** `takePicture` (refused with `RuntimeException("takePicture
  failed")`, said in the log), zoom, exposure compensation, focus and
  metering areas, face detection (refused as Android refuses it where there
  is none), the `AutoFocusMoveCallback` (never called), RenderScript
  allocations as preview targets, and a `SurfaceTexture` drawn by OpenGL ES.

The tests:
- the core's ("the cameras behind a Java app's android.hardware.Camera",
  both hosts and the phone's Self-test, 44 checks), on
  `core/tests/mg_camera_standin.c`, which stands in for the shell's cameras:
  a back camera turned 90 degrees with focus and a torch, a front one turned
  270 without, each at a few sizes, a picture every 1/fps of a second on a
  thread of its own, four quarters of known colours (red, green, blue,
  white, in BT.601's video-range values) with the picture's number in its
  first four bytes, its rows longer than the picture and the rest 0xEE. What
  the cameras are; opening refused (the stand-in can refuse as Windows does),
  busy and no such camera; nothing held before streaming; each picture held
  as NV21 byte for byte; NV21 and YV12 copies (YV12's aligned rows, V's plane
  before U's); the picture drawn for a view in the four turns, mirrored and
  not; the next picture numbered higher; focusing once, close up and
  cancelled, continuous; the torch on, and off at stop; nothing after stop;
  the front camera beside the back one; closing and opening again; started
  at 10 frames a second on the stand-in's slowest 15, pictures dropped and
  kept, each one or the other; and a camera left open closed at the app's
  end.
- the test app `camera` under qemu with the same stand-in
  (`core/tests/app/camera`, 40 checks): the permission; the features; the
  cameras' information and an unknown one refused; `open()` on a
  `HandlerThread`, `open(9)` refused, the same camera again refused; the
  parameters offered, read through Android's getters and `get`, flattened
  and unflattened, an area written as Android writes it, set as a scanner
  sets them and read back, and what is not offered refused; the display
  orientation; buffers added before the callback -- two of the right size,
  one too small between them, one larger -- filled in order with the small
  one's frame given as null and nothing after the last, every byte of each
  right, their numbers rising, all on the `HandlerThread`; the preview on
  the `SurfaceView`, where the test finds the picture turned 90 degrees and
  scaled; autofocus, and its cancelling; the torch through the parameters;
  `setPreviewCallback` with a slow callback (new arrays, numbers with gaps,
  nothing after it is taken away); the one-shot callback; stop and start
  again; release and use after release; and the front camera opened on a
  thread with no looper (its callbacks on the main thread), fixed focus
  answering at once, no flash modes, its preview on the `TextureView`, where
  the test finds the picture mirrored and then turned -- the `TextureView`
  framed as VK mp3 mod frames its QR scanner's (corners painted white with a
  rounded rectangle toggled to inverse, markers in a layer), so that the
  test finds the picture under the frame, the frame's canvas the frame's
  size, and its white corners. And once more with
  every camera refused (14 checks): the first open fails with "Fail to
  connect to camera service" and the core's log line says the user or the
  system refused it.

camera2, which CameraX speaks and nothing else (urent's QR scanner: a
CameraX `Preview` on a `PreviewView` and an `ImageAnalysis` handing each
image to Huawei's ScanKit), is over the same natives:
- `camera2/CameraManager.java` lists the cameras by index ("0", "1"),
  answers their characteristics and opens them;
  `camera2/impl/MeglerCameras.java` makes the characteristics from what the
  camera says when opened once: hardware level LEGACY, the capability
  BACKWARD_COMPATIBLE (CameraX drops a camera without it), facing,
  orientation, a stream map of the camera's sizes for YUV_420_888, NV21,
  JPEG and the private format, its frame rates, focus modes, the torch.
- `camera2/impl/MeglerCameraDevice.java` is the device and its sessions, one
  stream at a time as a LEGACY camera has it: the size of the session's
  `ImageReader` when it has one, else the camera's nearest to 1280 x 720.
  Each picture answers the next request queued by `capture`, else the
  repeating one, and goes to the request's targets: an `ImageReader` gets it
  as an image, a `SurfaceView` or `SurfaceTexture` as a bitmap turned
  upright (mirrored for the front camera). `PreviewView` uses a
  `TextureView` on a LEGACY camera and lays it out at the stream's size,
  then scales the view, so the upright bitmap squeezed into it comes out
  right. The result is a `TotalCaptureResult` whose `SENSOR_TIMESTAMP` is
  the image's timestamp (CameraX's `MetadataImageReader` pairs images and
  results by it), with the focus state following `CONTROL_AF_MODE` and
  `CONTROL_AF_TRIGGER`, exposure and white balance CONVERGED, and
  `FLASH_MODE` TORCH lighting the torch.
- `android/media/ImageReader.java` in place of the stub (null surface, no
  images): NV21 buffers the camera fills, given as YUV_420_888's three
  planes as Android's devices give them (Y pixel stride 1, U and V 2, row
  stride the width), `maxImages` enforced.
- The metadata classes (`CameraCharacteristics`, `CaptureRequest`,
  `CaptureResult`, `CameraCaptureSession`'s state callback, and
  `params/StreamConfigurationMap`, `OutputConfiguration`,
  `SessionConfiguration`) are written from the generated stubs by
  `tools/compat/camera2-overlays.py`: each key a real key with Android's
  name, values kept by name.
- **Not done:** an `ImageReader` of another size than the stream (it gets
  no pictures, said once in the log), JPEG captures, reprocessing,
  high-speed and custom sessions (`onConfigureFailed`), zoom, regions,
  manual sensor control, and a display turned from 0.
- The test app `camera`'s step "camera2": two cameras, facing and
  orientation, 640 x 480 among the YUV sizes; the back camera opened, a
  session of a 640 x 480 `ImageReader`, a repeating request with a tag;
  three images with the stand-in's picture in their planes (its number,
  the red top left and white bottom right in YUV), the strides, and each
  image's timestamp among the results' `SENSOR_TIMESTAMP`s; closed.

The shell's side is `player/CameraHost.cpp`, over Windows.Media.Capture
(docs/ARCHITECTURE.md, "Cameras"): the cameras found as media frame source
groups, opened on the UI thread where Windows asks the user the first time,
NV12 pictures from a frame reader, focus and the torch. `tools/build-tests.sh
win` builds it into the test program and runs `mg_tests --camera-windows`:
the PC's first camera through the host and the core's camera code, ten
pictures at its smallest size taken in order and copied and drawn, focus and
the light where it has them, nothing after stop (a PC without a camera
says so and checks nothing). This PC's one camera is DroidCam's virtual one
("DroidCam Source 3", a source group with no panel, so a camera at the back
turned 0), which with no phone behind it streams a flat orange: opened at
640 x 480 and 30 frames a second, given as RGB24 and turned into NV12 by the
frame reader, ten pictures in order, no focus, no light, 9 checks; the last
picture, written to the scratch folder as `camera-0.bmp`, is that orange.
Not tried on a phone.

### 3. Apps that are not the test apps

What stands between this and an app from a shop, as far as reading tells
(*none of it tried*):

- **Native libraries with Java.** Done: "An app's native libraries" above.
  What a particular library asks of `libandroid` and the C library beyond
  what the test app does is found by running it.
- **Kotlin.** No Kotlin compiler is on this PC, so nothing of it was run. A
  Kotlin app brings its standard library in its own DEX, which is ordinary
  bytecode: `kotlin.jvm.internal.Intrinsics`' checks on every parameter,
  lambdas as classes (d8 leaves no `invoke-custom` at a `minSdk` an app really
  has). Coroutines lean on `AtomicReferenceFieldUpdater` and so on `Unsafe`'s
  compare-and-swap, on `ThreadLocal`, and on finding the main dispatcher,
  which is by `ServiceLoader` over `META-INF/services` in the APK (the app's
  class loader finds those now) or by `Class.forName` of a known name; all of
  which exist here and are tested singly, not together. Kotlin's reflection
  reads the `kotlin.Metadata` annotation, which is a run-time annotation with
  string arrays: read here.
- **The big libraries**: an HTTP client needs sockets, which are here, and
  TLS, which is conscrypt's where its library is in the root; a JSON library that reflects on generic field types needs
  `Signature`, which is; dependency injection that generates code needs
  nothing; one that scans a `DexFile`'s class names gets them.
- **A collector**, before any of them runs for long (below).

### 4. Threads and memory in 2 GB

A 32-bit process here has 2 GB of addresses for everything: ANGLE, textures,
the app's native heap, the libraries, and Java. Hollow Knight alone left 690 MB
of them free. Three things follow.

**Threads are cheap here and must stay so.** A Java thread costs a host stack
and a frame stack. The host stack can be small, since Java frames are not on it:
160 KB (or what a thread asks for above that, for native code that wants a deep
stack). `mg_thread_whole_stack` commits it whole for Android code, so it is
memory, not only addresses, which Windows counts against the app's limit. It
once gave 1 MB; the interpreter recurses in C only where C stands between two
pieces of bytecode (`mg_dvm_invoke`), and the guard there (`c_stack_low`, 48 KB
above the bottom) turns running out into a catchable `StackOverflowError` --
except where native code and Java call each other deeper than that, as Meta's
FlexLayout does to measure a layout (Instagram, Messenger and Facebook Lite
stopped at "22 calls from native code deep"): on 32-bit ARM the call goes on in
a stack of 512 KB of its own, made then and let go when it returns
(`on_more_stack`, the stub `on_stack` of `mg_stubs.S`), whose part below the
collector scans as well (`outer_lo`, `outer_hi`); the natives test app runs
such a layout 150 deep, with collections at its bottom. The
deepest any thread's host stack was seen to use, measured over a heavy app of
213 threads (`java: addresses by kind` reports it, and the data app's run says
it): 23 KB; the data app itself 7 KB. So 256 KB was a tenfold margin and 768 KB
a thread less than the 1 MB before it -- Yandex Maps' 253 threads held 253 MB
of host stacks, then some 65 (package 498 → 499) -- and 160 KB still leaves
112 KB above the guard (c7053436). The frame stack is 384 KB of
addresses for an ordinary thread, reserved at once and committed as it grows:
64 KB first, doubled as frames need it (`stack_grow`). A thread's state is not
freed when the thread ends yet; the host layer has no hook for that.

**The runtime's own tables are kept small.** What it keeps of DEX files and
linked classes is counted by kind (`mg_dvm_meta_alloc_kind`;
`java: addresses by kind` and the Java tests' runs say it): classes, methods,
fields, vtables, the DEX files' tables (each index's cache), names, and the
class names of files registered and not viewed yet. A buffer that is freed
once used (a name looked up by its descriptor, a prototype's signature before
it is interned) is not counted; it was, and the count grew with every lookup.
Measured on avito under qemu (213 threads, 50,000 classes linked), 126 MB at
first: a method was 128 bytes, 40 of them the profile's counts, which are now
apart (`mg_dvm_prof`, made the first time the profile counts the method): 88
bytes, and the methods 42 MB to 29. A lazily registered DEX file's class names
are copied while the file is not viewed (the table of definitions points at
them); once it is viewed its entries point at the file's own again and the
copies go (`mg_dvm_classes_viewed`), and every file of a big app is viewed
within seconds. And names, a million of them, small and kept for good, are
laid end to end in blocks of 256 KB, without the heap's header and rounding
of each: names 35 MB to 9, the tables 126 MB to 87. What is left is the
methods, the DEX files' tables (28 MB) and the classes (11 MB).

**Objects have to be freed.** A process that never collects is a
demonstration. Measured on `hello` under qemu, with the runtime's own counts
(instructions counted by `mg_dvm_count_instructions`):

| | pictures | objects made | bytes | instructions |
|---|---|---|---|---|
| three seconds with nothing touched | 0 | 0 | 0 | 14 thousand (371 turns of the main loop) |
| ten presses of the button | 30 | 3493 | 268 KB | 419 thousand |
| for each picture of those | | 116 | 9.2 KB | 13 thousand |

An app that waits allocates nothing at all: the main loop's poll returns null
and its wait is a monitor. A press costs 27 KB (three pictures: pressed, the
changed text, released). So with the 40 MB the app holds once started, **500
MB is reached after some 17,000 presses** -- hours of use -- **or after 14
minutes of anything that redraws at 60 pictures a second** (9.2 KB a picture
is 550 KB a second), and sooner for a screen with more on it than five views:
a scrolling list was not measured. An animation is what makes the collector
urgent; a form is not.

Thirteen thousand instructions a picture is a fifth of a millisecond of
interpreting on the phone at the rate expected below; what a picture costs
there will be the shell's drawing and the copy of 1.5 MB.

The collector planned (superseded in round 7 by "The collector" under Design,
which scans native stacks conservatively and stops threads where they are,
instead of the exact native roots and safepoints below):

- *Non-moving mark and sweep, stopping the world.* Non-moving because pointers to
  objects are everywhere outside the runtime's sight: every `jobject` native
  code holds is the object's address, `Get...ArrayElements` hands out the
  array's own storage, an object's identity hash is its address, and
  `Unsafe` works on addresses.
- *The heap as the runtime's own regions* (`mg_mem_reserve`, a megabyte or a few
  at a time, size classes inside), not the C heap: so that "is this word the
  address of an object" is a bitmap lookup, mark bits sit beside the heap rather
  than in every header, and empty regions go back to the system.
- *Exact inside the heap*: a class knows which of its slots are references (the
  field's type says), an object array is all references, static fields the
  same.
- *Conservative for the interpreter's registers*: a thread's frame stack is
  scanned word by word, and a word that is the address of an object keeps it. An
  int that looks like a pointer keeps something alive a little longer, and in
  exchange nothing has to know which register holds what at which instruction,
  which would take a verifier.
- *Exact for native code*: a table of global references (`NewGlobalRef` records
  them, which `mg_jni.c` does not do today) and, per thread, of the objects JNI
  calls have handed out since the native method was entered, dropped when it
  returns. Native stacks are not scanned.
- *The roots that exist today and are nowhere written down as roots*: each
  thread's frame stack and its pending exception and `Thread` object; every
  linked class's statics and its `Class` object; interned strings and each DEX
  file's resolved string constants; the JNI layer's C-only classes' own fields
  and statics (`mg_jobj.fields` of the stand-ins), which hold runtime objects;
  what the drivers keep in C (the Java driver's clipboard string, the app's
  class loader, the `Method[]` of every proxy class, monitors' owners); and
  the handles of `megler.Gfx`, which are C memory the Java objects point at and
  which must be freed when those die -- the framework frees bitmaps in
  finalizers, so finalizers come with it.
- *Safepoints* at backward branches, calls and allocation: the check every
  instruction makes today for the step limit (`g_dvm.slow`) is the place.
  A thread in native code is stopped where it next calls JNI.
- With it: the daemons, finalizers, `Reference` and `ReferenceQueue`, and
  libcore's `NativeAllocationRegistry`, without which the native memory behind
  every `Bitmap` is never freed.

**The header has to shrink.** 48 bytes an object is 24 MB for half a million
objects. Every read of an object's fields is behind a function now, so
`mg_jobj` can lose `fields`, `nfields`, `refs` and `permanent` for the runtime's
objects and come down to 16 bytes; the C-only world keeps the full one.

### 5. Speed

*Measured in round 8* ("The interpreter's benchmark" above has how):
- On the phone (0.1.0.139, Apktool M, frame meter): the start interpreted at
  71.5 M instructions a second, and steady use at 10 to 30 M;
- "other native methods" took 5 to 30% of Java's time.

Round 8 did, in order, each with its own numbers in its section and commit:
- the meter's clock reads (natives, allocation);
- a call's frame without the C library's memset and memcpy;
- intrinsics;
- trivial methods;
- allocation without atomics;
- `slow_step` out of line;
- the two loops;
- a JNI call's argument words.

The interpreter's place in the program matters as much as any of these:
where its loop falls moved x64 by up to 70%.

x64 `/O2`, the interpreter at the same place in both builds, ns a turn,
before the round (9f2001d) and after (44fa81f):

| item | before | after | |
| --- | ---: | ---: | ---: |
| int arithmetic | 12.5 | 7.7 | 1.6x |
| long and double arithmetic | 21.1 | 16.5 | 1.3x |
| iget and iput | 21.7 | 14.5 | 1.5x |
| sget and sput | 10.0 | 7.1 | 1.4x |
| invoke-static | 17.1 | 14.5 | 1.2x |
| invoke-virtual | 19.7 | 17.1 | 1.2x |
| invoke-interface, two classes | 28.2 | 22.3 | 1.3x |
| aget and aput | 20.4 | 13.6 | 1.5x |
| String.charAt and length | 63.4 | 12.7 | 5x |
| String.equals, 23 characters | 2899 | 14.3 | 200x |
| String.hashCode, kept | 43.2 | 10.5 | 4x |
| StringBuilder, 3 appends | 1201 | 738 | 1.6x |
| HashMap.get | 336 | 235 | 1.4x |
| ArrayList iterator | 92.5 | 69.6 | 1.3x |
| a small object made | 98.6 | 46.6 | 2.1x |
| an int[16] made | 69.5 | 28.2 | 2.5x |
| synchronized block | 19.2 | 14.8 | 1.3x |
| exception made, thrown, caught | 389 | 282 | 1.4x |
| Math.min, max, abs | 55.5 | 23.2 | 2.4x |
| System.arraycopy, 16 ints | 62.0 | 14.2 | 4.4x |
| Integer.valueOf | 131 | 70.9 | 1.9x |
| getClass and casts | 55.9 | 36.7 | 1.5x |

Under qemu, which is noisier and is not the phone: int arithmetic 136 to 130,
invoke-static 268 to 170, String.equals 28195 to 245, a small object 1277 to
632, HashMap.get 4495 to 2609, Math.sin by JNI 817 to 688; in all 25.1 to
32.9 M instructions a second. The phone's Self-test now prints its own table.

Starts under qemu (the real workload): Apktool M to its splash screen at rest,
501 M instructions in 26.2 s before and 471 M in 18.1 s after; hello 4.7 M in
0.64 s and 4.1 M in 0.56 s; demos 5.4 M in 0.74 s and 5.0 M in 0.70 s.

What did not pay:
- Moving the field and array instructions' throwing out of line
  (`field_slow`, `array_refused`): every item got slower by 5 to 30%, since the
  compiler laid the loop out worse. Not kept.
- Taking the instruction counter out: 3 to 8%, and the meter needs it. Kept.

What is next, cheapest first:
1. **Link the package with the interpreter in a fixed place** (`/ORDER`, as
   the benchmark's build does). Until then the phone's speed can move by tens
   of percent between builds for no reason in the code. It is a line in
   `tools/build-appx.sh`.
2. **A thin lock** for the uncontended monitor: one compare-and-swap of a word
   in the object, a monitor made only when another thread wants it or `wait`
   is called. Under qemu a synchronized block costs 1100 ns against 15 on
   x64. The phone's table will say what it costs there.
3. **Quickening** of field access and invokes into a side table per method
   (the DEX is used where it lies, and other threads read it). x64 shows iget
   and iput at about 1.5 ns an instruction against arithmetic's 1.1.
4. **An inline cache** for invoke-interface: two classes at a site cost 22.3
   ns against invoke-virtual's 17.1.
5. Common pairs of instructions as one.

**A JIT is possible and should wait.** Megler can make executable memory, but
only by turning a written page into an executable one (56 to 82 µs a pair of
flips on the phone, plus the cache flush), never both at once. Code generated in
batches and never changed afterwards fits that; code that patches itself (inline
caches in the instruction stream) does not, and must keep what changes in data. A
template compiler -- each Dalvik instruction as a fixed run of Thumb-2 over the
register array, a call into C for anything hard -- would give perhaps three to six
times the interpreter on loops and arithmetic. It needs a Thumb-2 encoder, faults
in generated code told apart by the fault handler, and a way to debug it on a
device that is only seen through its log. It is worth that when an app is known
to be held back by the interpreter and the cheaper things are done; not before
apps run.

## Existing files that changed, and that will

Changed for the second milestone, outside `core/src/dvm`:

| File | What |
|---|---|
| `core/src/jni/mg_jni.c`, `mg_jni.h` | the join: "One Java world" above; `mg_jobj.dvm`; `mg_jni_native_counts` |
| `core/src/host/mg_host.h`, `mg_host_win32.c`, `mg_host_posix.c` | `mg_atomic_cas64` |
| `core/src/bionic/mg_bionic.c` | `libopenjdkjvm.so` is a system library |
| `core/src/bionic/mg_libc_io.c`, `mg_libc_misc.c`, `mg_net.c` | the names libcore's libraries import that were missing: the 37 of `build/android9/lib/report/layer-missing.txt` and a few before that list was made |
| `core/tests/mg_tests.c`, `mg_tests.h`, `mg_tests_main.c` | the runtime's tests in the core's; the reports' modes in the command-line program |
| `core/tests/guest/t_jni.c`, `tools/build-guest.sh` | the guest library for the join's tests |
| `tools/build-tests.sh` | the fixtures first; the second run on libcore |
| `tools/build-appx.sh` | the fixtures in `selftest/dvm`; `--libcore` |

Changed for the third, outside `core/src/dvm`:

| File | What |
|---|---|
| `core/include/megler.h` | `megler_gfx_host`; `megler_window_host.present_pixels`; `megler_run_config.gfx` and `.android_root` |
| `core/src/drivers/mg_driver.c`, `mg_driver.h` | list the Java driver (`mg_java.c`, `mg_java_gfx.c` are new) |
| `core/src/apk/mg_apk.c`, `mg_apk.h` | an APK that is Java alone is runnable; how the launcher activity asks to be held (`"orientation"` in the report) |
| `core/src/bionic/mg_net.c` | asking whether a descriptor is a socket is answered "it is not", quietly (libcore asks it of every file it closes) |
| `core/tests/mg_tests.c`, `mg_tests.h`, `mg_tests_main.c` | the drawing's tests in the core's; the Android root looked for in the package; `--java` |
| `tools/build-tests.sh` | the two Java apps under qemu-arm |
| `tools/build-appx.sh` | `--libcore` puts the Android root at `android` in the package (it was `selftest/android`) |
| `tools/dvm-android-root.sh` | the framework's DEX and the platform's resources in the root |

Still to change, for the steps above:

| File | Why |
|---|---|
| `core/src/jni/mg_jni_android.c`, `mg_jni_loop.c`, `mg_jni_prefs.c` | what they implement in C exists in Java in the framework; for a Java app they serve as natives under it or not at all, and stay as they are for the engine drivers |
| `core/src/apk/mg_apk.c` | (done) an APK with Java and native libraries that no engine was recognised in is `java` |
| `core/src/run/mg_run.c` | it refuses every app where guest code cannot run; a Java app without native libraries needs none (only the PC's tests meet this) |
| `core/src/host/*` | something called when a thread ends |
| `core/src/bionic/mg_libc_mem.c`, `mg_net.c` | a shared mapping for a file mapped twice; (done) sockets |
| `core/src/ld/mg_ld.c` | (done) libraries told apart by where they came from |
| `app/Megler` | `megler_gfx_host` and `present_pixels`; a Java app's page and state; the keyboard and the rest of what `megler.Native` asks of a host; the Android root in the ordinary package or unpacked once |
| `docs/STATUS.md`, `docs/ARCHITECTURE.md`, `README.md` | they describe the first milestone |

## What is on disk, and what would still need a download

`build/android9` (not in the repository; `tools/android9-*.sh` made it) has
Android 9's boot class path as DEX files (`boot/`), the system image's
`framework` folder, ICU's data and the time zones (`system/`), libcore's and
ART's sources at `android-9.0.0_r47` (`aosp/`), and the nine native libraries
built or taken for 32-bit ARM (`lib/`: conscrypt's `libjavacrypto.so` is built,
`libcrypto.so` and `libssl.so` are Android 9's own from the VNDK snapshot). Everything above runs from those, from
the Java framework's sources in `core/framework`, and
from what the build scripts make of them: `build/framework/megler-framework.dex`
and `build/guest/java/*.apk`.

Not there:

| What | For |
|---|---|
| R8 8.5 or later (Google's Maven, `com.android.tools:r8`, 10 to 15 MB) | only if the local d8's output of the framework proves unusable |
| SQLite's source (sqlite.org, `sqlite-amalgamation-3540000.zip`) | (there: `build/sqlite`, fetched by `tools/sqlite-fetch.sh`) |

## On this PC

Things found while working that the plan leans on:

- A JDK with `javac`: `C:\Program Files\Java\jdk1.8.0_202`.
- An Android SDK in `C:\Users\User\Documents`: build-tools 22.0.1, 28.0.3 and
  29.0.3 (`d8.jar`, `dx.jar`, `dexdump.exe`), platforms 23, 29 and 30
  (`android.jar`), and NDKs r10e, r17c, r19c and r21e.
- `smali` and `baksmali` 3.0.5 are in `Downloads\BatchApkTool\bin`. They were
  not run (a third-party bundle); the fixtures are in smali's syntax so that they
  can be.

The tests do not depend on any of these: the fixtures are assembled by
`tools/dexasm.py`, and the two programs javac and d8 made are kept as they made
them.
