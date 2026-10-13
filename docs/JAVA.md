# Java: running an app's own bytecode

An ordinary Android app is DEX bytecode; its native libraries, if it has any, are
called from that. Everything else in this port runs apps whose code is native and
plays their Java side in C (see [ARCHITECTURE.md](ARCHITECTURE.md), "The Java
side"). This document is about the other kind: a runtime that executes the
bytecode itself.

Three milestones of it are written and tested. The first was a DEX reader and a
Dalvik interpreter with a small `java.lang` of its own. The second joined it to
the JNI layer and put **Android 9's real libcore** under it, with libcore's own
native libraries (32-bit ARM, loaded by this port's linker) doing the native
work. The third put an Android framework on top -- the original Megler's, built
here from its sources -- and a driver that starts an app with it: **an APK that
is Java alone starts through `megler_run`, its `Application` and `Activity` run,
its layout is inflated from its resources, measured, laid out and drawn, the
picture reaches the window host, a touch runs a listener, and Back takes it
down.** That is *proven* under qemu-arm, with a stand-in for the shell's
drawing.

**Nothing of it has run on a phone**, and the shell has not yet got its half:
the drawing (`megler_gfx_host`: Direct2D, DirectWrite and WIC behind a C
interface) and the showing of a picture that is memory, not a texture. The
phone's package builds with all of it.

As in the other documents, **proven** means a test that was run shows it, and
where; **expected** and **not verified** mean what they say.

## What is there

| | |
|---|---|
| `core/src/dvm/hk_dvm.h` | the interface: start (two ways), add DEX files, register native methods, call, exceptions, tracing |
| `core/src/dvm/hk_dvm_internal.h` | the structures (class, method, field, object, frame, thread) and how registers hold values |
| `core/src/dvm/hk_dvm_dex.c` | reading a DEX file: header, checksum, tables, strings, line numbers, instruction lengths, a method's annotations |
| `core/src/dvm/hk_dvm_class.c` | which file defines which class, linking, vtables, lookups, resolution, static initialisation, binding native methods |
| `core/src/dvm/hk_dvm_object.c` | objects, arrays, strings, Class objects, monitors, a thread's state, exceptions |
| `core/src/dvm/hk_dvm_interp.c` | the interpreter: frames, calls, every instruction, exception delivery |
| `core/src/dvm/hk_dvm.c` | the interface's functions; the two ways to start |
| `core/src/dvm/hk_dvm_apk.c` | `classesN.dex` out of an APK |
| `core/src/dvm/hk_dvm_boot.c`, `boot/*.smali`, `hk_dvm_boot_dex.h` | the first way to start: a `java.lang` of 66 classes, in smali and C, built in |
| `core/src/dvm/hk_dvm_art.c` | the second way: what Android's runtime implements of libcore inside itself (`art/runtime/native/*.cc`), in C: `Object`, `Class`, `String`, `StringFactory`, `Thread`, `Throwable`, `System.arraycopy`, `VMRuntime`, `VMStack`, `Unsafe`, references, arrays |
| `core/src/dvm/hk_dvm_art_reflect.c` | `java.lang.reflect`: `Field`, `Method`, `Constructor` and the `Class` methods that make them |
| `core/src/dvm/hk_dvm_art_host.c` | what stands in for libcore's native libraries where they cannot run (a PC): a few dozen methods, enough to start and to run what needs no ICU |
| `core/src/dvm/hk_dvm_jvm.c` | loading libcore's native libraries, and `libopenjdkjvm.so`'s `JVM_*` functions for them |
| `core/src/dvm/hk_dvm_art_annot.c` | annotations as reflection reads them, and what a DEX file keeps as annotations of its own (nesting, generic signatures, declared exceptions) |
| `core/src/jni/hk_jni.c` | the JNI layer, which is now also the runtime's JNI: see "One Java world" below |
| `core/src/drivers/hk_java.c` | the driver for an app that is Java: starts the runtime, the framework and the app; `megler.Native` (the log, the APK's files, the screen, input, the end) |
| `core/src/drivers/hk_java_gfx.c` | `megler.Gfx`: bitmaps as memory, and `android.graphics` handed to the shell through `megler_gfx_host` |
| `core/include/megler.h` | `megler_gfx_host`, the contract for the shell's drawing; `megler_window_host.present_pixels`; `megler_run_config.android_root` |
| `core/framework-overlay/` | what differs here from the original's Java framework: six files, each saying at its top what differs and why |
| `core/src/dvm/art/*.smali`, `hk_dvm_art_dex.h` | two classes the runtime adds to libcore's: a security provider that stands in for conscrypt's |
| `tools/build-framework.sh` | builds the framework's DEX from the original's sources and the overlay |
| `core/tests/app/`, `tools/build-test-app.sh` | two test apps as ordinary Android apps (aapt2, javac, d8), and their build |
| `core/tests/hk_java_tests.c` | the test of the driver: the apps started through `megler_run`, their log and their pictures checked |
| `core/tests/hk_gfx_standin.c`, `hk_gfx_tests.c`, `hk_gfx_natives_tests.c` | a stand-in for the shell's drawing (no anti-aliasing, text as boxes, PNG), and the tests of it and of `megler.Gfx` |
| `core/tests/hk_dvm_tests*.c`, `core/tests/dvm/` | the tests, part of the core's own (`hk_tests_run`, so the phone's Self-test too); 39 `.smali` fixtures that make four DEX files; two programs compiled by javac and d8, kept with the JVM's answers beside them |
| `core/tests/guest/t_jni.c` | a guest library with native methods, for the tests of the join |
| `tools/dexasm.py` | a DEX assembler for a subset of smali, a structural checker, a lister |
| `tools/dvm-ops.py`, `tools/dvm-boot.py` | generate the arithmetic tests and their answers; the built-in `java.lang` as a C array |
| `tools/build-dvm-fixtures.sh` | regenerates the generated sources and puts the tests' DEX files in `build/guest/armeabi-v7a/dvm` (`tools/build-tests.sh` and `tools/build-appx.sh` run it) |
| `tools/dvm-d8-fixture.sh` | rebuilds the two javac-and-d8 programs and the JVM's answers for them |
| `tools/dvm-android-root.sh` | puts together the folder libcore is started from, out of `build/android9` |
| `tools/dvm-report.sh`, `tools/dvm-natives.py` | the measurements this document quotes |

The runtime is compiled into the core like any other module. An app with a
native engine does not pay for it: nothing of it runs until `hk_dvm_start` or
`hk_dvm_start_android` is called, and only the Java driver calls either.

## What is proven

Two commands. Each runs the whole of the core's tests twice, in two processes
(a process has one runtime and one boot class path): once with the runtime on
its built-in `java.lang`, and once on Android 9's libcore from
`build/android9/root`. Under qemu-arm the second command then starts each of the
three Java test apps through `megler_run`, a process each.

```
bash tools/build-tests.sh win      (MSVC x64, run on the PC; libcore's native libraries cannot run there)
    3414 checks, 0 failed          on the built-in java.lang
    3428 checks, 0 failed          on libcore, its bytecode alone
    (then the shell's own drawing, which is not this document's: 263, 263, 14, 9, 8 and 6 checks)

bash tools/build-tests.sh arm      (gcc for arm-linux-gnueabihf, Thumb-2, hard-float, under qemu-arm)
    3638 checks, 0 failed          on the built-in java.lang
    3677 checks, 0 failed          on libcore with its native libraries
      36 checks, 0 failed          the Java test app hello, through megler_run
      47 checks, 0 failed          the Java test app demos
      42 checks, 0 failed          the Java test app natives: a library of its own, built by the NDK
      36 checks, 0 failed          hello again, on an upright surface (720 x 1280 at 320 dpi)
    (then each app once more, its drawing recorded for the PC's run to draw again)
```

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
7617 classes, 11.4 MB of DEX), and after them two classes of the runtime's own
("The security providers" below): 7619.

**Without libcore's native libraries** (the PC, and the same code as ARM):

- All six files are read and **every one of the 7619 classes links** (in both
  runs: `7619 linked, 0 not`).
- The program that uses `java.lang` alone (`d8t/Main.java`) gives the JVM's
  answers for 9 of its 10 tests, and the one that uses `java.util`, `java.io`
  and `java.text` (`d8t/Lib.java`) for the 7 of its 17 that need no native
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

- The libraries load through `ld/hk_ld.c` with **no import left unprovided**
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
- **Both programs match the JVM**: all 10 tests of `Main` and all 17 of `Lib`,
  which add regular expressions, `String.format`, `DecimalFormat`,
  `SimpleDateFormat` in UTC and in `Europe/Moscow`, `BigInteger` and
  `BigDecimal`, case conversion and character classes outside ASCII, charsets
  (UTF-8, UTF-16LE, ISO-8859-1, US-ASCII, windows-1251), `CRC32`, `Deflater`
  and `Inflater`, files through `java.io`
  (`File`, streams, readers and writers, `RandomAccessFile`) and through
  `java.nio.file` and a `FileChannel`, a second thread with `join`, `wait` and
  `notify`, exceptions the interpreter throws, and `System.currentTimeMillis`,
  `nanoTime`, `getProperty`, `arraycopy`, `identityHashCode`. The JVM's answers
  were made by JDK 1.8.0_202 from the same class files.
- **One test's answer is Android's and not the JVM's**, and
  `core/tests/dvm/d8/lib-android.txt` says which and why: three messages of
  exceptions the runtime itself throws, which Java does not specify and
  Android words its own way -- `divide by zero` (the JVM: `/ by zero`), a
  negative array size as `Dimension 2: -1` (the JVM 8: no message), and an
  index out of bounds as `length=5; index=5` (the JVM: `5`). A difference that
  is not in that file fails the test. There is no other.
- `java.nio.file`'s native half is in no table: `libopenjdk.so` exports it as
  `Java_sun_nio_fs_UnixNativeDispatcher_...`, and those are found by name (a
  check that some were; 15 by the end of the two programs).
- **Who answers each native method** (`bash tools/dvm-report.sh natives arm`):
  the boot class path declares 1237 in 96 classes. The runtime answers 296 in
  C, the libraries 645 (568 they registered, 77 found by their JNI names), and
  296 have no answer: 280 of conscrypt's `NativeCrypto` (its library,
  `libjavacrypto.so`, is not built) and 16 others (`sun.misc.Version` 4,
  `NativeStart` 2, `Package` 2, `TimeZone` 2, `sun.nio.ch.Net` 2, and one each
  of `Runtime`, `MethodHandleImpl`, `Constructor`, a test class). Calling one
  is an `UnsatisfiedLinkError`.
