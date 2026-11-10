package megler.test.natives;

import android.app.Activity;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Build;
import android.os.Bundle;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;
import java.util.Arrays;

/**
 * Runs the native library through everything Lib declares, one named step after another. Each step is a line of the log --
 * "name ok" or "name FAILED" and why -- and a line on the screen; the test reads the log, a person the screen. Under the lines is
 * the bitmap the library painted, drawn as it is, pixel for pixel. Between the two, two SurfaceViews native code draws on
 * (WindowTest), whose steps go on after these.
 */
public class Main extends Activity {
    private static final StringBuilder sLines = new StringBuilder();
    private static int sOk, sFailed;
    private static TextView sText;

    static void say(String name, boolean ok, String detail) {
        if (ok) sOk++;
        else sFailed++;
        String line = name + (ok ? " ok" : " FAILED" + (detail == null ? "" : ": " + detail));
        Log.i("NativesTest", line);
        sLines.append(line).append('\n');
        if (sText != null) sText.setText(sLines);
    }

    private interface Step {
        boolean run() throws Throwable;
    }

    private String detail;

    private void step(String name, Step s) {
        detail = null;
        try {
            say(name, s.run(), detail);
        } catch (Throwable t) {
            say(name, false, t.toString());
        }
    }

    /** The painted bitmap, 16 pixels in from the view's corner, on a dark field. */
    static class Picture extends View {
        Bitmap bitmap;

        Picture(Context c) {
            super(c);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            canvas.drawColor(0xFF202020);
            if (bitmap != null) canvas.drawBitmap(bitmap, 16, 16, null);
        }
    }

