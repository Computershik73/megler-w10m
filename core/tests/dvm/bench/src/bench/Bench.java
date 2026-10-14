package bench;

import java.util.ArrayList;
import java.util.HashMap;

/**
 * The interpreter's benchmark (core/tests/hk_dvm_bench.c runs it): each static method does `n` times one kind of work and
 * returns something made of it, so that nothing it does is for nothing. The runner times each and counts the instructions it
 * executed. Compiled by javac and d8 (tools/dvm-bench-fixture.sh) into core/tests/dvm/bench/bench.dex.
 */
public final class Bench {
    private Bench() {}

    static final class Point {
        int x, y;
        Point(int x, int y) { this.x = x; this.y = y; }
        int sum() { return x + y; }
    }
    interface Shape { int area(); }
    static final class Square implements Shape {
        final int side;
        Square(int s) { side = s; }
        public int area() { return side * side; }
    }
    static final class Rect implements Shape {
        final int w, h;
        Rect(int w, int h) { this.w = w; this.h = h; }
        public int area() { return w * h; }
    }
    static class Base { int value(int i) { return i + 1; } }
    static final class Derived extends Base { @Override int value(int i) { return i + 2; } }

    static int counter;
    private static final Object LOCK = new Object();
    private static final String TEXT = "The quick brown fox jumps over the lazy dog, 0123456789 times.";
    private static final RuntimeException PREMADE = new RuntimeException("premade");

    /** int arithmetic: add, multiply, shift, xor, and a compare and branch a turn. */
    public static int arith(int n) {
        int s = 1;
        for (int i = 0; i < n; i++) s = (s * 31 + i) ^ (s >>> 7);
        return s;
    }

    /** long and double arithmetic. */
    public static int wide(int n) {
        long a = 1;
        double d = 0.5;
        for (int i = 0; i < n; i++) {
            a = a * 6364136223846793005L + 1442695040888963407L;
            d = d * 0.999 + (a & 0xFF);
        }
        return (int) (a ^ (long) d);
    }

    /** iget and iput on one object. */
    public static int fields(int n) {
        Point p = new Point(1, 2);
        for (int i = 0; i < n; i++) {
            p.x += p.y;
            p.y = p.x & 7;
        }
        return p.x;
    }

    /** sget and sput. */
    public static int statics(int n) {
        for (int i = 0; i < n; i++) counter += i;
        return counter;
    }

    private static int add(int a, int b) { return a + b; }