- **Which static initialisers fail with the libraries** (`bash
  tools/dvm-report.sh init arm`): of 7619, 7607 run. `NativeCrypto` itself and
  ten of conscrypt's that follow from it cannot, for want of its library; and
  `android.icu.text.Transliterator` did not finish in the 50 million
  instructions each is given there, which is *not* shown to be a failure or
  not to be one.
- **What apps use of `java.security`** (two more tests of `Lib`, against the
  JVM): `MessageDigest` MD5, SHA-1, SHA-256 and SHA-512, in one piece and in
  several; `SecureRandom` (bytes, ints, seeds); `UUID.randomUUID()` and the
  name-based kind; `CRC32` and `Adler32`; and one step further, `Mac`
  HmacSHA256, `Cipher` AES/CBC/PKCS5Padding there and back, PBKDF2 key
  derivation, `Base64`. All give the JVM's answers. None of it is conscrypt's:
  see "The security providers".

### A Java app with an Activity (qemu-arm)

`core/tests/app/hello` is an ordinary small app, built the ordinary way
(`tools/build-test-app.sh`: aapt2, javac against the SDK's `android.jar`, d8;
not signed): a manifest, an `Application` subclass, one `Activity`, a layout in
its resources (a `LinearLayout` with a `TextView`, a `Button`, a view of its own
that draws with `Canvas`, and an `ImageView` of a PNG), strings and colours. It
has no native library. `hk_tests --java hello ...` starts it through
`megler_run`, on a 480 x 800 screen at 160 dpi so that a dp is a pixel, with a
stand-in for the shell's drawing, and checks (36 checks):

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
- **The layout**, as the app reads it back: every view the full 480 wide, the
  button 64 high, the drawn view 100, the picture 64 square, one under the
  other.