    private Picture picture;
    private WindowTest windows;
    private StackTest stack;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFFE0E0E0);
        picture = new Picture(this);
        root.addView(picture, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 96));
        /* Native code on SurfaceViews, under the picture; its steps go on after the others, one for each tap of the test's. */
        windows = new WindowTest(this);
        root.addView(windows.view(), new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 120));
        root.setOnTouchListener((v, e) -> {
            if (e.getAction() == android.view.MotionEvent.ACTION_UP) windows.tapped();
            return true;
        });
        /* And, at the tap after the window test's last step, two SurfaceViews one over the other, the one over the window translucent, under the row (StackTest). */
        windows.afterLast = () -> {
            if (stack != null) return;
            stack = new StackTest(this);
            root.addView(stack.view(), 2, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 100));
        };
        sText = new TextView(this);
        sText.setTextColor(0xFF000000);
        sText.setTextSize(10);
        root.addView(sText, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(root);
        Log.i("NativesTest", "onCreate");
        runAll();
        Log.i("NativesTest", "done ok=" + sOk + " failed=" + sFailed);
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        int[] at = new int[2];
        picture.getLocationOnScreen(at);
        Log.i("NativesTest", "layout focus=" + focus + " picture=" + at[0] + "," + at[1] + "," + picture.getWidth() + "x" + picture.getHeight());
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (stack != null) stack.end();
        windows.end();
        Log.i("NativesTest", "onDestroy");
    }

    private void runAll() {
        final String libDir = getApplicationInfo().nativeLibraryDir;

        step("load", () -> {
            detail = "calls=" + Lib.onLoadCalls + " class=" + Lib.onLoadClass;
            return Lib.onLoadCalls == 1 && Lib.onLoadClass == Lib.class;
        });
        step("registered", () -> Lib.addInts(2, 3) == 5 && new Lib(7).instanceTimes(6) == 42 && "RegisterNatives".equals(Lib.registeredHow()));
        step("primitives", () -> !Lib.flip(true) && Lib.flip(false) && Lib.nextByte((byte) 127) == (byte) -128 && Lib.nextChar('a') == 'b' && Lib.nextChar('￿') == 0
                && Lib.nextShort((short) 32767) == (short) -32768 && Lib.nextInt(Integer.MAX_VALUE) == Integer.MIN_VALUE && Lib.nextLong(Long.MAX_VALUE) == Long.MIN_VALUE
                && Lib.nextLong(0xFFFFFFFFL) == 0x100000000L && Lib.half(3f) == 1.5f && Lib.twice(1.25) == 2.5 && Lib.twice(-1e300) == -2e300);
        step("stack-mix", () -> {
            double want = 1 + (double) (2L << 40) + 3.5 + 4 + (double) -5L + (double) 6.25f + 7.125 + (double) (1L << 50) + 9 + 0.5;
            double got = Lib.mix(1, 2L << 40, 3.5, 4, -5L, 6.25f, 7.125, 1L << 50, 9, 0.5);
            long wantLong = (1L << 40) * 3 + 2 * 5 + (-3L << 35) * 7 + (byte) -4 * 11 + 5L * 13 + (short) -6 * 17 + 'A' * 19 + 23;
            long gotLong = new Lib(0).mixLong(1L << 40, 2, -3L << 35, (byte) -4, 5L, (short) -6, 'A', true);
            detail = got + " for " + want + ", " + gotLong + " for " + wantLong;
            return got == want && gotLong == wantLong;
        });
        step("overloads", () -> Lib.over(5) == 6 && Lib.over("abc") == 3);
        step("arrays", () -> {
            int[] ints = { 1, 2, 3, -10 };
            byte[] bytes = new byte[8];
            double[] doubles = { 0.25, 0.5, 1.0 };
            boolean[] z = { false, true };
            char[] c = { 'x', 'q' };
            short[] s = { 0, -7 };
            float[] f = { 0f, 2.5f };
            long[] squares = Lib.squares(5);
            String[] words = Lib.words("a bc def");
            boolean ok = Lib.sum(ints) == -4 && Arrays.equals(ints, new int[] { -1, -2, -3, 10 });
            Lib.fill(bytes, 2, 3, (byte) 9);
            ok &= Arrays.equals(bytes, new byte[] { 0, 0, 9, 9, 9, 0, 0, 0 });
            ok &= squares.length == 5 && squares[0] == 0 && squares[4] == 16L * 1000000007L;
            ok &= Lib.sumCritical(doubles) == 1.75 && doubles[0] == 0.75 && doubles[2] == 1.5;
            ok &= Lib.others(z, c, s, f) == 1000 + 'q' - 7 + 2 && z[0] == false && c[0] == 'r' && s[0] == -8 && f[0] == 5f;
            ok &= words.length == 3 && "a".equals(words[0]) && "bc".equals(words[1]) && "def".equals(words[2]) && Lib.total(words) == 6;
            return ok;
        });
        step("bounds", () -> {
            int bits = Lib.bounds(new int[] { 1, 2, 3, 4 }, new Object[2], "abcd");
            detail = "0x" + Integer.toHexString(bits);
            return bits == 31;
        });
        step("strings", () -> {
            int[] bytes = new int[1];
            String world = Lib.region("hello wörld", 6, 5, bytes);
            detail = Lib.greet("мир") + " | " + Lib.reverse("ab€c") + " | " + world + " " + bytes[0];
            return "hello, мир".equals(Lib.greet("мир")) && "c€ba".equals(Lib.reverse("ab€c")) && "wörld".equals(world) && bytes[0] == 6
                    && "".equals(Lib.reverse(""));
        });
        step("callback", () -> {
            final Object[] seen = new Object[4];
            long before = Lib.counter;
            int r = Lib.callBack((a, b, c, s) -> {
                seen[0] = a;
                seen[1] = b;
                seen[2] = c;
                seen[3] = s;
                return a * 2;
            }, 21);
            Lib made = Lib.make(9, "made");
            detail = r + " " + Arrays.toString(seen) + " counter+" + (Lib.counter - before);
            return r == 42 && seen[0].equals(21) && seen[1].equals(21L << 33) && seen[2].equals(5.25) && "from native".equals(seen[3])
                    && Lib.counter - before == "from native/1.5".length() && made.factor == 9 && "made".equals(made.label) && made.instanceTimes(2) == 18;
        });
        step("throw-native", () -> {
            String first = null;
            int code = 0;
            try {
                Lib.throwIt("thrown in C");
            } catch (IllegalStateException e) {
                first = e.getMessage();
            }
            try {
                Lib.throwOwn(77);
            } catch (Lib.Failure e) {
                code = e.code;
            }
            detail = first + " " + code;
            return "thrown in C".equals(first) && code == 77;
        });
        step("throw-java", () -> "thrown in Java".equals(Lib.catchIt(() -> {
            throw new UnsupportedOperationException("thrown in Java");
        })) && "nothing was thrown".equals(Lib.catchIt(() -> { })));
        step("find-class", () -> {
            String missing = Lib.find("megler/test/natives/Nothing"), missingArray = Lib.find("[Lno/such/Thing;");
            boolean stillMissing = false;
            try {
                Class.forName("megler.test.natives.Nothing");
            } catch (ClassNotFoundException e) {
                stillMissing = true;
            }
            detail = missing + " " + missingArray + " " + stillMissing;
            return "found".equals(Lib.find("megler/test/natives/Lib$Failure")) && "found".equals(Lib.find("[Lmegler/test/natives/Lib;")) && "found".equals(Lib.find("[[I"))
                    && "found".equals(Lib.find("android/view/View")) && "java.lang.ClassNotFoundException".equals(missing)
                    && "java.lang.ClassNotFoundException".equals(missingArray) && stillMissing;
        });
        step("references", () -> {
            int bits = Lib.refs(new Lib(1));
            detail = "0x" + Integer.toHexString(bits);
            return bits == 0x3FFF;
        });
        step("reflection", () -> {
            int[] out = new int[2];
            Lib.counter = 31;
            Object m = Lib.reflect(Lib.class.getDeclaredMethod("addInts", int.class, int.class), Lib.class.getDeclaredField("counter"), out);
            detail = out[0] + " " + out[1] + " " + m + " " + Lib.reflected[0];
            return out[0] == 42 && out[1] == 31 && m instanceof Method && "staticBack".equals(((Method) m).getName())
                    && "x/2.0".equals(((Method) m).invoke(null, "x", 2f)) && Lib.reflected[0] instanceof Field && "factor".equals(((Field) Lib.reflected[0]).getName());
        });
        step("monitor", () -> Lib.monitor(new Object()));
        step("thread", () -> {
            Lib.ThreadReport r = new Lib.ThreadReport();
            boolean started = Lib.startThread(r, "native-worker", false);
            detail = "started=" + started + " called=" + r.called + " GetEnv " + r.before + "/" + r.after + " attach=" + r.rc + " app=" + r.app + " string=" + r.string + " kept=" + r.kept
                    + " name=" + r.name + " daemon=" + r.daemon + " group=" + r.group + " after detach=" + Lib.afterDetach();
            return started && r.called && r.before == -2 && r.rc == 0 && r.after == 0 && !r.app && r.string && r.kept && "native-worker".equals(r.name) && !r.daemon
                    && "main".equals(r.group) && Lib.afterDetach() == -2;
        });
        step("thread-daemon", () -> {
            Lib.ThreadReport r = new Lib.ThreadReport();
            boolean started = Lib.startThread(r, "native-daemon", true);
            detail = "called=" + r.called + " name=" + r.name + " daemon=" + r.daemon;
            return started && r.called && "native-daemon".equals(r.name) && r.daemon;
        });
        step("needed-library", () -> Lib.dep(1L << 33) == (1L << 33) * 4242);
        step("asset", () -> {
            String text = Lib.readAsset(getAssets(), "note.txt");
            detail = String.valueOf(text);
            return text != null && "an asset, read from native code".equals(text.trim()) && Lib.readAsset(getAssets(), "nothing.txt") == null;
        });
        step("bitmap", () -> {
            Bitmap bm = Bitmap.createBitmap(64, 64, Bitmap.Config.ARGB_8888);
            String told = Lib.paint(bm);
            detail = told + " " + Integer.toHexString(bm.getPixel(4, 4)) + " " + Integer.toHexString(bm.getPixel(20, 40)) + " " + Integer.toHexString(bm.getPixel(40, 4)) + " "
                    + Integer.toHexString(bm.getPixel(63, 63));
            picture.bitmap = bm;
            picture.invalidate();
            return "64x64 stride=256 format=1 rc=0".equals(told) && bm.getPixel(4, 4) == 0xFF00FF00 && bm.getPixel(20, 40) == 0xFFFF0000 && bm.getPixel(40, 4) == 0xFF0000FF
                    && bm.getPixel(15, 15) == 0xFF00FF00 && bm.getPixel(16, 16) == 0xFFFF0000 && bm.getPixel(63, 63) == 0x80FFFFFF;
        });
        step("direct-buffer", () -> {
            ByteBuffer mine = ByteBuffer.allocateDirect(16);
            mine.put(0, (byte) 5);
            mine.put(3, (byte) 200);
            long sum = Lib.bufferSum(mine);
            ByteBuffer theirs = Lib.sharedBuffer();
            boolean ok = sum == 205 * 1000 + 16 && mine.get(15) == 77;
            ok &= theirs != null && theirs.isDirect() && theirs.capacity() == 8 && theirs.get(3) == 4 && theirs.get(7) == 8;
            theirs.put(0, (byte) 99);
            detail = sum + " " + theirs + " " + Lib.sharedAt(0);
            return ok && Lib.sharedAt(0) == 99;
        });
        step("configuration", () -> {
            DisplayMetrics dm = getResources().getDisplayMetrics();
            String told = Lib.configuration(getAssets());
            String want = "density=" + dm.densityDpi + " sdk=" + Build.VERSION.SDK_INT + " orientation=" + (dm.widthPixels >= dm.heightPixels ? 2 : 1) + " w="
                    + dm.widthPixels * 160 / dm.densityDpi + " h=" + dm.heightPixels * 160 / dm.densityDpi + " lang=en";
            detail = told + " for " + want;
            return want.equals(told);
        });
        step("library-folder", () -> {
            String found = ((dalvik.system.BaseDexClassLoader) getClassLoader()).findLibrary("natives");
            File lib = new File(libDir, "libnatives.so"), dep = new File(libDir, "libnatives_dep.so");
            detail = libDir + " " + found + " " + lib.length() + " " + dep.length();
            byte[] magic = new byte[4];
            try (FileInputStream in = new FileInputStream(lib)) {
                in.read(magic);
            }
            /*
             * The folder holds the APK's libraries and nothing else, each of the entry's bytes, as ByteDance's librarian sums them
             * against the APK's entries: the test put an older version's there before the start (mg_java_tests.c) -- a library this
             * one has not, and libnatives_dep.so of the entry's size and other bytes, which Megler up to 0.1.0.685 kept.
             */
            java.util.TreeSet<String> want = new java.util.TreeSet<>(), have = new java.util.TreeSet<>(Arrays.asList(new File(libDir).list()));
            boolean same = true;
            try (java.util.zip.ZipFile apk = new java.util.zip.ZipFile(getPackageCodePath())) {
                for (java.util.Enumeration<? extends java.util.zip.ZipEntry> en = apk.entries(); en.hasMoreElements();) {
                    java.util.zip.ZipEntry e = en.nextElement();
                    String name = e.getName();
                    if (!name.startsWith("lib/armeabi-v7a/") || !name.endsWith(".so")) continue;
                    File placed = new File(libDir, name.substring(16));
                    want.add(placed.getName());
                    java.util.zip.CRC32 crc = new java.util.zip.CRC32();
                    byte[] buf = new byte[8192];
                    int n;
                    try (FileInputStream in = new FileInputStream(placed)) {
                        while ((n = in.read(buf)) > 0) crc.update(buf, 0, n);
                    }
                    if (placed.length() != e.getSize() || crc.getValue() != e.getCrc()) {
                        same = false;
                        detail += " " + placed.getName() + " is not the APK's";
                    }
                }
            }
            detail += " folder=" + have + " apk=" + want;
            return lib.getPath().equals(found) && lib.length() > 1000 && dep.length() > 1000 && magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F'
                    && want.equals(have) && same;
        });
        step("loaded-once", () -> {
            System.loadLibrary("natives");
            System.load(libDir + "/libnatives.so");
            detail = "calls=" + Lib.onLoadCalls;
            return Lib.onLoadCalls == 1;
        });
        step("own-file", () -> {
            /* A library the app unpacks for itself and loads by its path: the bytes of the second one under a name no APK has. */
            File copy = new File(getFilesDir(), "libunpacked.so");
            byte[] buf = new byte[65536];
            int n;
            try (FileInputStream in = new FileInputStream(new File(libDir, "libnatives_dep.so")); FileOutputStream out = new FileOutputStream(copy)) {
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            System.load(copy.getPath());
            return true;
        });
        step("own-file-same-name", () -> {
            /*
             * A library of the APK's name and size and other bytes, made by the app and loaded by its path before anything loaded
             * the APK's of that name: the APK's libnatives_alt.so with the "version 1" of its text made "version 2". On Android
             * System.load loads the file it is given; TikTok Lite 47.7.4 loads the libnpth.so it unpacks so.
             */
            File src = new File(libDir, "libnatives_alt.so"), dir = new File(getFilesDir(), "own"), copy = new File(dir, "libnatives_alt.so");
            byte[] bytes = new byte[(int) src.length()], from = "natives_alt version 1".getBytes("US-ASCII");
            try (FileInputStream in = new FileInputStream(src)) {
                int at = 0, n;
                while (at < bytes.length && (n = in.read(bytes, at, bytes.length - at)) > 0) at += n;
            }
            int found = -1, times = 0;
            for (int i = 0; i + from.length <= bytes.length; i++) {
                int k = 0;
                while (k < from.length && bytes[i + k] == from[k]) k++;
                if (k == from.length) {
                    found = i;
                    times++;
                }
            }
            if (times != 1) {
                detail = "the library has its text " + times + " times";
                return false;
            }
            bytes[found + from.length - 1] = '2';
            dir.mkdirs();
            try (FileOutputStream out = new FileOutputStream(copy)) {
                out.write(bytes);
            }
            System.load(copy.getPath());
            String said = Alt.version();
            detail = "said=" + said + " size=" + copy.length() + "/" + src.length();
            return "natives_alt version 2".equals(said) && copy.length() == src.length();
        });
        step("dlopen-own-file", () -> {
            /* A library the app wrote itself, in a folder no library came from before, opened by native code by its path. */
            File dir = new File(getFilesDir(), "dl"), copy = new File(dir, "libdlopened.so");
            byte[] buf = new byte[65536];
            int n;
            dir.mkdirs();
            try (FileInputStream in = new FileInputStream(new File(libDir, "libnatives_dep.so")); FileOutputStream out = new FileOutputStream(copy)) {
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            }
            int answer = Lib.dlopenAnswer(copy.getPath());
            detail = "answer=" + answer;
            return answer == 4242;
        });
        step("missing-library", () -> {
            String byName = null, byPath = null;
            try {
                System.loadLibrary("nothere");
            } catch (UnsatisfiedLinkError e) {
                byName = e.getMessage();
            }
            try {
                System.load(libDir + "/libghost.so");
            } catch (UnsatisfiedLinkError e) {
                byPath = e.getMessage();
            }
            detail = byName + " | " + byPath;
            return byName != null && byName.contains("PathClassLoader") && byName.endsWith("couldn't find \"libnothere.so\"") && byPath != null
                    && byPath.equals("dlopen failed: library \"" + libDir + "/libghost.so\" not found");
        });
        step("missing-method", () -> {
            try {
                Lib.notThere();
            } catch (UnsatisfiedLinkError e) {
                detail = e.getMessage();
                return "No implementation found for void megler.test.natives.Lib.notThere() (tried Java_megler_test_natives_Lib_notThere and Java_megler_test_natives_Lib_notThere__)"
                        .equals(e.getMessage());
            }
            return false;
        });
        step("looper-watched", () -> {
            int r = Lib.watchLooper(new Lib.LooperReport());
            detail = "watchLooper=" + r;
            return r == 1;
        });
        step("sensors", () -> {
            /* The phone's sensors through the NDK (jni/natives.c, watchSensors): its events come later, on this thread's looper. */
            int r = Lib.watchSensors(new Lib.SensorReport());
            detail = "watchSensors=" + r;
            return r == 1;
        });
        step("gc-held", () -> {
            /* Twice: the global reference made in the first call must hold across the second's collection too. */
            int first = Lib.holdAcrossGc(), second = Lib.holdAcrossGc();
            detail = "first " + first + " second " + second;
            return first == 1 && second == 1;
        });
        step("system-icu", () -> {
            /* Android 9's ICU is version 60; it is the Android root's, the one libcore uses. */
            int v = Lib.icuVersion();
            detail = "version=" + v;
            return v == 60;
        });
        step("abi", () -> {
            /*
             * What SoLoader asks to choose an app's libraries: the hidden ApplicationInfo.primaryCpuAbi by reflection, then
             * Build.SUPPORTED_ABIS kept to those of the process's width (Process.is64Bit). Both must say 32-bit ARM.
             */
            Object primary = android.content.pm.ApplicationInfo.class.getDeclaredField("primaryCpuAbi").get(getApplicationInfo());
            detail = "primary=" + primary + " is64=" + android.os.Process.is64Bit() + " supported=" + java.util.Arrays.toString(android.os.Build.SUPPORTED_ABIS);
            return "armeabi-v7a".equals(primary) && !android.os.Process.is64Bit() && "armeabi-v7a".equals(android.os.Build.SUPPORTED_ABIS[0])
                    && android.os.Build.SUPPORTED_64_BIT_ABIS.length == 0;
        });
        step("device", () -> {
            /* The device a game chooses its detail by: Build's fields and the properties native code reads name the same one. */
            String model = Lib.systemProperty("ro.product.model"), device = Lib.systemProperty("ro.product.device"),
                   maker = Lib.systemProperty("ro.product.manufacturer"), platform = Lib.systemProperty("ro.board.platform");
            detail = "model=" + android.os.Build.MODEL + "/" + model + " device=" + android.os.Build.DEVICE + "/" + device + " maker="
                    + android.os.Build.MANUFACTURER + "/" + maker + " platform=" + platform;
            return "Pixel".equals(android.os.Build.MODEL) && android.os.Build.MODEL.equals(model) && android.os.Build.DEVICE.equals(device)
                    && android.os.Build.MANUFACTURER.equals(maker) && "msm8996".equals(platform);
        });
        step("string-alloc", () -> {
            /* A string AllocObject made and a String constructor called on it through JNI, as an app-lock library makes its hashes. */
            String s = Lib.allocatedString("hash\u00e9".getBytes(java.nio.charset.StandardCharsets.UTF_8), new char[] { 'a', 'b', 'c' });
            detail = "string=" + s;
            return "hash\u00e9|abc".equals(s) && s.length() == 9 && s.hashCode() == "hash\u00e9|abc".hashCode();
        });
        step("many-args", () -> {
            /* Java called back from native code with 78 arguments (mg_jni.c's JARGS), through "..." and through jvalues. */
            Object[] names = new Object[33];
            for (int k = 0; k < names.length; k++) names[k] = "o" + k;
            int r = Lib.manyArgs(names);
            detail = "" + r;
            return r == 3139533;
        });
        step("deep-callbacks", () -> {
            /*
             * Native code and Java calling each other 150 levels deep on a thread of the default stack, as Meta's FlexLayout
             * measures a layout (Instagram, Messenger and Facebook Lite ended at their sign-in screens with "StackOverflowError: 22
             * calls from native code deep"): Android's threads have 1 MB for it.
             */
            /* At the bottom a collection on that thread, then one by this thread while that one waits there, then garbage made
               where what the collections lost would be: each level's string, held by its native frame alone, must be as it was. */
            final int[] got = { -2 };
            final Throwable[] failed = new Throwable[1];
            final java.util.concurrent.CountDownLatch atBottom = new java.util.concurrent.CountDownLatch(1), collected = new java.util.concurrent.CountDownLatch(1);
            final String[] junk = new String[512];
            Lib.deepest = () -> {
                System.gc();
                atBottom.countDown();
                try { collected.await(); } catch (InterruptedException e) { throw new RuntimeException(e); }
                for (int k = 0; k < 20000; k++) junk[k % junk.length] = "garbage of the layout " + k;
            };
            Thread t = new Thread(() -> {
                try { got[0] = Lib.down(150); } catch (Throwable e) { failed[0] = e; }
            }, "deep-layout");
            t.start();
            atBottom.await();
            System.gc();
            collected.countDown();
            t.join();
            Lib.deepest = null;
            detail = "depth " + got[0] + (failed[0] != null ? " " + failed[0] : "");
            return got[0] == 150 && failed[0] == null;
        });
    }
}
