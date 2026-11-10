package megler.test.natives;

import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.os.Looper;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.nio.ByteBuffer;

/**
 * The class whose native methods are in libnatives.so (jni/natives.c). The library is loaded where an app loads its own: in the
 * static initialiser of the class that needs it. Its JNI_OnLoad registers the first three methods; the others are found by
 * their Java_ names when first called.
 */
public class Lib {
    /** Written by JNI_OnLoad: how many times it has run, and the class FindClass gave it for this one's name. */
    static int onLoadCalls;
    static Class<?> onLoadClass;
    static long counter;
    static Object[] reflected = new Object[1];
    int factor;
    String label;

    static {
        System.loadLibrary("natives");
    }

    public Lib(int factor) {
        this.factor = factor;
    }

    interface Callback {
        int onValue(int a, long b, double c, String s);
    }

    /** What a thread of the library's own says of itself once it has attached. */
    public static class ThreadReport {
        int before = 99, rc = 99, after = 99;
        boolean app, string, kept, daemon, called;
        String name, group;

        public void onThread(int before, int rc, int after, boolean app, boolean string, boolean kept) {
            this.before = before;
            this.rc = rc;
            this.after = after;
            this.app = app;
            this.string = string;
            this.kept = kept;
            Thread t = Thread.currentThread();
            name = t.getName();
            daemon = t.isDaemon();
            group = t.getThreadGroup() == null ? null : t.getThreadGroup().getName();
            called = true;
        }
    }

    public static class LooperReport {
        public void onLooper(int value, int events) {
            Main.say("looper", value == 'L' && events == 1 && Looper.myLooper() == Looper.getMainLooper(), "value=" + value + " events=" + events);
        }
    }

    /** What the library's sensor queue heard (watchSensors), told from its callback on the main thread's looper. */
    public static class SensorReport {
        public void onSensors(boolean ok, String detail) {
            Main.say("sensors-events", ok && Looper.myLooper() == Looper.getMainLooper(), detail);
        }
    }

    public static class Failure extends RuntimeException {
        final int code;

        public Failure(int code) {
            super("failure " + code);
            this.code = code;
        }
    }

    static String staticBack(String s, float f) {
        return s + "/" + f;
    }

    /* Called back from native code with 78 arguments, 45 ints and 33 objects, as WhatsApp's native code calls JniBridge.jnidispatch. */
    static int manyBack(int i0, int i1, int i2, int i3, int i4, int i5, int i6, int i7, int i8, int i9, int i10, int i11, int i12, int i13, int i14, int i15, int i16, int i17, int i18, int i19, int i20, int i21, int i22, int i23, int i24, int i25, int i26, int i27, int i28, int i29, int i30, int i31, int i32, int i33, int i34, int i35, int i36, int i37, int i38, int i39, int i40, int i41, int i42, int i43, int i44, Object o0, Object o1, Object o2, Object o3, Object o4, Object o5, Object o6, Object o7, Object o8, Object o9, Object o10, Object o11, Object o12, Object o13, Object o14, Object o15, Object o16, Object o17, Object o18, Object o19, Object o20, Object o21, Object o22, Object o23, Object o24, Object o25, Object o26, Object o27, Object o28, Object o29, Object o30, Object o31, Object o32) {
        int sum = 1 * i0 + 2 * i1 + 3 * i2 + 4 * i3 + 5 * i4 + 6 * i5 + 7 * i6 + 8 * i7 + 9 * i8 + 10 * i9 + 11 * i10 + 12 * i11 + 13 * i12 + 14 * i13 + 15 * i14 + 16 * i15 + 17 * i16 + 18 * i17 + 19 * i18 + 20 * i19 + 21 * i20 + 22 * i21 + 23 * i22 + 24 * i23 + 25 * i24 + 26 * i25 + 27 * i26 + 28 * i27 + 29 * i28 + 30 * i29 + 31 * i30 + 32 * i31 + 33 * i32 + 34 * i33 + 35 * i34 + 36 * i35 + 37 * i36 + 38 * i37 + 39 * i38 + 40 * i39 + 41 * i40 + 42 * i41 + 43 * i42 + 44 * i43 + 45 * i44;
        int named = 0;
        if (("o0").equals(o0)) named++;
        if (("o1").equals(o1)) named++;
        if (("o2").equals(o2)) named++;
        if (("o3").equals(o3)) named++;
        if (("o4").equals(o4)) named++;
        if (("o5").equals(o5)) named++;
        if (("o6").equals(o6)) named++;
        if (("o7").equals(o7)) named++;
        if (("o8").equals(o8)) named++;
        if (("o9").equals(o9)) named++;
        if (("o10").equals(o10)) named++;
        if (("o11").equals(o11)) named++;
        if (("o12").equals(o12)) named++;
        if (("o13").equals(o13)) named++;
        if (("o14").equals(o14)) named++;
        if (("o15").equals(o15)) named++;
        if (("o16").equals(o16)) named++;
        if (("o17").equals(o17)) named++;
        if (("o18").equals(o18)) named++;
        if (("o19").equals(o19)) named++;
        if (("o20").equals(o20)) named++;
        if (("o21").equals(o21)) named++;
        if (("o22").equals(o22)) named++;
        if (("o23").equals(o23)) named++;
        if (("o24").equals(o24)) named++;
        if (("o25").equals(o25)) named++;
        if (("o26").equals(o26)) named++;
        if (("o27").equals(o27)) named++;
        if (("o28").equals(o28)) named++;
        if (("o29").equals(o29)) named++;
        if (("o30").equals(o30)) named++;
        if (("o31").equals(o31)) named++;
        if (("o32").equals(o32)) named++;
        return sum * 100 + named;
    }