- **The first picture**, on pixels: the drawn view's blue field, its red
  rectangle at (10,10)-(50,30) and nothing just outside it, its green one moved
  by `translate`; the PNG stretched from 16 to 64 pixels with its orange and
  green halves and the layout's grey showing through its transparent border;
  black boxes where the text is.
- **A touch on the button** (down and up through `megler_touch`): the
  `OnClickListener` runs, the text becomes `pressed 1`, the drawn view is
  invalidated, and the next picture shows both -- the green rectangle twice as
  wide, and exactly eight letters' worth of black where there were ten. A
  touch beside the button is no press.
- **Pause and resume** from the shell (`megler_set_paused`): `onPause`,
  `onResume`.
- **Back**: `onPause`, `onStop`, `onDestroy` in that order, the main loop
  ends, and the app is reported ended with status 0.

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
  an app was tried there; "What differs from the original's framework" below
  says why.
- A row held down shows that it is pressed inside its own rectangle and
  nowhere else (it was a grey disc as wide as the screen, over the rows above
  and below; the same section says why).

It needs the platform's resources: without `framework-res.apk` in the root it
stops at the row layout's id (`Resources$NotFoundException`).

**On the PC**, where libcore's native libraries cannot run (and `megler_run`
refuses every app, since guest code cannot run; the test starts the driver
itself there: `hk_tests.exe --java hello <apk> <data dir> build\android9\root`),
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
`tools/build-tests.sh` says that it is not run. `hk_tests --java natives ...`
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

and, from outside: the library's own `__android_log_print` line, and the bitmap
it painted on the picture, pixel for pixel where the app drew it. The picture
is `build/java-pictures/natives-1.bmp`.

The linker's part has a test of its own in the core's tests (`test_guest_kinds`
in `core/tests/hk_tests.c`, 15 checks, both ARM runs): two libraries named
`libt_same.so`, one in the folder that stands for the app's and one in a folder
that stands for the Android root's, and a library of each kind that needs
"libt_same.so". The runtime's are loaded first. Each gets the one of its own
kind; a weak import of a name that only the other kind exports stays unbound on
both sides; `dlsym(RTLD_DEFAULT)` sees the app's only; neither kind can be
loaded from the other's place.

### The phone's package

