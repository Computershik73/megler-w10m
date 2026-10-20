package meglet.test.natives;

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
    /** The system's ICU opened by name and a function of each of its two libraries found by its versioned name: the version, or below 1. */
    static native int icuVersion();
    /** A system property as native code reads it: "" when there is none. */
    static native String systemProperty(String name);
    static native String allocatedString(byte[] bytes, char[] chars);
    /** Objects held in a native method's locals and by a global reference while System.gc() runs: 1 when all read as they were. */
    static native int holdAcrossGc();

    /** Declared and in no library. */
    static native void notThere();
}