    /** invoke-static of a small method. */
    public static int invokeStatic(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) s = add(s, i);
        return s;
    }

    /** invoke-virtual, one class at the call site. */
    public static int invokeVirtual(int n) {
        Base b = new Derived();
        int s = 0;
        for (int i = 0; i < n; i++) s += b.value(i);
        return s;
    }

    /** invoke-virtual of a final class's method (the call devirtualises to one target). */
    public static int invokeFinal(int n) {
        Point p = new Point(3, 4);
        int s = 0;
        for (int i = 0; i < n; i++) s += p.sum();
        return s;
    }

    /** invoke-interface, one class at the call site. */
    public static int invokeInterface(int n) {
        Shape sh = new Square(3);
        int s = 0;
        for (int i = 0; i < n; i++) s += sh.area();
        return s;
    }

    /** invoke-interface, two classes taking turns at the call site. */
    public static int invokeInterface2(int n) {
        Shape[] shapes = { new Square(3), new Rect(2, 5) };
        int s = 0;
        for (int i = 0; i < n; i++) s += shapes[i & 1].area();
        return s;
    }

    /** aget and aput over an int[] of 256. */
    public static int arrays(int n) {
        int[] a = new int[256];
        int s = 0;
        for (int i = 0; i < n; i++) {
            int k = i & 255;
            a[k] = a[k] + i;
            s += a[(k + 1) & 255];
        }
        return s;
    }

    /** String.charAt and String.length, a character a turn. */
    public static int charAt(int n) {
        String t = TEXT;
        int s = 0;
        for (int i = 0; i < n; i++) s += t.charAt(i % t.length());
        return s;
    }

    /** String.equals of two strings that are equal and not the same object, 23 characters. */
    public static int equalsStr(int n) {
        String a = "abcdefghijklmnopqrstuvw";
        String b = new String(a.toCharArray());
        int s = 0;
        for (int i = 0; i < n; i++) if (a.equals(b)) s++;
        return s;
    }

    /** String.hashCode, once worked out and then kept, as libcore keeps it. */
    public static int hashCodeStr(int n) {
        String a = TEXT;
        int s = 0;
        for (int i = 0; i < n; i++) s += a.hashCode();
        return s;
    }

    /** String.indexOf of a character near the end. */
    public static int indexOf(int n) {
        String t = TEXT;
        int s = 0;
        for (int i = 0; i < n; i++) s += t.indexOf('9');
        return s;
    }

    /** A StringBuilder: two strings and an int appended, made into a String. */
    public static int stringBuilder(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            StringBuilder sb = new StringBuilder();
            sb.append("item ").append(i).append(" of the list");
            s += sb.toString().length();
        }
        return s;
    }

    /** HashMap.get of an Integer key in a map of 1000, and a put a turn in 16. */
    public static int hashMap(int n) {
        HashMap<Integer, Integer> map = new HashMap<>();
        for (int i = 0; i < 1000; i++) map.put(i, i * 2);
        int s = 0;
        for (int i = 0; i < n; i++) {
            Integer v = map.get(i % 1000);
            if (v != null) s += v;
            if ((i & 15) == 0) map.put(i % 1000, i);
        }
        return s;
    }

    /** An ArrayList of 100 Integers gone through with its iterator, a turn an element. */
    public static int arrayList(int n) {
        ArrayList<Integer> list = new ArrayList<>();
        for (int i = 0; i < 100; i++) list.add(i);
        int s = 0, done = 0;
        while (done < n) {
            for (Integer v : list) s += v;
            done += 100;
        }
        return s;
    }

    /** A small object made a turn, two fields. */
    public static int alloc(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            Point p = new Point(i, 1);
            s += p.x;
        }
        return s;
    }

    /** An int[16] made a turn. */
    public static int allocArray(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            int[] a = new int[16];
            s += a.length;
        }
        return s;
    }

    /** A synchronized block on one lock no other thread wants. */
    public static int sync(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            synchronized (LOCK) { s += i; }
        }
        return s;
    }

    /** A synchronized method, the same. */
    private static synchronized int syncAdd(int a, int b) { return a + b; }
    public static int syncMethod(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) s = syncAdd(s, i);
        return s;
    }

    /** An exception made, thrown and caught two frames up. */
    private static void thrower(int i) { throw new IllegalStateException("no"); }
    public static int exceptions(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            try { thrower(i); } catch (IllegalStateException e) { s++; }
        }
        return s;
    }

    /** One made in advance, thrown and caught in the same method. */
    public static int throwPremade(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) {
            try { throw PREMADE; } catch (RuntimeException e) { s++; }
        }
        return s;
    }

    /** Math.min, Math.max and Math.abs. */
    public static int math(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) s += Math.max(Math.min(i, 1000), -Math.abs(s & 255));
        return s;
    }

    /** System.arraycopy of 16 ints. */
    public static int arraycopy(int n) {
        int[] a = new int[64], b = new int[64];
        for (int i = 0; i < n; i++) System.arraycopy(a, i & 31, b, 16, 16);
        return b[20];
    }

    /** Math.sin: a native method of libcore's own libraries, called through JNI where they run (the phone, qemu). */
    public static int nativeSin(int n) {
        double s = 0;
        for (int i = 0; i < n; i++) s += Math.sin(i & 1023);
        return (int) s;
    }

    /** Integer.valueOf of small and of larger numbers, and intValue. */
    public static int boxing(int n) {
        int s = 0;
        for (int i = 0; i < n; i++) s += Integer.valueOf(i & 1023).intValue();
        return s;
    }

    /** Object.getClass and a check-cast and instance-of. */
    public static int classes(int n) {
        Object o = new Square(2);
        int s = 0;
        for (int i = 0; i < n; i++) {
            if (o.getClass() == Square.class) s++;
            if (o instanceof Shape) s += ((Shape) o).area();
        }
        return s;
    }
}