`bash tools/build-appx.sh arm --libcore` puts the Android root
(`build/android9/root`) in the package as the folder `android`: 20 files,
58,892 KB, of which ICU's data is 24 MB, the platform's resources 12 (cut down;
Android's own file would be 44), the six DEX files of libcore 11, the
framework's 7.6, the libraries 4.5. `megler_run` finds the folder there for a
Java app, and the Self-test finds it and runs the runtime's tests on libcore
exactly as the second run under qemu does. (The package was 37,745 KB with the
whole resource file; it has not been built here since the file was cut.)

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
such a thing (`hk_dvm_profile`: for every method, the instructions executed in
it and under it, its calls, the objects made in it, and for a native method
the time), and the test of `demos` puts a step under it when asked:

```
HK_JAVA_PROFILE=dialog qemu-arm ... hk_tests --java demos <apk> <data dir> <android root> -      (or start, or scroll)
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

**What was wasteful, in the original's framework Java** (both are fixed in the
overlay, and the original has both):

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

Measured by the runtime's own counts, in the 32-bit build under qemu
(`hk_mem_usage` gives nothing in a test program, so what the *process* takes was
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
`bionic/hk_libc_mem.c`.

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
- The interpreter's speed on this PC (a Ryzen 7 7800X3D; `hk_tests --dvm-bench
  <fixtures>`): 372 million instructions a second in a counting loop and 40
  million calls a second, as the tests are built (`/O1`); 473 and 52 million
  with `/O2`, which is how the app is built. Under qemu-arm the figures (48 and
  4 million) measure qemu. These were taken at the first milestone and not
  again.

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
*and type*: obfuscators give fields of different types the same name. (The
original looks by name alone.)

There is one name space and one class path: two classes of one name cannot be,
and no class is loaded *through* a loader. Class loaders exist all the same, as
libcore's own objects, because apps hold and ask them:

- The system's classes (libcore, the framework) have a null `classLoader`
  field, which libcore reads as its `BootClassLoader`.
- An app's classes name the app's loader: the framework makes a real
  `PathClassLoader` over the APK and hands it to the runtime
  (`hk_dvm_set_app_loader`), which writes it into the `Class` object of every
  class of the DEX files added after `hk_dvm_app_follows`.
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

An object is one allocation: the JNI layer's `hk_jobj`, then three words of the
interpreter's (its class, its monitor, its size), then its contents -- instance
fields as 8-byte slots, or an array's elements, or a string's fields and then
its UTF-16.

So **a pointer to an object is an `hk_jobj *` as native code and the JNI layer
already know it**: `kind` says what it is, `u.arr.data`/`u.arr.len` are an
array's elements (pointing into the same allocation), `u.oarr.v` an object
array's, `u.str.u16`/`len16` a string's text, and for a Class object `kind` is
`HK_J_CLASS`. `permanent` is set, so the JNI layer's reference counts leave these
objects alone, and `dvm` says the runtime made it. A string's `u.str.utf8` is
NULL until someone asks for it, since Java works in UTF-16.

The choice was between this and an object of the interpreter's own that the JNI
layer would wrap. Being the same object means no table between the two worlds, no
second identity for anything, and that everything already written against
`hk_jobj` -- `GetArrayLength`, `GetStringUTFChars`, the drivers' own reads --
goes on working on objects bytecode made. Its cost is the header: 48 bytes on the
phone (36 of them `hk_jobj`), 88 on a 64-bit PC, where Android's own is 8.

Slots are 8 bytes whatever the field's type. An `int` field wastes four; in
return a long or a double needs nothing special, and a field's place is the
header plus its slot number times eight for every type, which is what
`Unsafe.objectFieldOffset` hands libcore.

### Registers and frames

A register is one machine word (`hk_dvm_reg`, a `uintptr_t`): 32 bits on the
phone, 64 on a PC. An int, a float's bits and the small types sit in the low 32
bits with the rest zero; a reference is the pointer; **a long or a double is in
two registers, low half first, on both hosts**. On the phone that is Dalvik's own
frame layout and costs nothing. On a 64-bit host each half sits in a word twice
its size, which wastes room and means the PC's tests take the same path through
every wide instruction as the phone will. (The original keeps a 64-bit word per
register and a whole long in the first of its pair, which would double every
frame on a 32-bit host.) The comment at `hk_dvm_reg` in `hk_dvm_internal.h` is
the authority.

Frames are not C frames. Each thread has a stack of its own (196,608 registers:
768 KB on the phone), and a call from bytecode to bytecode pushes a frame there
and carries on in the same C function; a return pops it. So Java recursion costs
words of that stack and none of the host's, a `StackOverflowError` is an ordinary
bounds check, and a thread that runs Java needs no more host stack than any
other. The original's interpreter recurses in C and gives its threads stacks of
32, 64 and 256 MB for it, which a 2 GB address space could not afford.

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
Values become `hk_jvalue` only at the boundary with C.

`new String(...)` is done as ART does it: a string's length is fixed when it is
made, so the constructor's result replaces the object `new-instance` made in
every register that holds it. On libcore the constructor called is not
`String.<init>` (which only throws there) but the `StringFactory.newStringFrom...`
method of the same parameters.

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
`hk_dvm_pending_stack` and libcore's `StackTraceElement`s.

### Static initialisation

A class is initialised at the first `new-instance`, static field access or static
call: the superclass first, then the values the file gives static fields, then
`<clinit>`. A second thread asking meanwhile waits; the same thread asking again
(an initialiser that reaches back to its own class) carries on with the class
half-made, as Java specifies. An initialiser that throws leaves the class
unusable: `ExceptionInInitializerError` the first time, `NoClassDefFoundError`
after.

### One Java world with the JNI layer

Before the runtime is started the JNI layer is what it was: classes, methods and
fields looked up by name, each method implemented in C by a driver or logged
once. The Unity and NativeActivity drivers and `hk_jni_android.c`,
`hk_jni_loop.c`, `hk_jni_prefs.c`, `hk_media.c` run that way and are untouched;
the core's 242 checks of them pass before the runtime starts in the same
process.

Once it is started (`hk_jni.c` asks `g_dvm.running`):

- **Every class the layer knows has a class of the runtime's.** One a DEX file
  defines is the runtime's own, and its methods and fields are the bytecode's.
  One no DEX file defines stays the layer's, implemented in C as before, and
  gets a stand-in in the runtime (a "foreign" class under `Object`) so that its
  objects can be locked, stored in arrays and held by bytecode. Bytecode cannot
  call such a class's C methods; nothing needs that yet.
- **Every object is the runtime's**: `NewObject`, `NewString`, `New<Type>Array`
  and the layer's own `hk_jni_new_*` make runtime objects, and a class's
  `jclass` is its `java.lang.Class` object.
- **One pending exception**, the runtime's; `Throw`, `ExceptionOccurred` and
  the rest work on it.
- `FindClass` links a class and does not initialise it, as Android's does
  (the specification says it initialises; libcore's libraries rely on Android's
  way, looking classes up before their natives are registered). Asking for a
  method or field id initialises.
- `Call...Method` on a method of a DEX class runs the bytecode, virtually or
  exactly as asked; fields are slots. A native method takes up to 64 parameters
  and 120 argument words (it was 32 and 22).
- `MonitorEnter`/`MonitorExit` are the runtime's monitors;
  `NewDirectByteBuffer`, `GetDirectBufferAddress` and `-Capacity` work on
  libcore's own `java.nio.DirectByteBuffer` when it is there.

**A native method is bound** when it is first called, in this order: a C function
registered with the runtime for its class (`hk_dvm_register`, the JNI layer's
own `hk_jhle` table shape: what the runtime implements itself); then a library's
function, registered with `RegisterNatives` or exported under its JNI name
(`Java_pkg_Class_method`, then with `__` and the mangled parameters for an
overloaded one; `_1`, `_2`, `_3` and `_0xxxx` as the specification has them);
then a stand-in (`hk_dvm_register_fallback`), used only where no library
answers. A `RegisterNatives` entry for a method the class does not declare is
said in the log and left out, where Android refuses the whole table. A method
annotated `@CriticalNative` in its DEX file is called with its arguments alone,
no `JNIEnv` and no class, as Android calls it (`java.lang.Math`'s are such).

### On libcore

What libcore expects of the runtime and how it is given (`hk_dvm_art.c`, each
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
  new in the host layer (`hk_atomic_cas64`).
- **Threads**: `Thread.currentThread()` makes the calling thread's `Thread`
  object the first time it is asked (in `ThreadGroup.mainThreadGroup`, by the
  constructor libcore keeps for the runtime). `Thread.start` is
  `nativeCreate`: a host thread that is a guest thread from the start (native
  methods run on it), running `Thread.run`. `join`, `sleep`, `yield`,
  `holdsLock`, the name and the priority work; `interrupt` sets a flag that
  `interrupted` reads and does not wake a thread that waits. **The daemons are
  not started** (`java.lang.Daemons`: finalizers, reference queues, the heap
  task): nothing is collected, so there is nothing for them to do.
- **Reflection** (`hk_dvm_art_reflect.c`): `getDeclaredField(s)`,
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
- **Annotations** (`hk_dvm_art_annot.c`): an app's own with run-time
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
at every app's start (the framework adds a provider of its own first thing),
since conscrypt's library is not built.

So while `lib/libjavacrypto.so` is not in the root, the list is changed just
before that initialiser runs (a hook on class initialisation,
`g_dvm.before_init`): in conscrypt's place a provider of the runtime's own,
`megler.security.RuntimeProvider`, then libcore's, then BouncyCastle;
conscrypt's TLS provider and the two properties that name its socket
factories are taken out. The runtime's provider is two classes in smali
(`core/src/dvm/art`, assembled into `hk_dvm_art_dex.h` and put on the boot
class path after libcore's files). It goes by conscrypt's name, because the
name is asked for: BouncyCastle's own key derivation takes its digests from
"AndroidOpenSSL" and refuses to start on Android without it. Under that name
it offers the six digests -- which are BouncyCastle's own classes, in Java --
and `SHA1PRNG`, which is the system's random source (`hk_random`). MACs,
ciphers and key derivation are BouncyCastle's. There is no TLS, and asking for
it is `NoSuchAlgorithmException`; a cipher asked of "AndroidOpenSSL" by name
is not there.

**The built-in `java.lang` stays** as the first way to start
(`hk_dvm_start(NULL, 0)`). It could be dropped now that every fixture passes on
libcore too; it is kept because libcore is 40 MB that the ordinary package does
not carry, and because the PC's tests must run where `build/android9` has not
been unpacked.

### Starting on Android's libcore

`hk_dvm_start_android(root)`, with `root` a folder laid out like Android's
`/system` (`tools/dvm-android-root.sh` makes `build/android9/root`):

```
framework/core-oj.dex, core-libart.dex, conscrypt.dex, okhttp.dex, bouncycastle.dex, apache-xml.dex
framework/core-oj.jar, core-libart.jar       the jars as the device has them (no bytecode in them)
framework/megler-framework.dex                  the Java framework (tools/build-framework.sh); for an app, not for libcore
framework/framework-res.apk                   the platform's own resources, Android 9's; for an app
lib/*.so                                      libcore's native libraries, 32-bit ARM
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
   `libopenjdkjvm.so` becomes a system library answered by `hk_dvm_jvm.c` (the
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

`core/src/drivers/hk_java.c`. `hk_apk.c` names the engine `java` for an APK
with `classes.dex` and a launcher activity that no other engine was recognised
in, with native libraries or without, and such an APK is runnable when it has
no native code or has it for 32-bit ARM.

- **Where the files are**: `megler_run_config.android_root`, or the folder
  `android` in the package (`megler_config.package_dir`) when that is NULL. The
  folder is only read. A Java app started without it is refused with a
  sentence that says what is missing.
- **The start**, on a thread that stands for Android's main thread (4 MB of
  host stack; bytecode's frames are not on it): `hk_dvm_start_android`, the
  framework's DEX, the APK's (`hk_dvm_app_follows` between the two), the
  natives of `megler.Native` and `megler.Gfx`, and then one call:
  `megler.AppRunner.run(activity, application)`. From there it is the
  framework's own Java, as in the original: it reads the manifest itself,
  prepares the main `Looper`, makes the app's class loader, the `Application`
  and its `Context`, starts the content providers, calls
  `Application.onCreate`, makes the launcher `Activity` and calls `onCreate`,
  `onStart`, `onResume`, and loops.
- **Input is asked for, not delivered.** The framework's main loop calls
  `Native.pollInput` between messages, every 8 ms at most. `megler_touch`,
  `megler_key` and `megler_set_paused` only queue (256 events; a move may be
  dropped when the queue is full, what ends a gesture is not), so everything
  that happens to the app happens on its main thread. Back is one event, when
  the key comes up; other keys pass as they are. `megler_set_focused` has no
  counterpart in the framework and is not passed on.
- **Frames are the framework's business**: its `Choreographer` and `ViewRoot`
  draw when something was invalidated, into a bitmap the size of the screen,
  and call `Gfx.present`. That is `megler_window_host.present_pixels`: the
  picture as memory (B, G, R, A premultiplied, top row first), which the shell
  must copy before it returns. Nothing goes through OpenGL. A picture comes
  when something changed, not at a steady rate.
- **The end**: when its last activity is gone the framework calls
  `Native.exit`, which has the main `Looper` quit; `AppRunner.run` returns and
  the app is reported ended with status 0. `megler_stop` sets a flag the next
  poll sees, which finishes every activity on the main thread, and the same
  follows. An exception that reaches the top of the main thread ends the app
  with status -1 and the exception and its stack as the reason.
- **The APK's files**: `Native.readApkFile` and the asset calls read the
  app's APK (0), the platform's resources (1: `framework/framework-res.apk` of
  the root, when it is there) and the split APKs (2 and up) through
  `zip/hk_zip.c`; a stored file can also be had as a descriptor and an offset.
- **What is not answered yet** answers "no" in a way the framework's Java
  already handles, said once in the log for each class: `megler.Sqlite` (a
  database that cannot be opened: `SQLiteCantOpenDatabaseException`),
  `megler.Audio` (an `AudioTrack` that is not initialised, a sound that did not
  load), `megler.Web` (an empty rectangle), `megler.Sensors` (not available),
  `megler.MediaCodecNative` (not supported), `megler.EGLNative` (no context) and
  the `android.opengl.GLES*` wrappers (nothing happens). Also not asked of the
  shell yet: the keyboard, the clipboard (it is the app's own), sharing,
  opening a URL, insets (zero), dark mode (no), vibration.
- **Which way up.** The surface is whatever `megler_run_config` says, upright or
  on its side, and the framework lays out for it; nothing in the driver
  assumes either. What the app *asks* for is known twice: before it runs,
  from the manifest (`megler_apk_report`'s `"orientation"`: the launcher
  activity's `android:screenOrientation` as `landscape`, `portrait` or
  `unspecified`, an alias taking its target's), and while it runs, when the
  framework calls `Native.setOrientation` for an activity's attribute or for
  `setRequestedOrientation`. The second is only logged: there is no call of
  the shell's to pass it to, and a surface cannot change its size under a
  running app yet.

### An app's native libraries

An APK with `classes.dex`, a launcher activity and native libraries is the
engine `java` like one without (`hk_apk.c`), provided the libraries include
32-bit ARM ones (`armeabi-v7a`, else `armeabi`); one with only `arm64-v8a` or
x86 libraries is refused with a sentence that says so. The engines that are
recognised by their libraries (Unity, a NativeActivity and the rest) are looked
for first and keep their drivers.

**Two kinds of library** (`ld/hk_ld.h`, "The runtime's libraries"). The
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

**`System.loadLibrary`** is libcore's own Java all the way to
`Runtime.nativeLoad`. `VMStack.getCallingClassLoader` gives the loader of the
class that called -- the app's `PathClassLoader` for a class of the app's --
whose `findLibrary` looks in `ApplicationInfo.nativeLibraryDir`
(`/data/data/<package>/lib`) and opens the file to see that it is there. The
Java driver has put each of the APK's libraries there as a real file when the
app starts (`place_libraries` in `hk_java.c`; written only when absent or of
another size, so only an app's first start pays for it): the class loader
needs them to exist, and so do apps -- loaders that read a library's ELF header
to load what it needs first (Facebook's SoLoader, which every React Native app
has) and code that lists the folder. The original Megler keeps empty stand-ins
there and serves the APK's entry when one is opened; real files are simpler
and cost the libraries' size on the phone's storage once.
`JVM_NativeLoad` (`hk_dvm_jvm.c`) then loads: with no class loader (one of
the runtime's classes asked) a library of the Android root's, otherwise the
APK's of that file name. A path that names a file with something in it that is
not in the APK -- a library the app unpacked or downloaded itself -- is loaded
from that file. A library is loaded once: a second request finds it and does
not run `JNI_OnLoad` again. What comes back when it fails is ART's sentence
(`dlopen failed: library "..." not found`, `JNI_ERR returned from JNI_OnLoad in
"..."`, `Bad JNI version returned from JNI_OnLoad in "...": N`); a library the
APK does not have at all never gets that far, and is libcore's own
`UnsatisfiedLinkError` with the class loader in it.

**Binding.** A native method is looked for in this order: the C tables of the
runtime and the drivers (`hk_dvm_register`); what a library registered with
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
(`drivers/hk_java_ndk.c`) work on the framework's `Bitmap`: the memory is the
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
`ANativeWindow_fromSurface` works for the `Surface` objects the engine drivers
make and gives NULL for one of the framework's (a `SurfaceView`'s): that needs
a window per `SurfaceView` -- for OpenGL ES an EGL surface the shell composes
with the views' picture, for `ANativeWindow_lock` a bitmap of the core's that
`unlockAndPost` hands to the view -- and neither is written.

**The JNI functions on the runtime's objects**, down the table. "app": a step
of the `natives` test app calls it from a library built by the NDK; "core":
the core's own JNI test does (`hk_dvm_tests_jni.c`); "libcore": Android's own
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
| `GetObjectRefType` | **wrong**: `JNILocalRefType` for every reference | nothing |
| `IsSameObject` | right | app |
| `AllocObject` | the object with no constructor run | nothing |
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

`GetObjectRefType` is the one that answers wrongly; telling a global
reference from a local one would need references that are more than the
object's address, which nothing else here needs while nothing is collected.

### Drawing: `megler.Gfx` and the shell

`android.graphics` in the framework is Java over the 54 native methods of
`megler.Gfx`. Here they are `core/src/drivers/hk_java_gfx.c`, and the drawing
itself is the shell's, behind `megler_gfx_host` in `core/include/megler.h`, whose
comment is the contract: every encoding that crosses it, and what is expected
where the original leaned on CoreGraphics without saying.

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
- **The stand-in** (`core/tests/hk_gfx_standin.c`) is for tests and is not a
  renderer to look at: a pixel is in when its centre is, curves are straight
  pieces, strokes have no joins, a gradient is its first colour, text is a box
  for each letter with fixed metrics (advance 0.6 of the size, ascent 0.8,
  descent 0.2), images are PNG only. It is deterministic on every machine,
  which is what the tests want of it.

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

### Memory

**Nothing is collected.** Every object is a zeroed allocation from the C heap
that lives as long as the process. (The original does not collect either.) The
plan is under "Threads and memory" below. Allocation goes through one function,
`alloc_raw` in `hk_dvm_object.c`.

### What is not checked

There is no bytecode verifier and there are no access checks. A method whose
instructions name registers it does not have, or use an int as a reference, is
not caught; `private` is not enforced. What is checked is everything that comes
from outside the instruction stream's own logic: the file's header, checksum and
table bounds, every index before its table is read, array kinds and bounds, that
an object has the field an instruction reads. An app's DEX is trusted as its
native code is.

## What is not done

- **Nothing has run on the phone**, and the shell cannot show a Java app yet
  (`megler_gfx_host`, `present_pixels`).
- No garbage collection, no finalizers; `Reference`s are never cleared or
  queued: a `WeakReference` holds its object for good, which is correct Java
  (nothing promises a collection) and is why it works.
- No `MethodHandle` (`invoke-polymorphic`, `invoke-custom`,
  `const-method-handle` and `const-method-type` throw). A class loader cannot
  give two classes one name, and classes are not unloaded.
- Of the framework's native side only `megler.Native` and `megler.Gfx` are there
  (see "The Java driver"): no database, no sound from Java, no web view, no
  sensors, no video, no OpenGL from Java, no keyboard.
- Images other than what the shell decodes; the stand-in reads PNG alone, and
  Windows 10 Mobile has no WebP decoder, which apps' resources use heavily.
- **No network**: sockets fail as if there were none (`bionic/hk_net.c`), and
  `libopenjdk`'s start-up, which probes for IPv6, is told so.
- **No TLS and no conscrypt**: `libjavacrypto.so` is not built (it needs
  `libssl`). Digests, random numbers, MACs and ciphers are had without it
  ("The security providers"); TLS and whatever asks conscrypt's provider by
  name for more than a digest are not.
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
- **Two things about the loader, known and left** (`ld/hk_ld.c`, not this
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

### 2. The framework: the original's, built here

Decided on measurements (`bash tools/dvm-report.sh link-framework`, and `link
<file>`; `python tools/dvm-natives.py` for the native methods):

| | Android 9's own `framework.jar` | the original's Java framework |
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
SurfaceFlinger, Skia. The original's is the same API with those services
written out in Java inside the app's own process.

`tools/build-framework.sh` builds it: the original's `src` and `gen-src`
(3077 files of them used; `stubs` to compile against), read where they are in the other
repository and never written to, with `core/framework-overlay` on top, where a
file replaces the original's of the same path. javac of JDK 1.8.0_202, d8 of
build-tools 29.0.3 with `--min-api 26`; 5808 classes; `build/framework/
megler-framework.dex`, which `tools/dvm-android-root.sh` puts in the root. `bash
tools/dvm-report.sh link build/framework/megler-framework.dex` says: 5965
linked, 0 not; 0 classes, methods and fields named and not found.

**What differs from the original's framework: six files** in the overlay,
five of them a statement or a method each inside an otherwise untouched copy
(the copies are whole files, because a file is what the overlay replaces).

- `android/webkit/MimeTypeMap.java`: the original hands its table to
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
  another and come back.
- `android/content/res/ResTable.java`, two methods: a resource is found by
  its name without indexing the whole package ("What an AlertDialog cost").
- `android/content/res/Resources.java`, the class `Bag`: a style's items are
  found by hashing (the same section).
- `android/graphics/drawable/RippleDrawable.java` and `megler/ResInflate.java`,
  a method and a statement: a ripple inflated from XML is told that it has a
  mask. Android's commonest ripple is nothing but a mask (the background of
  every list row and of everything that is `selectableItemBackground`), and
  the mask is what keeps it inside the view. The inflater left the mask layer
  out of what is drawn, rightly, and never told the drawable; with no mask
  and no layers the drawable took itself for a borderless one and drew its
  highlight as a disc of half its longer side: for a pressed row of a list, a
  grey disc as wide as the screen over the rows above and below.

The original has all five faults; they are its author's to take back.

That the framework was written against a later libcore (the original's ART
module is Android 16's) showed nowhere else in linking; whether it shows in
*behaviour* is known only as far as the two test apps go. Should the overlay
grow past a few dozen files, the framework is better copied here.

The platform's resources it reads are Android 9's own `framework-res.apk`,
and they fit: the action bar, the dialog, the list row and the button of the
test apps are that file's layouts, styles and nine-patches.

**Its native side still to write**, in this order of need:

| The original's file | What it is | Here |
|---|---|---|
| `megler-tl-dvm-sqlite.c` | the system's SQLite | SQLite built in, or the system's `winsqlite3.dll` (*not verified* to be usable from the app container on a phone) |
| `megler-tl-dvm-audio.c`, `-media.c` | `AudioTrack`, `SoundPool`, `MediaPlayer`, `MediaCodec` | onto `ndk/hk_audio.c`; decoding on Media Foundation |
| `megler-tl-dvm-gles*.c`, `EGLNative` | generated calls into ANGLE | regenerated with this port's native signature; ANGLE is already here. With it comes a picture over OpenGL, which `present_pixels` does not provide for |
| `megler-tl-dvm-web.m`, `-sensors.m` | `WebView`; sensors | the shell's XAML WebView; `Windows.Devices.Sensors` |
| the keyboard, clipboard, sharing, URLs, orientation, insets | `megler.Native`'s calls to the host | calls of the shell's that `megler.h` does not have yet |

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
- **The big libraries**: an HTTP client needs sockets and TLS, neither of
  which is here; a JSON library that reflects on generic field types needs
  `Signature`, which is; dependency injection that generates code needs
  nothing; one that scans a `DexFile`'s class names gets them.
- **A collector**, before any of them runs for long (below).

### 4. Threads and memory in 2 GB

A 32-bit process here has 2 GB of addresses for everything: ANGLE, textures,
the app's native heap, the libraries, and Java. Hollow Knight alone left 690 MB
of them free. Three things follow.

**Threads are cheap here and must stay so.** A Java thread costs a host stack
and a frame stack. The host stack can be small, since Java frames are not on it
(1 MB, or what the native code it calls needs: `hk_thread_whole_stack` commits
it whole). The frame stack is 768 KB today, allocated at once; it should be
reserved and committed as it grows. Thirty threads are then some tens of MB,
where the original's 32 MB a thread would be a gigabyte. A thread's state is not
freed when the thread ends yet; the host layer has no hook for that.

**Objects have to be freed.** A process that never collects is a
demonstration. Measured on `hello` under qemu, with the runtime's own counts
(instructions counted by `hk_dvm_count_instructions`):

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

The collector planned:

- *Non-moving mark and sweep, stopping the world.* Non-moving because pointers to
  objects are everywhere outside the runtime's sight: every `jobject` native
  code holds is the object's address, `Get...ArrayElements` hands out the
  array's own storage, an object's identity hash is its address, and
  `Unsafe` works on addresses.
- *The heap as the runtime's own regions* (`hk_mem_reserve`, a megabyte or a few
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
  them, which `hk_jni.c` does not do today) and, per thread, of the objects JNI
  calls have handed out since the native method was entered, dropped when it
  returns. Native stacks are not scanned.
- *The roots that exist today and are nowhere written down as roots*: each
  thread's frame stack and its pending exception and `Thread` object; every
  linked class's statics and its `Class` object; interned strings and each DEX
  file's resolved string constants; the JNI layer's C-only classes' own fields
  and statics (`hk_jobj.fields` of the stand-ins), which hold runtime objects;
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
`hk_jobj` can lose `fields`, `nfields`, `refs` and `permanent` for the runtime's
objects and come down to 16 bytes; the C-only world keeps the full one.

### 5. Speed

*Measured, on the PC:* above. *Expected on the phone, not verified:* a Snapdragon
820's core is roughly a sixth to an eighth of that PC's in single-thread work, so
some 50 to 80 million simple instructions a second and 6 to 9 million calls.
Android's own interpreter is of the same order (a hand-written assembly one,
perhaps half as fast again); Android's compiled code is ten to thirty times
faster on arithmetic. An app that lays out and draws views in Java at 60 frames a
second will feel that; one that mostly waits for input will not. The original
runs apps on an interpreter of this kind, on processors three to five times
faster.

What to do about it, cheapest first:

1. Take the per-instruction `g_dvm.slow` check out of the loop: poll at branches
   and calls only.
2. **Quicken**: when an instruction's field, method or string has been resolved,
   rewrite it in a private copy of the method's code to a form that carries the
   answer (a slot number, a vtable index), so that running it again resolves
   nothing.
3. Do the hottest libcore methods in C (`String.equals`, `hashCode`, `indexOf`):
   the original has a short list of such "fast paths".
4. Common pairs of instructions as one.

**A JIT is possible and should wait.** The port can make executable memory, but
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
| `core/src/jni/hk_jni.c`, `hk_jni.h` | the join: "One Java world" above; `hk_jobj.dvm`; `hk_jni_native_counts` |
| `core/src/host/hk_host.h`, `hk_host_win32.c`, `hk_host_posix.c` | `hk_atomic_cas64` |
| `core/src/bionic/hk_bionic.c` | `libopenjdkjvm.so` is a system library |
| `core/src/bionic/hk_libc_io.c`, `hk_libc_misc.c`, `hk_net.c` | the names libcore's libraries import that were missing: the 37 of `build/android9/lib/report/layer-missing.txt` and a few before that list was made |
| `core/tests/hk_tests.c`, `hk_tests.h`, `hk_tests_main.c` | the runtime's tests in the core's; the reports' modes in the command-line program |
| `core/tests/guest/t_jni.c`, `tools/build-guest.sh` | the guest library for the join's tests |
| `tools/build-tests.sh` | the fixtures first; the second run on libcore |
| `tools/build-appx.sh` | the fixtures in `selftest/dvm`; `--libcore` |

Changed for the third, outside `core/src/dvm`:

| File | What |
|---|---|
| `core/include/megler.h` | `megler_gfx_host`; `megler_window_host.present_pixels`; `megler_run_config.gfx` and `.android_root` |
| `core/src/drivers/hk_driver.c`, `hk_driver.h` | list the Java driver (`hk_java.c`, `hk_java_gfx.c` are new) |
| `core/src/apk/hk_apk.c`, `hk_apk.h` | an APK that is Java alone is runnable; how the launcher activity asks to be held (`"orientation"` in the report) |
| `core/src/bionic/hk_net.c` | asking whether a descriptor is a socket is answered "it is not", quietly (libcore asks it of every file it closes) |
| `core/tests/hk_tests.c`, `hk_tests.h`, `hk_tests_main.c` | the drawing's tests in the core's; the Android root looked for in the package; `--java` |
| `tools/build-tests.sh` | the two Java apps under qemu-arm |
| `tools/build-appx.sh` | `--libcore` puts the Android root at `android` in the package (it was `selftest/android`) |
| `tools/dvm-android-root.sh` | the framework's DEX and the platform's resources in the root |

Still to change, for the steps above:

| File | Why |
|---|---|
| `core/src/jni/hk_jni_android.c`, `hk_jni_loop.c`, `hk_jni_prefs.c` | what they implement in C exists in Java in the framework; for a Java app they serve as natives under it or not at all, and stay as they are for the engine drivers |
| `core/src/apk/hk_apk.c` | (done) an APK with Java and native libraries that no engine was recognised in is `java` |
| `core/src/run/hk_run.c` | it refuses every app where guest code cannot run; a Java app without native libraries needs none (only the PC's tests meet this) |
| `core/src/host/*` | something called when a thread ends |
| `core/src/bionic/hk_libc_mem.c`, `hk_net.c` | a shared mapping for a file mapped twice; sockets |
| `core/src/ld/hk_ld.c` | (done) libraries told apart by where they came from |
| `app/Megler` | `megler_gfx_host` and `present_pixels`; a Java app's page and state; the keyboard and the rest of what `megler.Native` asks of a host; the Android root in the ordinary package or unpacked once |
| `docs/STATUS.md`, `docs/ARCHITECTURE.md`, `README.md` | they describe the first milestone |

## What is on disk, and what would still need a download

`build/android9` (not in the repository; `tools/android9-*.sh` made it) has
Android 9's boot class path as DEX files (`boot/`), the system image's
`framework` folder, ICU's data and the time zones (`system/`), libcore's and
ART's sources at `android-9.0.0_r47` (`aosp/`), and the seven native libraries
built or taken for 32-bit ARM (`lib/`). Everything above runs from those, from
the original's repository beside this one (its Java framework's sources), and
from what the build scripts make of them: `build/framework/megler-framework.dex`
and `build/guest/java/*.apk`.

Not there:

| What | For |
|---|---|
| `libjavacrypto.so` and `libssl.so` for 32-bit ARM | conscrypt: TLS, `SecureRandom`, message digests through the provider. Built from the same sources as the rest, not downloaded. |
| R8 8.5 or later (Google's Maven, `com.android.tools:r8`, 10 to 15 MB) | only if the local d8's output of the framework proves unusable |
| SQLite's source (sqlite.org, the amalgamation, 2.5 MB) | only if `winsqlite3.dll` cannot be used |

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