    static native int manyArgs(Object[] names);

    /* A layout n levels deep, each measured by native code that calls back here for the next (pingPong), with three Java calls between; deepest run at the bottom. */
    static native int pingPong(int n);
    static Runnable deepest;
    static int down(int n) {
        if (n > 0) return measure(n);
        if (deepest != null) deepest.run();
        return 0;
    }
    private static int measure(int n) { return 1 + child(n); }
    private static int child(int n) { return pingPong(n); }

    /* Registered by JNI_OnLoad. */
    static native int addInts(int a, int b);
    native int instanceTimes(int k);
    static native String registeredHow();

    /* Found by name. */
    static native boolean flip(boolean v);
    static native byte nextByte(byte v);
    static native char nextChar(char v);
    static native short nextShort(short v);
    static native int nextInt(int v);
    static native long nextLong(long v);
    static native float half(float v);
    static native double twice(double v);
    static native double mix(int a, long b, double c, int d, long e, float f, double g, long h, int i, double j);
    native long mixLong(long a, int b, long c, byte d, long e, short f, char g, boolean h);
    static native int over(int v);
    static native int over(String s);

    static native int sum(int[] a);
    static native void fill(byte[] a, int from, int count, byte v);
    static native long[] squares(int n);
    static native double sumCritical(double[] a);
    static native int others(boolean[] z, char[] c, short[] s, float[] f);
    static native String[] words(String sentence);
    static native int total(String[] strings);
    static native int bounds(int[] ints, Object[] objects, String s);

    static native String greet(String name);
    static native String reverse(String s);
    static native String region(String s, int start, int len, int[] utf8Bytes);

    static native int callBack(Callback callback, int v);
    static native Lib make(int factor, String label);

    static native void throwIt(String message);
    static native void throwOwn(int code);
    static native String catchIt(Runnable runnable);
    static native String find(String jniName);

    static native int refs(Object o);
    static native Object reflect(Method method, Field field, int[] out);
    static native boolean monitor(Object o);

    static native boolean startThread(ThreadReport report, String name, boolean daemon);
    static native int afterDetach();

    static native long dep(long v);

    static native String readAsset(AssetManager assets, String name);
    static native String paint(Bitmap bitmap);
    static native long bufferSum(ByteBuffer buffer);
    static native ByteBuffer sharedBuffer();
    static native int sharedAt(int i);
    static native String configuration(AssetManager assets);
    static native int watchLooper(LooperReport report);
    static native int watchSensors(SensorReport report);
    /** The system's ICU opened by name and a function of each of its two libraries found by its versioned name: the version, or below 1. */
    static native int icuVersion();
    /** A copy of libnatives_dep.so dlopen'd by its path: its natives_dep_answer (4242, its constructor run), or -1 when it did not open. */
    static native int dlopenAnswer(String path);
    /** A system property as native code reads it: "" when there is none. */
    static native String systemProperty(String name);
    static native String allocatedString(byte[] bytes, char[] chars);
    /** Objects held in a native method's locals and by a global reference while System.gc() runs: 1 when all read as they were. */
    static native int holdAcrossGc();

    /** Declared and in no library. */
    static native void notThere();
}
